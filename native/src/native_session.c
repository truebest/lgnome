/* Session-slot lifecycle: connect/stop for one slot, whole-app teardown, the
 * shared-media teardown, and the audio-mix publications (solo mask, duck
 * retarget) that must follow every slot change. SDL/main thread only. */

#include "native_session.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "native_app.h"
#include "native_capture_glue.h"
#include "native_presentation.h"
#include "native_config.h"
#include "native_rdp_callbacks.h"
#include "native_rgba_owner.h"

#include "clog.h"

clog_define(g_native_log_session, cLogLevelInfo, cLogFlags_Default, "native", NULL);

/* Retargets foreground-only policies after every active_index store: playback ducking
 * follows the on-screen session while outgoing camera/microphone payloads stop for every
 * background slot. Incoming playback audio remains mixed from all connected slots. */
void native_duck_retarget(App *app) {
    int fg = atomic_load(&app->active_index);
    native_audio_pipeline_set_duck_foreground(&app->audio_pipeline, fg,
                                              (uint32_t)app->duck_mask[fg] & ~(1u << fg));
    native_capture_follow_active_slot(app);
}

bool native_any_slot_connected(const App *app) {
    for (int i = 0; i < NATIVE_SETTINGS_MAX_SESSIONS; i++) {
        if (app->sessions[i].rdp) {
            return true;
        }
    }
    return false;
}

/* Tears down the shared presentation pipeline (RGBA surface, audio/video tracks, media
 * player). Sessions themselves are unaffected; call when no slot needs the screen. */
void native_stop_media(App *app) {
    if (!app) {
        return;
    }
    pthread_mutex_lock(&app->video_lock);
    native_rgba_surface_close(app->rgba);
    app->rgba = NULL;
    native_reset_rgba_owner(app);
    /* native_stop_media() is SDL/main-thread only, so the frozen surface's texture can
     * be destroyed directly along with the current canvas. */
    native_close_hub_return_rgba_locked(app);
    /* Tracks first, then the shared player/library they attach to. */
    pthread_mutex_lock(&app->audio_lock);
    native_audio_close(app->audio);
    app->audio = NULL;
    pthread_mutex_unlock(&app->audio_lock);
    native_video_close(app->video);
    app->video = NULL;
    native_media_close(app->media);
    app->media = NULL;
#ifdef HELLOLG_TARGET_WEBOS
    /* native_stop_media() only ever runs on the SDL/main thread, so it's safe to destroy
     * directly here rather than deferring; drain any texture a worker-thread callback
     * handed off but the render loop hasn't gotten to yet. */
    if (app->pending_texture_destroy) {
        SDL_DestroyTexture(app->pending_texture_destroy);
        app->pending_texture_destroy = NULL;
    }
#endif
    pthread_mutex_unlock(&app->video_lock);
}

void native_publish_effective_solo_mask(App *app) {
    uint8_t connected_mask = 0;
    for (int i = 0; i < NATIVE_SETTINGS_MAX_SESSIONS; i++) {
        if (app->sessions[i].rdp) {
            connected_mask |= (uint8_t)(1u << i);
        }
    }
    native_audio_pipeline_set_solo_mask(&app->audio_pipeline, app->mixer_solo_mask & connected_mask);
}

/* Stops one slot's RDP session (joins its worker) and detaches it from the shared
 * input/mixer state. Does NOT touch the media pipeline — a switch keeps audio alive. */
void native_stop_slot(App *app, int index) {
    if (!app || index < 0 || index >= NATIVE_SETTINGS_MAX_SESSIONS) {
        return;
    }
    NativeSessionSlot *slot = &app->sessions[index];
    if (index == atomic_load(&app->active_index)) {
        native_input_set_active(&app->input, false);
        native_input_set_session(&app->input, NULL);
    }
    if (slot->rdp) {
        /* Prevent capture workers from retaining/submitting through the handle before
         * rdp_session_stop joins and frees it. set_slot synchronizes with both workers. */
        native_capture_redirect_set_slot(&app->capture_redirect, index, NULL, false,
                                         false);
        rdp_session_stop(slot->rdp);
        slot->rdp = NULL;
    }
    pthread_mutex_lock(&app->video_lock);
    native_audio_pipeline_close_source(&app->audio_pipeline, index);
    slot->audio_routed = false;
    atomic_store(&slot->audio_codec, 0u);
    atomic_store(&slot->audio_sample_rate, 0u);
    atomic_store(&slot->audio_channels, 0u);
    slot->audio_incompatible_logged = false;
    /* Whatever the cache held belongs to the connection that just died. */
    native_au_snapshot_reset(&slot->snapshot);
    pthread_mutex_unlock(&app->video_lock);
    native_publish_effective_solo_mask(app);
    atomic_store(&slot->snapshot_idr_ready, false);
    slot->snapshot_pending = false;
    /* The worker is joined above; its decoder can be freed from this thread now. */
    native_opus_decoder_close(slot->opus_decoder);
    slot->opus_decoder = NULL;
    slot->suppressed = false;
    atomic_store(&slot->current_state, (int)RDP_STATE_IDLE);
    atomic_store(&slot->session_failed, false);
#ifdef HELLOLG_TARGET_WEBOS
    app->session_started_ms[index] = 0;
    app->session_runtime_active[index] = false;
#endif
}

void native_stop_all_sessions(App *app) {
    if (!app) {
        return;
    }
#ifdef HELLOLG_TARGET_WEBOS
    /* Release the global evdev grab so the compositor/preconnect UI gets USB input back. */
    native_evdev_input_stop(&app->evdev_input);
#endif
    for (int i = 0; i < NATIVE_SETTINGS_MAX_SESSIONS; i++) {
        native_stop_slot(app, i);
    }
    native_stop_media(app);
}

