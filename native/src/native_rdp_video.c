/* RDP graphics callbacks: RemoteFX bitmap presentation and H.264 AU ingest. */

#include "native_rdp_callback_parts.h"

#include <stdatomic.h>

#include "h264_annexb.h"
#include "native_app.h"
#include "native_rdp_media.h"
#include "native_rdp_state.h"
#include "native_rdp_video.h"
#include "native_rgba_owner.h"
#include "native_time.h"

#include "clog.h"

clog_define(g_native_log_rdp_video, cLogLevelInfo, "native");

/* Bound decoder rebuilds caused by codec interleaving within one connection. */
#define NATIVE_MAX_GRAPHICS_PATH_SWITCHES 4

/* Periodic decoder-input statistics. */
#define NATIVE_VIDEO_PACING_LOG_MS 5000u

/* Hold hardware video through bitmap refinements from the same owner. */
#define NATIVE_VIDEO_PATH_HOLD_MS 1500u

/* Report one in this many discarded refinements. */
#define NATIVE_TILE_DROP_LOG_INTERVAL 500u

/* Flag a slow synchronous decoder call, not an idle remote desktop. */
#define NATIVE_VIDEO_SLOW_FEED_MS 150u

static void native_note_video_frame(App *app, NativeSessionSlot *slot, size_t len, uint32_t now_ms) {
    app->video_frames_window++;
    app->video_bytes_window += len;
    if (!app->video_pacing_started) {
        app->video_pacing_window_ms = now_ms;
        app->video_pacing_started = true;
        return;
    }
    uint32_t elapsed = now_ms - app->video_pacing_window_ms;
    if (elapsed < NATIVE_VIDEO_PACING_LOG_MS) {
        return;
    }
    clog(cLogLevelInfo,
         "video: %u accepted AUs in %ums (%llu kbit/s, %.1f AU/s); shared decoder input, last_slot=%s, not displayed FPS",
         app->video_frames_window, (unsigned)elapsed,
         (unsigned long long)(app->video_bytes_window * 8ull / elapsed),
         (double)app->video_frames_window * 1000.0 / (double)elapsed, native_session_slot_name(slot->index));
    app->video_frames_window = 0;
    app->video_bytes_window = 0;
    app->video_pacing_window_ms = now_ms;
}

/* Only same-owner transitions reach this counter. */
static bool native_note_graphics_path_switch(App *app, NativeSessionSlot *slot, const char *direction) {
    slot->graphics_path_switches++;
    clog(cLogLevelNotice, "switching graphics path %s", direction);
    if (slot->graphics_path_switches <= NATIVE_MAX_GRAPHICS_PATH_SWITCHES) {
        return false;
    }
    clog(cLogLevelError, "server interleaves H.264 and bitmap updates; neither path can present it");
    app->decoder_errors++;
    native_slot_report_terminal(slot, RDP_STATE_DECODER_ERROR, rdp_state_exit_code(RDP_STATE_DECODER_ERROR));
    return true;
}

