#include <assert.h>
#include <linux/videodev2.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "camera_h264_stream.h"
#include "h264_annexb.h"

static const uint8_t SPS[] = {0u, 0u, 0u, 1u, 0x67u, 0x42u, 0u, 0x1fu};
static const uint8_t PPS[] = {0u, 0u, 1u, 0x68u, 0xceu, 0x06u};
static const uint8_t IDR[] = {0u, 0u, 0u, 1u, 0x65u, 0x88u, 0x84u};
static const uint8_t PICTURE[] = {0u, 0u, 1u, 0x41u, 0x9au, 0x22u};
static const uint8_t SEED[] = {
    0u, 0u, 0u, 1u, 0x67u, 0x42u, 0u, 0x1fu, 0u, 0u, 1u, 0x68u, 0xceu, 0x06u, 0u, 0u, 0u, 1u, 0x65u, 0x88u, 0x84u,
};

static NativeCameraFrame frame(const uint8_t *data, size_t len, uint32_t sequence, uint32_t flags) {
    NativeCameraFrame value = {
        .data = data,
        .len = len,
        .timestamp_us = 7000u + sequence,
        .index = 1u,
        .sequence = sequence,
        .flags = flags,
        .held = true,
    };
    return value;
}

static void expect_seed_packet(NativeCameraH264Stream *stream, uint32_t sequence) {
    NativeCameraH264Packet packet;
    assert(native_camera_h264_stream_peek(stream, &packet));
    assert(packet.sequence == sequence);
    assert(packet.timestamp_us == 7000u + sequence);
    assert(packet.is_idr);
    assert(packet.len <= NATIVE_CAMERA_H264_MAX_AU_BYTES);
    NativeH264Info info;
    assert(native_h264_scan_annexb(packet.data, packet.len, &info) == NATIVE_H264_OK);
    assert(info.can_seed_decoder);
}

static void test_parameter_cache_and_seed_gate(void) {
    NativeCameraH264Stream *stream = native_camera_h264_stream_create();
    assert(stream);
    NativeCameraFrame input = frame(SPS, sizeof(SPS), 10u, 0u);
    assert(native_camera_h264_stream_ingest(stream, &input) == NATIVE_CAMERA_H264_INGEST_WAITING);
    input = frame(PPS, sizeof(PPS), 11u, 0u);
    assert(native_camera_h264_stream_ingest(stream, &input) == NATIVE_CAMERA_H264_INGEST_WAITING);
    input = frame(PICTURE, sizeof(PICTURE), 12u, 0u);
    assert(native_camera_h264_stream_ingest(stream, &input) == NATIVE_CAMERA_H264_INGEST_WAITING);
    input = frame(IDR, sizeof(IDR), 13u, 0u);
    assert(native_camera_h264_stream_ingest(stream, &input) == NATIVE_CAMERA_H264_INGEST_QUEUED);
    assert(native_camera_h264_stream_queued_aus(stream) == 1u);
    expect_seed_packet(stream, 13u);
    native_camera_h264_stream_pop(stream);

    input = frame(PICTURE, sizeof(PICTURE), 14u, 0u);
    assert(native_camera_h264_stream_ingest(stream, &input) == NATIVE_CAMERA_H264_INGEST_QUEUED);
    NativeCameraH264Packet packet;
    assert(native_camera_h264_stream_peek(stream, &packet));
    assert(!packet.is_idr && packet.sequence == 14u && packet.len == sizeof(PICTURE));
    native_camera_h264_stream_destroy(stream);
}

static void test_recovery_inputs(void) {
    NativeCameraH264Stream *stream = native_camera_h264_stream_create();
    assert(stream);
    /* The V4L2 counter wraps without losing a buffer: UINT32_MAX is followed by 0. */
    NativeCameraFrame input = frame(SEED, sizeof(SEED), UINT32_MAX, 0u);
    assert(native_camera_h264_stream_ingest(stream, &input) == NATIVE_CAMERA_H264_INGEST_QUEUED);
    input = frame(PICTURE, sizeof(PICTURE), 0u, 0u);
    assert(native_camera_h264_stream_ingest(stream, &input) == NATIVE_CAMERA_H264_INGEST_QUEUED);
    assert(native_camera_h264_stream_queued_aus(stream) == 2u);

    /* A driver-flagged corrupt buffer is a real fault, unlike a dropped one:
     * discard the FIFO and seed state so the caller reopens the node. */
    input = frame(PICTURE, sizeof(PICTURE), 1u, V4L2_BUF_FLAG_ERROR);
    assert(native_camera_h264_stream_ingest(stream, &input) == NATIVE_CAMERA_H264_INGEST_RECOVER);
    assert(native_camera_h264_stream_queued_aus(stream) == 0u);

    static const uint8_t avc[] = {0u, 0u, 0u, 2u, 0x65u, 0x88u};
    input = frame(avc, sizeof(avc), 40u, 0u);
    assert(native_camera_h264_stream_ingest(stream, &input) == NATIVE_CAMERA_H264_INGEST_RECOVER);
    static const uint8_t empty_nal[] = {0u, 0u, 1u, 0u, 0u, 1u, 0x65u};
    input = frame(empty_nal, sizeof(empty_nal), 41u, 0u);
    assert(native_camera_h264_stream_ingest(stream, &input) == NATIVE_CAMERA_H264_INGEST_RECOVER);
    uint8_t byte = 0u;
    input = frame(&byte, NATIVE_CAMERA_H264_MAX_AU_BYTES + 1u, 42u, 0u);
    assert(native_camera_h264_stream_ingest(stream, &input) == NATIVE_CAMERA_H264_INGEST_RECOVER);
    native_camera_h264_stream_destroy(stream);
}

