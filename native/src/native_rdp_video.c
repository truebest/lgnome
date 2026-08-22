/* RDP graphics callbacks: RemoteFX bitmap presentation and H.264 AU ingest. */

#include "native_rdp_callback_parts.h"

#include <stdatomic.h>

#include "h264_annexb.h"
#include "native_app.h"
#include "native_rdp_media.h"
#include "native_rdp_state.h"
#include "native_rdp_video.h"
#include "native_rgba_owner.h"

#include "clog.h"

clog_define(g_native_log_rdp_video, cLogLevelInfo, cLogFlags_Default, "native", NULL);

static void on_bitmap_update(void *ctx, uint16_t surface_id, uint32_t left, uint32_t top, uint32_t width, uint32_t height,
                             uint32_t stride, const uint8_t *rgba, size_t len) {
    NativeSessionSlot *slot = (NativeSessionSlot *)ctx;
    if (!slot) {
        return;
    }
    App *app = slot->app;
    /* Classify the graphics path BEFORE the background early-return: a backgrounded
     * stream (e.g. fresh from a hidden reconnect) must still teach the switch logic
     * that it renders via bitmaps and cannot be snapshot-switched. */
    atomic_store(&slot->video_via_bitmap, true);
    if (!native_slot_is_active(slot)) {
        /* Background session: only the active slot may touch the shared presentation. */
        return;
    }
    if (!rgba || len == 0 || width == 0 || height == 0) {
        app->decoder_errors++;
        native_slot_report_terminal(slot, RDP_STATE_DECODER_ERROR,
                                    rdp_state_exit_code(RDP_STATE_DECODER_ERROR));
        return;
    }
    uint16_t desktop_width = (uint16_t)atomic_load(&slot->desktop_width);
    uint16_t desktop_height = (uint16_t)atomic_load(&slot->desktop_height);

    pthread_mutex_lock(&app->video_lock);
    if (!native_slot_is_active(slot)) {
        /* Demoted between the gate above and the lock: a switch tore the video path down
         * meanwhile, and this in-flight update must not resurrect it. */
        pthread_mutex_unlock(&app->video_lock);
        return;
    }
    unsigned owner_epoch = atomic_load(&slot->connect_epoch);
    if (app->rgba && !native_rgba_owner_matches(app, slot->index, owner_epoch)) {
        /* A dirty rectangle can never be applied to another slot or connection
         * generation's pixels. Keep any frozen HUB return surface separate and start
         * this owner on a zeroed canvas. */
        native_close_rgba_locked(app, true);
    }
    if (app->video) {
        native_video_close(app->video);
        app->video = NULL;
        app->decoder_keyframe_pending = false;
        clog(cLogLevelNotice, "switching graphics path from NDL/H.264 to native RemoteFX RGBA");
    }
    if (!app->rgba) {
        app->rgba = native_rgba_surface_open(desktop_width, desktop_height);
        if (app->rgba) {
            app->rgba_owner_slot = slot->index;
            app->rgba_owner_epoch = owner_epoch;
        }
    } else if (native_rgba_surface_width(app->rgba) != desktop_width ||
               native_rgba_surface_height(app->rgba) != desktop_height) {
#ifdef HELLOLG_TARGET_WEBOS
        native_defer_rgba_texture_destroy(app);
#endif
        if (native_rgba_surface_resize(app->rgba, desktop_width, desktop_height) != NATIVE_RGBA_OK) {
            native_rgba_surface_close(app->rgba);
            app->rgba = NULL;
            native_reset_rgba_owner(app);
        }
    }
    NativeRgbaResult result = app->rgba ? native_rgba_surface_apply(app->rgba, left, top, width, height, stride, rgba, len)
                                        : NATIVE_RGBA_NOMEM;
    pthread_mutex_unlock(&app->video_lock);

    if (result == NATIVE_RGBA_OK) {
        /* Bitmap frames satisfy the post-switch watchdog too, or a switch to a
         * RemoteFX-only server would reconnect despite frames arriving. */
        atomic_fetch_add(&slot->video_ok_frames, 1u);
    }
    if (result != NATIVE_RGBA_OK) {
        app->decoder_errors++;
        clog(cLogLevelError,
             "terminal native error: DecoderError; RemoteFX bitmap update failed result=%d surface=%u "
             "rect=%ux%u+%u+%u",
             (int)result, (unsigned)surface_id, (unsigned)width, (unsigned)height, (unsigned)left,
             (unsigned)top);
        native_slot_report_terminal(slot, RDP_STATE_DECODER_ERROR,
                                    rdp_state_exit_code(RDP_STATE_DECODER_ERROR));
    }
}

/* The single compressed-video ingest path: worker callbacks land here, and the
 * switch machinery replays snapshot AUs through it so every AU takes the same
 * classification/decoder path. Decoder-seed classification happens before routing or
 * decoder-ownership gates, so both paths use one native dual-framing verdict. */