/* (Re)connects one slot using slot->config (the caller copies the settings in first).
 * Only the ACTIVE slot resets the shared decoder/input state; a background
 * connect-on-demand must not disturb the running stream. With `arm_snapshot` the AU
 * snapshot is armed HERE, between stopping the old worker and starting the new one —
 * the only ordering that guarantees the fresh connection cannot emit its decoder-seed
 * IDR into an unarmed cache (the caller re-reads snapshot.armed for allocation failure). */
bool native_slot_connect(App *app, int index, bool arm_snapshot) {
    if (!app || index < 0 || index >= NATIVE_SETTINGS_MAX_SESSIONS) {
        return false;
    }
    NativeSessionSlot *slot = &app->sessions[index];
#ifdef HELLOLG_TARGET_WEBOS
    /* All calls that reach here with a running slot are internal recovery reconnects,
     * except the explicit UI Connect path, which clears this clock before calling us. */
    uint64_t logical_session_started_ms = app->session_started_ms[index];
    bool logical_session_runtime_active = app->session_runtime_active[index];
#endif
    native_stop_slot(app, index);
#ifdef HELLOLG_TARGET_WEBOS
    pthread_mutex_lock(&app->video_lock);
    if (app->hub_return_rgba && app->hub_return_replacement_slot == index) {
        /* The old worker is joined, so this cumulative frame baseline is stable. Publish
         * the next epoch before the replacement worker can race out its first bitmap/AU. */
        native_arm_hub_return_replacement_locked(
            app, index, atomic_load(&slot->connect_epoch) + 1u,
            atomic_load(&slot->video_ok_frames));
    }
    pthread_mutex_unlock(&app->video_lock);
#endif
    if (arm_snapshot) {
        /* "Not ready" must be public before the cache can accept AUs (see on_video_au:
         * the timestamp/readiness pair is published append-side in the safe order). */
        atomic_store(&slot->snapshot_idr_ready, false);
        pthread_mutex_lock(&app->video_lock);
        if (!native_au_snapshot_arm(&slot->snapshot)) {
            clog(cLogLevelWarning, "%s AU snapshot allocation failed", native_session_slot_name(index));
        }
        pthread_mutex_unlock(&app->video_lock);
    }
    atomic_fetch_add(&slot->connect_epoch, 1u);
    atomic_store(&slot->terminal_state, (int)RDP_STATE_IDLE);
    atomic_store(&slot->desktop_width, NATIVE_RDP_INITIAL_DESKTOP_WIDTH);
    atomic_store(&slot->desktop_height, NATIVE_RDP_INITIAL_DESKTOP_HEIGHT);

    bool is_active = index == atomic_load(&app->active_index);
    if (is_active) {
        app->decoder_errors = 0;
        app->decoder_keyframe_pending = false;
        native_present_invalidate(app);
        atomic_store(&app->exit_code, 0);
        uint16_t window_width = (uint16_t)atomic_load(&app->input.window_width);
        uint16_t window_height = (uint16_t)atomic_load(&app->input.window_height);
        native_input_init(&app->input, NULL, NATIVE_RDP_INITIAL_DESKTOP_WIDTH, NATIVE_RDP_INITIAL_DESKTOP_HEIGHT);
        if (window_width != 0 && window_height != 0) {
            native_input_set_window_size(&app->input, window_width, window_height);
        }
    }

    RdpConfig config = {
        .host = slot->config.host,
        .port = slot->config.port,
        .username = slot->config.username,
        .password = slot->config.password,
        .domain = slot->config.domain,
        .width = NATIVE_RDP_INITIAL_DESKTOP_WIDTH,
        .height = NATIVE_RDP_INITIAL_DESKTOP_HEIGHT,
        .fps = slot->config.fps,
        .prefer_pcm_audio = app->audio_codec == NATIVE_AUDIO_CODEC_PCM ? 1 : 0,
        .enable_camera =
            app->settings->camera_enabled && slot->config.camera_redirect ? 1 : 0,
        .enable_audio_input =
            app->settings->audio_input_enabled &&
                    slot->config.audio_input_redirect
                ? 1
                : 0,
        .reserved0 = 0,
        .camera_width = app->settings->camera_width,
        .camera_height = app->settings->camera_height,
        .camera_fps = app->settings->camera_fps,
    };
    RdpCallbacks callbacks = native_rdp_callbacks(slot);

    clog(cLogLevelInfo, "starting %s session for %s:%u (%ux%u@%u AVC420/RemoteFX)",
         native_session_slot_name(index), config.host, (unsigned)config.port, (unsigned)config.width,
         (unsigned)config.height, (unsigned)config.fps);
    slot->rdp = rdp_session_start(&config, &callbacks);
    if (!slot->rdp) {
        clog(cLogLevelError, "rdp_session_start failed for the %s session",
             native_session_slot_name(index));
        return false;
    }
    native_capture_redirect_set_slot(
        &app->capture_redirect, index, slot->rdp, config.enable_camera != 0,
        config.enable_audio_input != 0);
#ifdef HELLOLG_TARGET_WEBOS
    app->session_started_ms[index] = logical_session_started_ms;
    app->session_runtime_active[index] = logical_session_runtime_active;
#endif
    native_publish_effective_solo_mask(app);
    if (is_active) {
        native_input_set_session(&app->input, slot->rdp);
    }
    return true;
}