static void test_sequence_gap_keeps_capture(void) {
    NativeCameraH264Stream *stream = native_camera_h264_stream_create();
    assert(stream);
    NativeCameraFrame input = frame(SEED, sizeof(SEED), 10u, 0u);
    assert(native_camera_h264_stream_ingest(stream, &input) == NATIVE_CAMERA_H264_INGEST_QUEUED);
    input = frame(PICTURE, sizeof(PICTURE), 11u, 0u);
    assert(native_camera_h264_stream_ingest(stream, &input) == NATIVE_CAMERA_H264_INGEST_QUEUED);
    assert(native_camera_h264_stream_queued_aus(stream) == 2u);

    /* Dropped buffers must not reopen the node, and must not discard AUs that
     * predate the gap: those still decode against everything already sent. The
     * pending credit fails immediately rather than after another wait bound. */
    input = frame(PICTURE, sizeof(PICTURE), 14u, 0u);
    assert(native_camera_h264_stream_ingest(stream, &input) == NATIVE_CAMERA_H264_INGEST_SAMPLE_ERROR);
    assert(native_camera_h264_stream_queued_aus(stream) == 2u);

    /* Pictures after the gap depend on frames that never arrived, so they stay
     * gated instead of being forwarded as a broken chain. */
    input = frame(PICTURE, sizeof(PICTURE), 15u, 0u);
    assert(native_camera_h264_stream_ingest(stream, &input) == NATIVE_CAMERA_H264_INGEST_WAITING);
    assert(native_camera_h264_stream_queued_aus(stream) == 2u);

    /* Cached SPS/PPS survive the gap, so a bare IDR is still normalized into a
     * self-contained seed; a reopen would have thrown that cache away. */
    input = frame(IDR, sizeof(IDR), 16u, 0u);
    assert(native_camera_h264_stream_ingest(stream, &input) == NATIVE_CAMERA_H264_INGEST_QUEUED);
    assert(native_camera_h264_stream_queued_aus(stream) == 3u);
    native_camera_h264_stream_pop(stream);
    native_camera_h264_stream_pop(stream);
    expect_seed_packet(stream, 16u);
    native_camera_h264_stream_pop(stream);

    /* An IDR that lands exactly on the gap re-seeds without spending a credit. */
    input = frame(SEED, sizeof(SEED), 40u, 0u);
    assert(native_camera_h264_stream_ingest(stream, &input) == NATIVE_CAMERA_H264_INGEST_QUEUED);
    expect_seed_packet(stream, 40u);
    native_camera_h264_stream_pop(stream);

    /* Repeated gaps keep the node open however often they happen. */
    for (uint32_t i = 0u; i < 3u; i++) {
        input = frame(PICTURE, sizeof(PICTURE), 100u + 10u * i, 0u);
        assert(native_camera_h264_stream_ingest(stream, &input) == NATIVE_CAMERA_H264_INGEST_SAMPLE_ERROR);
    }
    native_camera_h264_stream_destroy(stream);
}

static void test_sendable_au_wait_bound(void) {
    NativeCameraH264Stream *stream = native_camera_h264_stream_create();
    assert(stream);
    NativeCameraFrame input = frame(SPS, sizeof(SPS), 50u, 0u);
    assert(native_camera_h264_stream_ingest(stream, &input) == NATIVE_CAMERA_H264_INGEST_WAITING);
    input = frame(PPS, sizeof(PPS), 51u, 0u);
    assert(native_camera_h264_stream_ingest(stream, &input) == NATIVE_CAMERA_H264_INGEST_WAITING);

    /* A camera may start in the middle of a long GOP. Periodically fail only the
     * waiting credit; keep sequence and cached SPS/PPS until the next IDR. */
    for (uint32_t sequence = 52u; sequence < 80u; sequence++) {
        input = frame(PICTURE, sizeof(PICTURE), sequence, 0u);
        NativeCameraH264IngestResult expected = (sequence - 49u) % NATIVE_CAMERA_H264_WAIT_AUS == 0u
                                                    ? NATIVE_CAMERA_H264_INGEST_SAMPLE_ERROR
                                                    : NATIVE_CAMERA_H264_INGEST_WAITING;
        assert(native_camera_h264_stream_ingest(stream, &input) == expected);
    }
    assert(native_camera_h264_stream_queued_aus(stream) == 0u);
    input = frame(IDR, sizeof(IDR), 80u, 0u);
    assert(native_camera_h264_stream_ingest(stream, &input) == NATIVE_CAMERA_H264_INGEST_QUEUED);
    expect_seed_packet(stream, 80u);
    native_camera_h264_stream_pop(stream);

    native_camera_h264_stream_reset(stream);
    for (uint32_t i = 0u; i < 2u * NATIVE_CAMERA_H264_WAIT_AUS; i++) {
        input = frame(IDR, sizeof(IDR), 100u + i, 0u);
        NativeCameraH264IngestResult expected = i + 1u == NATIVE_CAMERA_H264_WAIT_AUS
                                                    || i + 1u == 2u * NATIVE_CAMERA_H264_WAIT_AUS
                                                ? NATIVE_CAMERA_H264_INGEST_SAMPLE_ERROR
                                                : NATIVE_CAMERA_H264_INGEST_WAITING;
        assert(native_camera_h264_stream_ingest(stream, &input) == expected);
    }

    input = frame(SEED, sizeof(SEED), 100u + 2u * NATIVE_CAMERA_H264_WAIT_AUS, 0u);
    assert(native_camera_h264_stream_ingest(stream, &input) == NATIVE_CAMERA_H264_INGEST_QUEUED);
    expect_seed_packet(stream, 100u + 2u * NATIVE_CAMERA_H264_WAIT_AUS);
    native_camera_h264_stream_destroy(stream);
}

