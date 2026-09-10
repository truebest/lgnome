/* Shared NDL media-pipeline ownership used by the RDP video and audio callbacks. */

#include "native_rdp_media.h"

#include <stdint.h>

#include "native_app.h"

#include "clog.h"

clog_define(g_native_log_rdp_media, cLogLevelInfo, "native");

/* Caller holds app->video_lock. */
NativeMedia *native_ensure_media_locked(App *app) {
    if (!app) {
        return NULL;
    }
    if (app->media) {
        return app->media;
    }
    app->media = native_media_open(
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

/* Caller holds video_lock. Attach mixed audio before video to avoid a mid-stream pipeline reload. */
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