static void on_bitmap_update(void *ctx, uint16_t surface_id, uint32_t left, uint32_t top, uint32_t width, uint32_t height,
                             uint32_t stride, const uint8_t *rgba, size_t len) {
    NativeSessionSlot *slot = (NativeSessionSlot *)ctx;
    if (!slot) {
        return;
    }
    App *app = slot->app;
    pthread_mutex_lock(&app->video_lock);
    bool recent_video = slot->video_seen && slot->video_epoch == atomic_load(&slot->connect_epoch) &&
                       native_monotonic_ms() - slot->video_last_au_ms < NATIVE_VIDEO_PATH_HOLD_MS;
    if (!native_slot_is_active(slot) || (slot->snapshot.armed && recent_video)) {
        atomic_store(&slot->video_via_bitmap, !recent_video);
        pthread_mutex_unlock(&app->video_lock);
        return;
    }
    atomic_store(&slot->video_via_bitmap, true);
    if (!rgba || len == 0 || width == 0 || height == 0) {
        app->decoder_errors++;
        native_slot_report_terminal(slot, RDP_STATE_DECODER_ERROR,
                                    rdp_state_exit_code(RDP_STATE_DECODER_ERROR));
        pthread_mutex_unlock(&app->video_lock);
        return;
    }
    uint16_t desktop_width = (uint16_t)atomic_load(&slot->desktop_width);
    uint16_t desktop_height = (uint16_t)atomic_load(&slot->desktop_height);

    unsigned owner_epoch = atomic_load(&slot->connect_epoch);
    if (app->rgba && !native_rgba_owner_matches(app, slot->index, owner_epoch)) {
        /* Dirty rectangles must not inherit another slot/epoch's pixels. */
        native_close_rgba_locked(app, true);
    }
    if (app->video) {
        bool same_owner = app->video_owner_slot == slot->index && app->video_owner_epoch == owner_epoch;
        uint32_t since_feed = native_monotonic_ms() - app->video_last_feed_ms;
        if (same_owner && app->video_feed_started && since_feed < NATIVE_VIDEO_PATH_HOLD_MS) {
            /* Only the matching decoder owner may hold bitmap refinements behind live hardware video. */
            atomic_store(&slot->video_via_bitmap, false);
            if (++app->tiles_dropped_under_video % NATIVE_TILE_DROP_LOG_INTERVAL == 0) {
                clog(cLogLevelDebug, "video: %u refinement tiles dropped while hardware video is live",
                     app->tiles_dropped_under_video);
            }
            pthread_mutex_unlock(&app->video_lock);
            return;
        }
        native_video_close(app->video);
        app->video = NULL;
        app->decoder_keyframe_pending = false;
        if (same_owner && native_note_graphics_path_switch(app, slot, "from NDL video to native RemoteFX RGBA")) {
            pthread_mutex_unlock(&app->video_lock);
            return;
        }
    }
    if (!app->rgba) {
        app->rgba = native_rgba_surface_open(desktop_width, desktop_height);
        if (app->rgba) {
            app->rgba_owner_slot = slot->index;
            app->rgba_owner_epoch = owner_epoch;
        }
    } else if (native_rgba_surface_width(app->rgba) != desktop_width ||
               native_rgba_surface_height(app->rgba) != desktop_height) {
#ifdef LGNOME_TARGET_WEBOS
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

static void native_note_video_stream(NativeSessionSlot *slot, bool valid_au) {
    if (valid_au) {
        slot->video_seen = true;
        slot->video_last_au_ms = native_monotonic_ms();
        slot->video_epoch = atomic_load(&slot->connect_epoch);
    }
}

/* Shared ingest for live callbacks and snapshot replay. */
void native_video_ingest_au_locked(NativeSessionSlot *slot, const uint8_t *data, size_t len, uint64_t pts90k) {
    if (!slot) {
        return;
    }
    App *app = slot->app;
    bool valid_au;
    bool decoder_seed;
    if (slot->video_codec == NATIVE_VIDEO_CODEC_H265) {
        NativeHevcInfo info;
        valid_au = native_hevc_scan_annexb(data, len, &info) == NATIVE_HEVC_OK;
        decoder_seed = valid_au && info.can_seed_decoder;
    } else {
        NativeH264Info info;
        valid_au = native_h264_scan_au(data, len, &info) == NATIVE_H264_OK;
        decoder_seed = valid_au && info.can_seed_decoder;
    }
    /* This stream feeds compressed AUs: snapshot switching applies. Stamped before any
     * routing so a backgrounded stream classifies its slot too. */
    atomic_store(&slot->video_via_bitmap, false);
    native_note_video_stream(slot, valid_au);
    /* Armed caches remain the handoff barrier after active_index is published. */
    if (slot->snapshot.armed) {
        if (!native_au_snapshot_append(&slot->snapshot, data, len, valid_au, decoder_seed, pts90k)) {
            clog(cLogLevelWarning,
                 "%s AU snapshot invalidated (overflow or malformed AU); switch-to will reconnect",
                 native_session_slot_name(slot->index));
        }
        atomic_store(&slot->snapshot_last_au_ms, native_monotonic_ms());
        atomic_store(&slot->snapshot_idr_ready, native_au_snapshot_ready(&slot->snapshot));
        return;
    }
    if (!native_slot_is_active(slot)) {
        return;
    }
    if (!data || len == 0) {
        app->decoder_errors++;
        native_slot_report_terminal(slot, RDP_STATE_DECODER_ERROR,
                                    rdp_state_exit_code(RDP_STATE_DECODER_ERROR));
        return;
    }

    /* Use the latest ResetGraphics size, which may differ from the connection handshake. */
    uint16_t desktop_width = (uint16_t)atomic_load(&slot->desktop_width);
    uint16_t desktop_height = (uint16_t)atomic_load(&slot->desktop_height);
    if (app->rgba) {
        bool same_owner = native_rgba_owner_matches(app, slot->index, atomic_load(&slot->connect_epoch));
        native_close_rgba_locked(app, true);
        if (same_owner && native_note_graphics_path_switch(app, slot, "from native RemoteFX RGBA to NDL video")) {
            return;
        }
    }
    if (app->video && (app->video_owner_slot != slot->index ||
                       app->video_owner_epoch != atomic_load(&slot->connect_epoch) ||
                       native_video_codec(app->video) != slot->video_codec)) {
        /* Keep the old picture until the new decoder owner supplies a keyframe. */
        if (!decoder_seed) {
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
            desktop_height == native_video_height(app->video) &&
            slot->video_codec == native_video_codec(app->video)) {
            /* A decoder seed permits in-band handoff only within the same codec. */
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
            /* Attach audio before video to load both tracks once, at the keyframe. */
            native_open_speculative_audio_locked(app);
            app->video = native_video_open(media, desktop_width, desktop_height, slot->config.fps, slot->video_codec);
            if (app->video) {
                clog(cLogLevelInfo,
                     "%s video decoder opened: NDL %s %ux%u epoch=%u AU=%zu bytes seed=%d",
                     native_session_slot_name(slot->index),
                     slot->video_codec == NATIVE_VIDEO_CODEC_H265 ? "HEVC" : "H.264",
                     (unsigned)desktop_width, (unsigned)desktop_height,
                     atomic_load(&slot->connect_epoch), len, decoder_seed);
                app->video_owner_slot = slot->index;
                app->video_owner_epoch = atomic_load(&slot->connect_epoch);
            }
        }
        if (!app->video) {
            app->decoder_errors++;
            clog(cLogLevelError,
                 "terminal native error: DecoderError; hardware decoder unavailable");
            native_slot_report_terminal(slot, RDP_STATE_DECODER_ERROR,
                                        rdp_state_exit_code(RDP_STATE_DECODER_ERROR));
            return;
        }
    }

    /* data is valid only for this synchronous callback. */
    uint32_t feed_started_ms = native_monotonic_ms();
    app->video_last_feed_ms = feed_started_ms;
    app->video_feed_started = true;
    NativeVideoResult result = native_video_feed(app->video, data, len, pts90k);
    uint32_t feed_finished_ms = native_monotonic_ms();
    uint32_t feed_ms = feed_finished_ms - feed_started_ms;
    if (feed_ms >= NATIVE_VIDEO_SLOW_FEED_MS) {
        clog_limited(cLogLevelWarning, 2, NATIVE_VIDEO_PACING_LOG_MS,
                     "video: decoder feed blocked for %ums", (unsigned)feed_ms);
    }
    if (result == NATIVE_VIDEO_NEED_KEYFRAME) {
        /* Publish under video_lock so a switch cannot inherit the old owner's refresh request. */
        if (slot->index == atomic_load(&app->active_index) && !app->decoder_keyframe_pending) {
            clog(cLogLevelWarning,
                 "decoder requested keyframe/recovery; waiting for native RDP recovery, no web fallback");
            app->decoder_keyframe_pending = true;
            atomic_store(&app->video_refresh_needed, true);
        }
        return;
    }
    if (result == NATIVE_VIDEO_OK) {
        app->decoder_keyframe_pending = false;
        /* Signals the SDL thread that the freshly switched-to stream is decoding. */
        atomic_fetch_add(&slot->video_ok_frames, 1u);
        native_note_video_frame(app, slot, len, feed_finished_ms);
        return;
    }
    if (result == NATIVE_VIDEO_DROPPED) {
        /* DROPPED does not count as decoded progress for replay or the switch watchdog. */
        return;
    }

    app->decoder_errors++;
    clog(cLogLevelError, "terminal native error: DecoderError; decoder feed result=%d", (int)result);
    native_slot_report_terminal(slot, RDP_STATE_DECODER_ERROR,
                                rdp_state_exit_code(RDP_STATE_DECODER_ERROR));
}

void native_video_ingest_au(NativeSessionSlot *slot, NativeVideoCodec codec, const uint8_t *data, size_t len,
                           uint64_t pts90k) {
    if (!slot) {
        return;
    }
    pthread_mutex_lock(&slot->app->video_lock);
    if (slot->video_codec != codec) {
        /* A cached delta chain belongs to one codec, even within the same connection. */
        if (slot->snapshot.armed) {
            (void)native_au_snapshot_arm(&slot->snapshot);
        }
        atomic_store(&slot->snapshot_idr_ready, false);
        slot->video_codec = codec;
        slot->video_seen = false;
    }
    native_video_ingest_au_locked(slot, data, len, pts90k);
    pthread_mutex_unlock(&slot->app->video_lock);
}

static void on_video_au(void *ctx, uint32_t codec, const uint8_t *data, size_t len, uint64_t pts90k) {
    NativeSessionSlot *slot = (NativeSessionSlot *)ctx;
    if (!slot) {
        return;
    }
    if (codec != RDP_VIDEO_CODEC_H264 && codec != RDP_VIDEO_CODEC_H265) {
        native_slot_report_terminal(slot, RDP_STATE_PROTOCOL_ERROR,
                                    rdp_state_exit_code(RDP_STATE_PROTOCOL_ERROR));
        return;
    }
    native_video_ingest_au(slot, codec == RDP_VIDEO_CODEC_H265 ? NATIVE_VIDEO_CODEC_H265 : NATIVE_VIDEO_CODEC_H264,
                           data, len, pts90k);
}

void native_rdp_install_video_callbacks(RdpCallbacks *callbacks) {
    callbacks->on_bitmap_update = on_bitmap_update;
    callbacks->on_video_au = on_video_au;
}
