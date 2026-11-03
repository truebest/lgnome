#include "media_ndl_internal.h"
#include <assert.h>

static BackendNdlVideoStatus status;
static BackendNdlResult fake_video_status(const BackendNdl *backend, BackendNdlVideoStatus *out) {
    (void)backend;
    *out = status;
    return BACKEND_NDL_OK;
}
#define backend_ndl_get_video_status fake_video_status
#include "../src/ndl_adapter/media_ndl.c"
#undef backend_ndl_get_video_status

static void output(NativeMedia *media, uint32_t generation, int64_t at, bool auxiliary) {
    BackendNdlVideoFrameEvent event = {.generation = generation, .submitted_at_us = at - 1000,
        .decoded_at_us = at, .decode_only = auxiliary};
    native_media_ndl_frame(media, &event);
}

int main(void) {
    NativeMedia media = {0};
    assert(pthread_mutex_init(&media.decoder_lock, NULL) == 0);
    assert(!native_media_ndl_decoder_stats(NULL).available);
    assert(!native_media_ndl_decoder_stats(&media).available);
    status = (BackendNdlVideoStatus){.state = BACKEND_NDL_VIDEO_PATH_READY,
        .supports_frame_events = true, .generation = 7, .forwarded = 2, .pending_pictures = 3};
    output(&media, 7, 1000000, false);
    output(&media, 7, 2000000, true);
    NativeDecoderStats stats = native_media_ndl_decoder_stats(&media);
    assert(stats.available && !stats.window_ms && stats.primary_total == 2 && stats.pending == 3);
    output(&media, 7, 6100000, false);
    stats = native_media_ndl_decoder_stats(&media);
    assert(stats.window_ms == 5100 && stats.at_ms == 6100);
    assert(stats.pictures == 3 && stats.primary == 2 && stats.intervals == 1);
    assert(stats.decode_p95_us == 1000 && stats.interval_p95_us == 5100000 && stats.gaps == 1);
    status.generation = 8;
    status.forwarded = 0;
    stats = native_media_ndl_decoder_stats(&media);
    assert(stats.available && stats.generation == 8 && !stats.window_ms && !stats.primary_total);
    output(&media, 8, 7000000, false);
    assert(media.decoder.gaps == 0 && media.decoder.interval.count == 0);
    status.generation = UINT32_MAX;
    output(&media, UINT32_MAX, 8000000, false);
    status.generation = 1;
    output(&media, 1, 9000000, false);
    assert(native_media_ndl_decoder_stats(&media).generation == 1);
    assert(media.decoder.primary == 1 && !media.decoder.published.window_ms);
    status.supports_frame_events = false;
    assert(!native_media_ndl_decoder_stats(&media).available);
    status.supports_frame_events = true;
    status.state = BACKEND_NDL_VIDEO_PATH_FAILED;
    assert(!native_media_ndl_decoder_stats(&media).available);
    pthread_mutex_destroy(&media.decoder_lock);
    return 0;
}
