#include "camera_h264_stream.h"

#include <linux/videodev2.h>
#include <stdlib.h>
#include <string.h>

#include "clog.h"
#include "h264_annexb.h"

clog_define(g_native_log_camera_h264_stream, cLogLevelInfo, "camera.h264");

typedef struct NativeCameraOwnedAu {
    uint8_t *data;
    size_t len;
    uint64_t timestamp_us;
    uint32_t sequence;
    bool is_idr;
} NativeCameraOwnedAu;

struct NativeCameraH264Stream {
    uint8_t *sps;
    size_t sps_len;
    uint8_t *pps;
    size_t pps_len;
    NativeCameraOwnedAu fifo[NATIVE_CAMERA_H264_FIFO_AUS];
    size_t head;
    size_t count;
    size_t bytes;
    unsigned waiting_aus;
    uint32_t last_sequence;
    bool have_sequence;
    bool seeded;
};

static void camera_owned_au_clear(NativeCameraOwnedAu *au) {
    free(au->data);
    memset(au, 0, sizeof(*au));
}

void native_camera_h264_stream_reset(NativeCameraH264Stream *stream) {
    if (!stream) {
        return;
    }
    for (size_t i = 0u; i < NATIVE_CAMERA_H264_FIFO_AUS; i++) {
        camera_owned_au_clear(&stream->fifo[i]);
    }
    free(stream->sps);
    free(stream->pps);
    stream->sps = NULL;
    stream->sps_len = 0u;
    stream->pps = NULL;
    stream->pps_len = 0u;
    stream->head = 0u;
    stream->count = 0u;
    stream->bytes = 0u;
    stream->waiting_aus = 0u;
    stream->last_sequence = 0u;
    stream->have_sequence = false;
    stream->seeded = false;
}

NativeCameraH264Stream *native_camera_h264_stream_create(void) {
    NativeCameraH264Stream *stream = (NativeCameraH264Stream *)calloc(1, sizeof(*stream));
    if (!stream) {
        clog(cLogLevelError, "could not allocate camera H.264 stream state");
    }
    return stream;
}

void native_camera_h264_stream_destroy(NativeCameraH264Stream *stream) {
    if (!stream) {
        return;
    }
    native_camera_h264_stream_reset(stream);
    free(stream);
}

static NativeCameraH264IngestResult camera_stream_recover(NativeCameraH264Stream *stream, const char *reason) {
    clog_limited(cLogLevelWarning, 4, 5000, "camera H.264 stream reset required: %s", reason);
    native_camera_h264_stream_reset(stream);
    return NATIVE_CAMERA_H264_INGEST_RECOVER;
}

static NativeCameraH264IngestResult camera_stream_wait(NativeCameraH264Stream *stream) {
    stream->waiting_aus++;
    if (stream->waiting_aus >= NATIVE_CAMERA_H264_WAIT_AUS) {
        stream->waiting_aus = 0u;
        clog_limited(cLogLevelDebug, 4, 5000,
                     "camera H.264 decoder seed still pending after the AU wait bound");
        return NATIVE_CAMERA_H264_INGEST_SAMPLE_ERROR;
    }
    return NATIVE_CAMERA_H264_INGEST_WAITING;
}

static bool camera_parameter_set_copy(const NativeH264NalView *nal, uint8_t **data, size_t *len) {
    static const uint8_t start_code[] = {0u, 0u, 0u, 1u};
    if (!nal || !nal->data || nal->len == 0u || !data || !len ||
        nal->len > NATIVE_CAMERA_H264_MAX_AU_BYTES - sizeof(start_code)) {
        return false;
    }
    size_t needed = sizeof(start_code) + nal->len;
    uint8_t *copy = (uint8_t *)malloc(needed);
    if (!copy) {
        return false;
    }
    memcpy(copy, start_code, sizeof(start_code));
    memcpy(copy + sizeof(start_code), nal->data, nal->len);
    free(*data);
    *data = copy;
    *len = needed;
    return true;
}

static bool camera_stream_cache_parameter_sets(NativeCameraH264Stream *stream, const NativeH264Info *info) {
    uint8_t *new_sps = NULL;
    size_t new_sps_len = 0u;
    uint8_t *new_pps = NULL;
    size_t new_pps_len = 0u;
    if (info->has_sps && !camera_parameter_set_copy(&info->sps, &new_sps, &new_sps_len)) {
        free(new_sps);
        free(new_pps);
        return false;
    }
    if (info->has_pps && !camera_parameter_set_copy(&info->pps, &new_pps, &new_pps_len)) {
        free(new_sps);
        free(new_pps);
        return false;
    }
    size_t final_sps_len = new_sps ? new_sps_len : stream->sps_len;
    size_t final_pps_len = new_pps ? new_pps_len : stream->pps_len;
    if (final_sps_len > NATIVE_CAMERA_H264_MAX_AU_BYTES ||
        final_pps_len > NATIVE_CAMERA_H264_MAX_AU_BYTES - final_sps_len) {
        free(new_sps);
        free(new_pps);
        return false;
    }
    if (new_sps) {
        free(stream->sps);
        stream->sps = new_sps;
        stream->sps_len = new_sps_len;
    }
    if (new_pps) {
        free(stream->pps);
        stream->pps = new_pps;
        stream->pps_len = new_pps_len;
    }
    return true;
}

