#ifndef LGNOME_CAMERA_H264_STREAM_H
#define LGNOME_CAMERA_H264_STREAM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "camera_v4l2.h"

#define NATIVE_CAMERA_H264_FIFO_AUS 8u
#define NATIVE_CAMERA_H264_FIFO_BYTES ((size_t)8u * 1024u * 1024u)
#define NATIVE_CAMERA_H264_WAIT_AUS 8u

typedef struct NativeCameraH264Stream NativeCameraH264Stream;

typedef struct NativeCameraH264Packet {
    const uint8_t *data;
    size_t len;
    uint64_t timestamp_us;
    uint32_t sequence;
    bool is_idr;
} NativeCameraH264Packet;

typedef enum NativeCameraH264IngestResult {
    /* A valid but not yet sendable AU was consumed; no sample is ready yet.
     * NATIVE_CAMERA_H264_WAIT_AUS consecutive results become SAMPLE_ERROR. */
    NATIVE_CAMERA_H264_INGEST_WAITING = 0,
    NATIVE_CAMERA_H264_INGEST_QUEUED = 1,
    /* The caller must answer any pending RDPECAM credit with SampleError, but
     * keep capturing: sequence and parameter-set state remain valid. */
    NATIVE_CAMERA_H264_INGEST_SAMPLE_ERROR = 2,
    /* The caller must answer any pending RDPECAM credit with SampleError and
     * reopen V4L2. The stream has already discarded its FIFO and seed state. */
    NATIVE_CAMERA_H264_INGEST_RECOVER = -1,
} NativeCameraH264IngestResult;

NativeCameraH264Stream *native_camera_h264_stream_create(void);
void native_camera_h264_stream_destroy(NativeCameraH264Stream *stream);
void native_camera_h264_stream_reset(NativeCameraH264Stream *stream);

/* Copies one borrowed V4L2 buffer into the bounded stream state. Only Annex-B
 * AUs are accepted. SPS/PPS are cached and prepended to an IDR when necessary;
 * dependent pictures remain gated until that self-contained seed is queued. */
NativeCameraH264IngestResult native_camera_h264_stream_ingest(NativeCameraH264Stream *stream,
                                                              const NativeCameraFrame *frame);

/* The peeked bytes remain owned by stream until pop/reset/destroy. */
bool native_camera_h264_stream_peek(const NativeCameraH264Stream *stream, NativeCameraH264Packet *packet);
void native_camera_h264_stream_pop(NativeCameraH264Stream *stream);
size_t native_camera_h264_stream_queued_aus(const NativeCameraH264Stream *stream);
size_t native_camera_h264_stream_queued_bytes(const NativeCameraH264Stream *stream);

#endif