void native_video_ingest_au(NativeSessionSlot *slot, const uint8_t *data, size_t len, uint64_t pts90k) {
    if (!slot) {
        return;
    }
    App *app = slot->app;
    NativeH264Info h264_info;
    bool valid_au = native_h264_scan_au(data, len, &h264_info) == NATIVE_H264_OK;
    bool decoder_seed = valid_au && h264_info.can_seed_decoder;
    /* This stream feeds H.264 AUs: snapshot switching applies. Stamped before any
     * routing so a backgrounded stream classifies its slot too. */
    atomic_store(&slot->video_via_bitmap, false);
    if (!native_slot_is_active(slot)) {
        /* Background session: normally the server has been asked to suppress graphics —
         * this also covers the in-flight tail and servers that ignore
         * TS_SUPPRESS_OUTPUT_PDU. With the AU snapshot armed (hidden reconnect for a
         * cacheable decoder seed) the compressed bytes are kept for replay on switch-to;
         * without it they are dropped outright. */
        pthread_mutex_lock(&app->video_lock);
        if (native_slot_is_active(slot)) {
            /* Promoted while we waited on the lock (a switch just took/cleared the
             * snapshot): this AU belongs to the LIVE stream now — fall through to the
             * active path below instead of silently losing it. */
            pthread_mutex_unlock(&app->video_lock);
        } else {
            if (slot->snapshot.armed) {
                if (!native_au_snapshot_append(&slot->snapshot, data, len, valid_au, decoder_seed,
                                               pts90k)) {
                    clog(cLogLevelWarning,
                         "%s AU snapshot invalidated (overflow or malformed AU); switch-to will reconnect",
                         native_session_slot_name(slot->index));
                }
                /* The quiet-gate timestamp must be public BEFORE readiness: a reader
                 * seeing ready=true with the previous (stale) timestamp could judge the
                 * stream quiet at the very moment this AU is landing. */
                atomic_store(&slot->snapshot_last_au_ms, native_monotonic_ms());
                atomic_store(&slot->snapshot_idr_ready, native_au_snapshot_ready(&slot->snapshot));
            }
            pthread_mutex_unlock(&app->video_lock);
            return;
        }
    }
    if (!data || len == 0) {
        app->decoder_errors++;
        native_slot_report_terminal(slot, RDP_STATE_DECODER_ERROR,
                                    rdp_state_exit_code(RDP_STATE_DECODER_ERROR));
        return;
    }

    /* The slot's desktop size reflects the server's real EGFX graphics output size
     * (on_desktop_size is re-invoked on every RDPGFX_RESET_GRAPHICS_PDU, not just the
     * initial MCS/GCC handshake), which can differ from the negotiated session size, e.g.
     * a TV whose hardware decoder always runs at panel resolution. Reopen the hardware decoder
     * whenever that size changes so it matches what the server actually encodes. */
    uint16_t desktop_width = (uint16_t)atomic_load(&slot->desktop_width);
    uint16_t desktop_height = (uint16_t)atomic_load(&slot->desktop_height);
    pthread_mutex_lock(&app->video_lock);
    if (!native_slot_is_active(slot)) {
        /* Demoted between the gate above and the lock (see on_bitmap_update). */
        pthread_mutex_unlock(&app->video_lock);
        return;
    }
    if (app->rgba) {
        native_close_rgba_locked(app, true);
        clog(cLogLevelNotice, "switching graphics path from native RemoteFX RGBA to NDL/H.264");
    }
    if (app->video && (app->video_owner_slot != slot->index ||
                       app->video_owner_epoch != atomic_load(&slot->connect_epoch))) {
        /* The decoder still holds another stream's state (a switch left the old picture
         * up on purpose). Swap only once THIS stream's keyframe is in hand: deltas fed
         * into foreign decoder state would be garbage, and closing earlier would just
         * black the screen for the whole handover. */
        if (!decoder_seed) {
            pthread_mutex_unlock(&app->video_lock);
            unsigned waited = atomic_fetch_add(&slot->keyframe_wait_drops, 1u) + 1u;
            if (waited % 60u == 1u) {
                clog(cLogLevelDebug,
                     "waiting for a %s keyframe; %u delta AUs dropped since the switch",
                     native_session_slot_name(slot->index), waited);
            }
            return;
        }
        atomic_store(&slot->keyframe_wait_drops, 0u);
        if (desktop_width == native_video_width(app->video) &&
            desktop_height == native_video_height(app->video)) {
            /* Same dimensions: hand the running decoder over in-band. gnome-remote-desktop
             * sends SPS+PPS with every IDR, which restarts H.264 decode cleanly — no
             * pipeline reload, no black gap, and the mixed audio track is never touched. */
            clog(cLogLevelNotice, "handing the video decoder to the %s session in-band (%ux%u)",
                 native_session_slot_name(slot->index), (unsigned)desktop_width,
                 (unsigned)desktop_height);
            app->video_owner_slot = slot->index;
            app->video_owner_epoch = atomic_load(&slot->connect_epoch);
            app->decoder_keyframe_pending = false;
        } else {
            clog(cLogLevelNotice,
                 "swapping video to the %s session (keyframe in hand, %ux%u -> %ux%u)",
                 native_session_slot_name(slot->index), (unsigned)native_video_width(app->video),
                 (unsigned)native_video_height(app->video), (unsigned)desktop_width,
                 (unsigned)desktop_height);
            native_video_close(app->video);
            app->video = NULL;
            app->decoder_keyframe_pending = false;
        }
    }
    if (app->video &&
        (desktop_width != native_video_width(app->video) || desktop_height != native_video_height(app->video))) {
        clog(cLogLevelNotice, "hardware surface size changed %ux%u -> %ux%u; reopening decoder",
             (unsigned)native_video_width(app->video), (unsigned)native_video_height(app->video),
             (unsigned)desktop_width, (unsigned)desktop_height);
        native_video_close(app->video);
        app->video = NULL;
        app->decoder_keyframe_pending = false;
    }
    if (!app->video) {
        NativeMedia *media = native_ensure_media_locked(app);
        if (media) {
            /* Audio must attach before the video track: the video open below triggers
             * the single pipeline load with the keyframe already in hand, whereas an
             * audio open arriving later would reload the pipeline mid-stream. */
            native_open_speculative_audio_locked(app);
            app->video = native_video_open(media, desktop_width, desktop_height, slot->config.fps);
            if (app->video) {
                app->video_owner_slot = slot->index;
                app->video_owner_epoch = atomic_load(&slot->connect_epoch);
            }
        }
        if (!app->video) {
            pthread_mutex_unlock(&app->video_lock);
            app->decoder_errors++;
            clog(cLogLevelError,
                 "terminal native error: DecoderError; hardware decoder unavailable");
            native_slot_report_terminal(slot, RDP_STATE_DECODER_ERROR,
                                        rdp_state_exit_code(RDP_STATE_DECODER_ERROR));
            return;
        }
    }

    /* The callback byte lifetime is synchronous: do not retain data beyond native_video_feed.
     * If this callback later queues AUs to another thread, copy the bytes before returning.
     */
    NativeVideoResult result = native_video_feed(app->video, data, len, pts90k);
    if (result == NATIVE_VIDEO_NEED_KEYFRAME) {
        /* Published while still holding video_lock and only for the slot that is active
         * RIGHT NOW: a switch flips active_index (and clears this flag) under the same
         * lock, so a feed racing the switch cannot leave a stale request behind — the
         * tick would send that refresh to the fresh target and arm its watchdog into a
         * needless reconnect on a static desktop. */
        if (slot->index == atomic_load(&app->active_index) && !app->decoder_keyframe_pending) {
            clog(cLogLevelWarning,
                 "decoder requested keyframe/recovery; waiting for native RDP recovery, no web fallback");
            app->decoder_keyframe_pending = true;
            /* Kick the SDL thread: refresh-capable servers deliver an IDR, refresh-
             * ineffective ones fall to the keyframe watchdog's reconnect. Without this a
             * broken reference chain (e.g. a stale snapshot replay) would freeze the
             * stream forever — grd never resends an IDR on its own. */
            atomic_store(&app->video_refresh_needed, true);
        }
        pthread_mutex_unlock(&app->video_lock);
        return;
    }
    pthread_mutex_unlock(&app->video_lock);
    if (result == NATIVE_VIDEO_OK) {
        app->decoder_keyframe_pending = false;
        /* Signals the SDL thread that the freshly switched-to stream is decoding. */
        atomic_fetch_add(&slot->video_ok_frames, 1u);
        return;
    }
    if (result == NATIVE_VIDEO_DROPPED) {
        /* Discarded by a still-loading decoder: not progress (video_ok_frames must not
         * move — the snapshot-replay success check and the switch watchdog read it as
         * "frames actually consumed") but not an error either. */
        return;
    }

    app->decoder_errors++;
    clog(cLogLevelError, "terminal native error: DecoderError; decoder feed result=%d", (int)result);
    native_slot_report_terminal(slot, RDP_STATE_DECODER_ERROR,
                                rdp_state_exit_code(RDP_STATE_DECODER_ERROR));
}

static void on_video_au(void *ctx, const uint8_t *data, size_t len, uint64_t pts90k) {
    native_video_ingest_au((NativeSessionSlot *)ctx, data, len, pts90k);
}

void native_rdp_install_video_callbacks(RdpCallbacks *callbacks) {
    callbacks->on_bitmap_update = on_bitmap_update;
    callbacks->on_video_au = on_video_au;
}
