#include "audio_input_pcm.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct Capture {
    int16_t packet[1024 * 2];
    size_t frames;
    unsigned calls;
} Capture;

static void on_packet(void *ctx, const int16_t *samples, size_t frames) {
    Capture *capture = (Capture *)ctx;
    assert(frames <= 1024u);
    memcpy(capture->packet, samples, frames * 2u * sizeof(int16_t));
    capture->frames = frames;
    capture->calls++;
}

static void test_mono_48k_packetizes_44k1_stereo(void) {
    NativeAudioInputPcm pcm = {0};
    Capture capture = {0};
    assert(native_audio_input_pcm_init(&pcm, 48000u, 1u, 441u, 0, on_packet, &capture));
    int16_t input[480];
    for (size_t i = 0; i < 480u; i++) {
        input[i] = (int16_t)i;
    }
    assert(native_audio_input_pcm_push(&pcm, input, 480u));
    assert(capture.calls == 1u);
    assert(capture.frames == 441u);
    for (size_t i = 0; i < capture.frames; i++) {
        assert(capture.packet[i * 2u] == capture.packet[i * 2u + 1u]);
    }
    native_audio_input_pcm_destroy(&pcm);
}

static void test_gain_clips_safely(void) {
    NativeAudioInputPcm pcm = {0};
    Capture capture = {0};
    assert(native_audio_input_pcm_init(&pcm, 44100u, 2u, 2u, 18, on_packet, &capture));
    int16_t input[] = {20000, -20000, 30000, -30000};
    assert(native_audio_input_pcm_push(&pcm, input, 2u));
    assert(capture.calls == 1u);
    assert(capture.packet[0] == INT16_MAX);
    assert(capture.packet[1] == INT16_MIN);
    assert(capture.packet[2] == INT16_MAX);
    assert(capture.packet[3] == INT16_MIN);
    native_audio_input_pcm_destroy(&pcm);
}

int main(void) {
    test_mono_48k_packetizes_44k1_stereo();
    test_gain_clips_safely();
    puts("PASS audio-input-pcm");
    return 0;
}