static bool camera_stream_queue(NativeCameraH264Stream *stream, const NativeCameraFrame *frame,
                                const NativeH264Info *info) {
    size_t prefix = 0u;
    if (info->has_idr && !info->can_seed_decoder) {
        if (!stream->sps || !stream->pps || stream->sps_len > SIZE_MAX - stream->pps_len) {
            return false;
        }
        prefix = stream->sps_len + stream->pps_len;
    }
    if (prefix > NATIVE_CAMERA_H264_MAX_AU_BYTES || frame->len > NATIVE_CAMERA_H264_MAX_AU_BYTES - prefix) {
        return false;
    }
    size_t output_len = prefix + frame->len;
    if (stream->count >= NATIVE_CAMERA_H264_FIFO_AUS || output_len > NATIVE_CAMERA_H264_FIFO_BYTES - stream->bytes) {
        return false;
    }
    uint8_t *copy = (uint8_t *)malloc(output_len);
    if (!copy) {
        return false;
    }
    size_t written = 0u;
    if (prefix) {
        memcpy(copy, stream->sps, stream->sps_len);
        written += stream->sps_len;
        memcpy(copy + written, stream->pps, stream->pps_len);
        written += stream->pps_len;
    }
    memcpy(copy + written, frame->data, frame->len);

    size_t tail = (stream->head + stream->count) % NATIVE_CAMERA_H264_FIFO_AUS;
    NativeCameraOwnedAu *queued = &stream->fifo[tail];
    queued->data = copy;
    queued->len = output_len;
    queued->timestamp_us = frame->timestamp_us;
    queued->sequence = frame->sequence;
    queued->is_idr = info->has_idr;
    stream->count++;
    stream->bytes += output_len;
    return true;
}

NativeCameraH264IngestResult native_camera_h264_stream_ingest(NativeCameraH264Stream *stream,
                                                              const NativeCameraFrame *frame) {
    if (!stream || !frame || !frame->data || frame->len == 0u || frame->len > NATIVE_CAMERA_H264_MAX_AU_BYTES) {
        return stream ? camera_stream_recover(stream, "invalid or oversized AU") : NATIVE_CAMERA_H264_INGEST_RECOVER;
    }
    if ((frame->flags & V4L2_BUF_FLAG_ERROR) != 0u) {
        return camera_stream_recover(stream, "V4L2 marked the buffer corrupt");
    }
    if (stream->have_sequence && frame->sequence != stream->last_sequence + 1u) {
        /* Dropped buffers break only the dependent-picture chain. The node is
         * healthy and the cached parameter sets stay valid, so re-gate on the
         * next IDR instead of reopening: reopening loses the seed as well and
         * the fresh capture starts mid-GOP, which drops far more video than the
         * gap itself. AUs already queued predate the gap and stay sendable.
         * Priming the wait counter answers a pending credit with one
         * SampleError now rather than after another full wait bound. */
        clog_limited(cLogLevelWarning, 4, 5000, "V4L2 sequence gap %u -> %u; re-seeding on the next IDR",
                     stream->last_sequence, frame->sequence);
        stream->seeded = false;
        stream->waiting_aus = NATIVE_CAMERA_H264_WAIT_AUS - 1u;
    }
    stream->last_sequence = frame->sequence;
    stream->have_sequence = true;

    NativeH264Info info;
    if (native_h264_scan_annexb(frame->data, frame->len, &info) != NATIVE_H264_OK) {
        return camera_stream_recover(stream, "malformed or non-Annex-B AU");
    }
    if (!info.starts_with_annexb_start_code) {
        return camera_stream_recover(stream, "AU does not begin with a start code");
    }

    if (!camera_stream_cache_parameter_sets(stream, &info)) {
        return camera_stream_recover(stream, "parameter-set allocation failure");
    }
    if (!info.has_vcl) {
        return camera_stream_wait(stream);
    }
    if (!stream->seeded && !info.has_idr) {
        return camera_stream_wait(stream);
    }
    if (info.has_idr && !info.can_seed_decoder && (!stream->sps || !stream->pps)) {
        return camera_stream_wait(stream);
    }
    if (!camera_stream_queue(stream, frame, &info)) {
        return camera_stream_recover(stream, "FIFO overflow, AU growth, or allocation failure");
    }
    if (info.has_idr) {
        stream->seeded = true;
    }
    stream->waiting_aus = 0u;
    return NATIVE_CAMERA_H264_INGEST_QUEUED;
}

bool native_camera_h264_stream_peek(const NativeCameraH264Stream *stream, NativeCameraH264Packet *packet) {
    if (!stream || !packet || stream->count == 0u) {
        return false;
    }
    const NativeCameraOwnedAu *head = &stream->fifo[stream->head];
    packet->data = head->data;
    packet->len = head->len;
    packet->timestamp_us = head->timestamp_us;
    packet->sequence = head->sequence;
    packet->is_idr = head->is_idr;
    return true;
}

void native_camera_h264_stream_pop(NativeCameraH264Stream *stream) {
    if (!stream || stream->count == 0u) {
        return;
    }
    NativeCameraOwnedAu *head = &stream->fifo[stream->head];
    stream->bytes -= head->len;
    camera_owned_au_clear(head);
    stream->head = (stream->head + 1u) % NATIVE_CAMERA_H264_FIFO_AUS;
    stream->count--;
}

size_t native_camera_h264_stream_queued_aus(const NativeCameraH264Stream *stream) {
    return stream ? stream->count : 0u;
}

size_t native_camera_h264_stream_queued_bytes(const NativeCameraH264Stream *stream) {
    return stream ? stream->bytes : 0u;
}
