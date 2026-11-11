#include "media_backend.h"
#include "media_ndl_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#include "native_qos.h"

#include "clog.h"

clog_define(g_native_log_media, cLogLevelInfo, "media.ndl");

#define LGNOME_NDL_FALLBACK_APP_ID "com.truebest.lgnome.native"

#ifdef LGNOME_WITH_NDL
typedef struct NativeDecoderWindow {
    NativeDecoderStats published;
    NativeQosSamples decode, interval;
    uint32_t generation, primary, reported_primary, gaps;
    int64_t began_us, last_primary_us;
} NativeDecoderWindow;

struct NativeMedia {
    BackendNdl *backend;
    pthread_mutex_t decoder_lock;
    NativeDecoderWindow decoder;
    bool has_video;
    BackendNdlVideoInfo video;
    bool has_audio;
    BackendNdlAudioInfo audio;
};

static void native_media_ndl_frame(void *userdata, const BackendNdlVideoFrameEvent *event) {
    NativeMedia *media = userdata;
    if (!event->generation || event->submitted_at_us < 0 || event->decoded_at_us < event->submitted_at_us) return;
    pthread_mutex_lock(&media->decoder_lock);
    NativeDecoderWindow *d = &media->decoder;
    int64_t now = event->decoded_at_us;
    if (d->generation != event->generation) {
        memset(d, 0, sizeof(*d));
        d->generation = event->generation;
        d->began_us = now;
    }
    native_qos_record(&d->decode, (uint64_t)(now - event->submitted_at_us));
    if (!event->decode_only) {
        d->primary++;
        if (d->last_primary_us && now >= d->last_primary_us) {
            uint64_t interval = (uint64_t)(now - d->last_primary_us);
            native_qos_record(&d->interval, interval);
            if (interval > 100000) d->gaps++;
        }
        d->last_primary_us = now;
    }
    if (now - d->began_us >= 5000000) {
        d->published = (NativeDecoderStats){
            .at_ms = (uint32_t)(now / 1000), .window_ms = (uint32_t)((now - d->began_us) / 1000),
            .pictures = d->decode.count, .primary = d->primary - d->reported_primary,
            .decode_p95_us = native_qos_p95(&d->decode),
            .interval_p95_us = native_qos_p95(&d->interval), .interval_max_us = d->interval.max_us,
            .intervals = d->interval.count, .gaps = d->gaps};
        native_qos_clear(&d->decode);
        native_qos_clear(&d->interval);
        d->began_us = now;
        d->reported_primary = d->primary;
    }
    pthread_mutex_unlock(&media->decoder_lock);
}

static void native_media_ndl_log(void *userdata, BackendNdlLogLevel level, const char *message) {
    (void)userdata;
    cLogLevel mapped = cLogLevelInfo;
    switch (level) {
        case BACKEND_NDL_LOG_DEBUG:
            mapped = cLogLevelDebug;
            break;
        case BACKEND_NDL_LOG_INFO:
            mapped = cLogLevelInfo;
            break;
        case BACKEND_NDL_LOG_WARN:
            mapped = cLogLevelWarning;
            break;
        default:
            mapped = cLogLevelError;
            break;
    }
    clog(mapped, "%s", message ? message : "");
}

/* backend_ndl gates its debug telemetry (NDL buffer polls, message formatting) on
 * minimum_log_level, so mirror the effective media.ndl level instead of formatting
 * everything for clog to discard. */
static BackendNdlLogLevel native_media_ndl_min_level(void) {
    if (clog_is_enabled(cLogLevelDebug)) {
        return BACKEND_NDL_LOG_DEBUG;
    }
    if (clog_is_enabled(cLogLevelInfo)) {
        return BACKEND_NDL_LOG_INFO;
    }
    if (clog_is_enabled(cLogLevelWarning)) {
        return BACKEND_NDL_LOG_WARN;
    }
    if (clog_is_enabled(cLogLevelError)) {
        return BACKEND_NDL_LOG_ERROR;
    }
    return BACKEND_NDL_LOG_OFF;
}

#ifdef LGNOME_NDL_ADAPTER_TESTING
BackendNdlLogLevel native_media_ndl_test_min_level(void) {
    return native_media_ndl_min_level();
}
#endif

static BackendNdlResult native_media_ndl_apply_tracks(NativeMedia *media) {
    if (!media || !media->backend) {
        return BACKEND_NDL_INVALID_ARGUMENT;
    }
    return backend_ndl_set_media(media->backend, media->has_video ? &media->video : NULL,
                                 media->has_audio ? &media->audio : NULL);
}
#endif