static void test_fifo_bounds(void) {
    NativeCameraH264Stream *stream = native_camera_h264_stream_create();
    assert(stream);
    NativeCameraFrame input = frame(SEED, sizeof(SEED), 100u, 0u);
    assert(native_camera_h264_stream_ingest(stream, &input) == NATIVE_CAMERA_H264_INGEST_QUEUED);
    for (uint32_t sequence = 101u; sequence < 108u; sequence++) {
        input = frame(PICTURE, sizeof(PICTURE), sequence, 0u);
        assert(native_camera_h264_stream_ingest(stream, &input) == NATIVE_CAMERA_H264_INGEST_QUEUED);
    }
    assert(native_camera_h264_stream_queued_aus(stream) == NATIVE_CAMERA_H264_FIFO_AUS);
    input = frame(PICTURE, sizeof(PICTURE), 108u, 0u);
    assert(native_camera_h264_stream_ingest(stream, &input) == NATIVE_CAMERA_H264_INGEST_RECOVER);
    assert(native_camera_h264_stream_queued_aus(stream) == 0u);

    input = frame(SEED, sizeof(SEED), 200u, 0u);
    assert(native_camera_h264_stream_ingest(stream, &input) == NATIVE_CAMERA_H264_INGEST_QUEUED);
    const size_t large_len = 3u * 1024u * 1024u;
    uint8_t *large = (uint8_t *)malloc(large_len);
    assert(large);
    memset(large, 0x55, large_len);
    large[0] = 0u;
    large[1] = 0u;
    large[2] = 0u;
    large[3] = 1u;
    large[4] = 0x41u;
    input = frame(large, large_len, 201u, 0u);
    assert(native_camera_h264_stream_ingest(stream, &input) == NATIVE_CAMERA_H264_INGEST_QUEUED);
    input.sequence = 202u;
    input.timestamp_us++;
    assert(native_camera_h264_stream_ingest(stream, &input) == NATIVE_CAMERA_H264_INGEST_QUEUED);
    input.sequence = 203u;
    input.timestamp_us++;
    assert(native_camera_h264_stream_ingest(stream, &input) == NATIVE_CAMERA_H264_INGEST_RECOVER);
    assert(native_camera_h264_stream_queued_bytes(stream) == 0u);
    free(large);
    native_camera_h264_stream_destroy(stream);
}

static void test_normalized_au_cap(void) {
    NativeCameraH264Stream *stream = native_camera_h264_stream_create();
    assert(stream);
    size_t parameter_len = NATIVE_CAMERA_H264_MAX_AU_BYTES / 2u + 64u;
    uint8_t *sps = (uint8_t *)malloc(parameter_len);
    uint8_t *pps = (uint8_t *)malloc(parameter_len);
    assert(sps && pps);
    memset(sps, 0x55, parameter_len);
    memset(pps, 0x55, parameter_len);
    sps[0] = pps[0] = 0u;
    sps[1] = pps[1] = 0u;
    sps[2] = pps[2] = 0u;
    sps[3] = pps[3] = 1u;
    sps[4] = 0x67u;
    pps[4] = 0x68u;

    NativeCameraFrame input = frame(sps, parameter_len, 300u, 0u);
    assert(native_camera_h264_stream_ingest(stream, &input) == NATIVE_CAMERA_H264_INGEST_WAITING);
    input = frame(pps, parameter_len, 301u, 0u);
    assert(native_camera_h264_stream_ingest(stream, &input) == NATIVE_CAMERA_H264_INGEST_RECOVER);
    assert(native_camera_h264_stream_queued_aus(stream) == 0u);

    free(sps);
    free(pps);
    native_camera_h264_stream_destroy(stream);
}

int main(void) {
    test_parameter_cache_and_seed_gate();
    test_recovery_inputs();
    test_sequence_gap_keeps_capture();
    test_sendable_au_wait_bound();
    test_fifo_bounds();
    test_normalized_au_cap();
    return 0;
}
