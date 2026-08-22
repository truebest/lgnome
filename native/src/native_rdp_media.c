/* Shared NDL media-pipeline ownership used by the RDP video and audio callbacks. */

#include "native_rdp_media.h"

#include <stdint.h>

#include "native_app.h"

#include "clog.h"

clog_define(g_native_log_rdp_media, cLogLevelInfo, cLogFlags_Default, "native", NULL);

/* Creates the shared SS4S media owner on first use. Caller must hold app->video_lock. */
NativeMedia *native_ensure_media_locked(App *app) {
    if (!app) {
        return NULL;
    }
    if (app->media) {
        return app->media;
    }
#ifdef HELLOLG_TARGET_WEBOS
    uint16_t viewport_width = (uint16_t)atomic_load(&app->render_width);
    uint16_t viewport_height = (uint16_t)atomic_load(&app->render_height);
#else
    uint16_t viewport_width = (uint16_t)atomic_load(&app->input.window_width);
    uint16_t viewport_height = (uint16_t)atomic_load(&app->input.window_height);
#endif
    /* Same defaulting the old open-at-first-AU path used when no render size is known
     * yet (e.g. audio negotiates before the first video frame). */
    NativeSessionSlot *active = native_active_slot(app);
    if (viewport_width == 0) {
        viewport_width = (uint16_t)atomic_load(&active->desktop_width);
    }
    if (viewport_height == 0) {
        viewport_height = (uint16_t)atomic_load(&active->desktop_height);
    }
    app->media = native_media_open(
        viewport_width, viewport_height,
        app->webos_platform.sdk_version[0] ? app->webos_platform.sdk_version : NULL);
    return app->media;
}

/* NDL sink: the headless miniaudio engine renders one 10 ms S16 block and this
 * callback feeds NDL. The engine render itself is lock-free; audio_lock only protects
 * the track pointer/lifetime, independently of RemoteFX presentation under video_lock. */
void native_audio_pipeline_feed_cb(void *ctx, const int16_t *samples, size_t frames) {
    App *app = (App *)ctx;
    pthread_mutex_lock(&app->audio_lock);
    if (app->audio) {
        size_t bytes = frames * NATIVE_AUDIO_PIPELINE_CHANNELS * sizeof(int16_t);
        if (native_audio_feed(app->audio, (const uint8_t *)samples, bytes) == NATIVE_AUDIO_ERROR) {
            /* Mute instead of closing: removing audio would reload the live video track
             * and force another server keyframe request. */
            clog(cLogLevelWarning, "mixed audio feed failed; muting audio");
            native_audio_disable(app->audio);
        }
    }
    pthread_mutex_unlock(&app->audio_lock);
}

/* Opens the shared mixed-audio track speculatively as PCM 48kHz stereo before the
 * rdpsnd negotiation confirms it. The outcome is deterministic against
 * gnome-remote-desktop (the client advertises Opus 48k + PCM and grd prefers Opus, which
 * decodes to 48k stereo), and opening audio EARLY removes the track-open race entirely: both
 * tracks share one webOS hardware pipeline, so an audio open landing after the video
 * stream has started reloads the pipeline and stalls video until an IDR the server never
 * resends. If negotiation ends up choosing something else (or the server has no audio),
 * the normal on_audio_format path corrects or mutes it. Caller must hold app->video_lock. */
void native_open_speculative_audio_locked(App *app) {
    if (!app || !app->media || !native_audio_pipeline_is_initialized(&app->audio_pipeline)) {
        return;
    }
    /* The backend's own lock serializes this track configuration against any
     * in-flight feed call (lock order: see App.video_lock). */
    pthread_mutex_lock(&app->audio_lock);
    if (app->audio) {
        pthread_mutex_unlock(&app->audio_lock);
        return;
    }
    app->audio = native_audio_open(app->media, RDP_AUDIO_CODEC_PCM_S16LE, NATIVE_AUDIO_PIPELINE_SAMPLE_RATE,
                                   NATIVE_AUDIO_PIPELINE_CHANNELS);
    if (app->audio) {
        clog(cLogLevelInfo, "opened speculative mixed PCM %uHz %uch track ahead of negotiation",
             NATIVE_AUDIO_PIPELINE_SAMPLE_RATE, NATIVE_AUDIO_PIPELINE_CHANNELS);
    }
    pthread_mutex_unlock(&app->audio_lock);
}