NativeMedia *native_media_open(const char *webos_sdk_version) {
#ifndef LGNOME_WITH_NDL
    (void)webos_sdk_version;
    clog(cLogLevelError, "NDL backend is not linked; hardware media unavailable");
    return NULL;
#else
    NativeMedia *media = (NativeMedia *)calloc(1, sizeof(NativeMedia));
    if (!media) {
        return NULL;
    }

    if (pthread_mutex_init(&media->decoder_lock, NULL) != 0) {
        free(media);
        return NULL;
    }
    BackendNdlConfig config;
    backend_ndl_config_defaults(&config);
    const char *app_id = getenv("APPID");
    config.app_id = app_id && app_id[0] ? app_id : LGNOME_NDL_FALLBACK_APP_ID;
    config.webos_sdk_version = webos_sdk_version;
    config.log_fn = native_media_ndl_log;
    config.userdata = media;
    config.video_frame_fn = native_media_ndl_frame;
    config.minimum_log_level = native_media_ndl_min_level();
    config.require_keyframe_after_reload = true;
    config.guard_audio_overflow = true;
    config.prime_pcm_after_load = true;
    media->backend = backend_ndl_open(&config);
    if (!media->backend) {
        clog(cLogLevelError, "NDL DirectMedia backend unavailable");
        pthread_mutex_destroy(&media->decoder_lock);
        free(media);
        return NULL;
    }
    clog(cLogLevelNotice, "NDL DirectMedia backend ready (profile=%s)",
         backend_ndl_abi_profile_name(backend_ndl_get_abi_profile(media->backend)));
    return media;
#endif
}

void native_media_close(NativeMedia *media) {
    if (!media) {
        return;
    }
#ifdef LGNOME_WITH_NDL
    backend_ndl_close(media->backend);
    pthread_mutex_destroy(&media->decoder_lock);
#endif
    free(media);
}

void native_media_emergency_release(void) {
#ifdef LGNOME_WITH_NDL
    (void)backend_ndl_emergency_release();
#endif
}

NativeDecoderStats native_media_ndl_decoder_stats(NativeMedia *media) {
    NativeDecoderStats out = {0};
#ifdef LGNOME_WITH_NDL
    BackendNdlVideoStatus status = {0};
    if (!media || backend_ndl_get_video_status(media->backend, &status) != BACKEND_NDL_OK ||
        status.state != BACKEND_NDL_VIDEO_PATH_READY || !status.supports_frame_events) return out;
    /* Do not call the backend while holding the callback's statistics lock. */
    pthread_mutex_lock(&media->decoder_lock);
    if (media->decoder.generation == status.generation) out = media->decoder.published;
    pthread_mutex_unlock(&media->decoder_lock);
    out.available = true;
    out.generation = status.generation;
    out.primary_total = status.forwarded;
    out.pending = status.pending_pictures;
#else
    (void)media;
#endif
    return out;
}

BackendNdl *native_media_ndl_backend(NativeMedia *media) {
#ifndef LGNOME_WITH_NDL
    (void)media;
    return NULL;
#else
    return media ? media->backend : NULL;
#endif
}

BackendNdlResult native_media_ndl_configure_video(NativeMedia *media,
                                                   const BackendNdlVideoInfo *info) {
#ifndef LGNOME_WITH_NDL
    (void)media;
    (void)info;
    return BACKEND_NDL_UNAVAILABLE;
#else
    if (!media || !info) {
        return BACKEND_NDL_INVALID_ARGUMENT;
    }
    bool had_video = media->has_video;
    BackendNdlVideoInfo saved = media->video;
    media->has_video = true;
    media->video = *info;
    BackendNdlResult result = native_media_ndl_apply_tracks(media);
    if (result != BACKEND_NDL_OK) {
        media->has_video = had_video;
        media->video = saved;
    }
    return result;
#endif
}

BackendNdlResult native_media_ndl_configure_audio(NativeMedia *media,
                                                   const BackendNdlAudioInfo *info) {
#ifndef LGNOME_WITH_NDL
    (void)media;
    (void)info;
    return BACKEND_NDL_UNAVAILABLE;
#else
    if (!media || !info) {
        return BACKEND_NDL_INVALID_ARGUMENT;
    }
    bool had_audio = media->has_audio;
    BackendNdlAudioInfo saved = media->audio;
    media->has_audio = true;
    media->audio = *info;
    BackendNdlResult result = native_media_ndl_apply_tracks(media);
    if (result != BACKEND_NDL_OK) {
        media->has_audio = had_audio;
        media->audio = saved;
    }
    return result;
#endif
}

BackendNdlResult native_media_ndl_clear_video(NativeMedia *media) {
#ifndef LGNOME_WITH_NDL
    (void)media;
    return BACKEND_NDL_UNAVAILABLE;
#else
    if (!media || !media->backend) {
        return BACKEND_NDL_INVALID_ARGUMENT;
    }
    bool had_video = media->has_video;
    BackendNdlVideoInfo saved = media->video;
    media->has_video = false;
    memset(&media->video, 0, sizeof(media->video));
    BackendNdlResult result = native_media_ndl_apply_tracks(media);
    if (result != BACKEND_NDL_OK) {
        media->has_video = had_video;
        media->video = saved;
    }
    return result;
#endif
}

BackendNdlResult native_media_ndl_clear_audio(NativeMedia *media) {
#ifndef LGNOME_WITH_NDL
    (void)media;
    return BACKEND_NDL_UNAVAILABLE;
#else
    if (!media || !media->backend) {
        return BACKEND_NDL_INVALID_ARGUMENT;
    }
    bool had_audio = media->has_audio;
    BackendNdlAudioInfo saved = media->audio;
    media->has_audio = false;
    memset(&media->audio, 0, sizeof(media->audio));
    BackendNdlResult result = native_media_ndl_apply_tracks(media);
    if (result != BACKEND_NDL_OK) {
        media->has_audio = had_audio;
        media->audio = saved;
    }
    return result;
#endif
}
