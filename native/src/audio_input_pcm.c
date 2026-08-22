#include "audio_input_pcm.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "clog.h"

clog_define(g_native_log_audio_input_pcm, cLogLevelInfo, cLogFlags_Default, "audio.input.pcm", NULL);

typedef struct NativeAudioInputPcmImpl {
    uint32_t input_rate;
    uint16_t input_channels;
    uint32_t frames_per_packet;
    int32_t gain_q15;
    NativeAudioInputPacketFn packet;
    void *packet_ctx;
    int16_t *packet_samples;
    size_t packet_frames;
    uint64_t input_index;
    uint64_t next_output_numerator;
    int16_t previous_left;
    int16_t previous_right;
    bool have_previous;
} NativeAudioInputPcmImpl;

static int16_t clamp_s16(int64_t sample) {
    if (sample > INT16_MAX) {
        return INT16_MAX;
    }
    if (sample < INT16_MIN) {
        return INT16_MIN;
    }
    return (int16_t)sample;
}

static int16_t apply_gain(int32_t gain_q15, int32_t sample) {
    int64_t scaled = (int64_t)sample * gain_q15;
    if (scaled >= 0) {
        scaled += 16384;
    } else {
        scaled -= 16384;
    }
    return clamp_s16(scaled / 32768);
}

static void emit_frame(NativeAudioInputPcmImpl *impl, int16_t left, int16_t right) {
    size_t offset = impl->packet_frames * NATIVE_AUDIO_INPUT_WIRE_CHANNELS;
    impl->packet_samples[offset] = apply_gain(impl->gain_q15, left);
    impl->packet_samples[offset + 1u] = apply_gain(impl->gain_q15, right);
    impl->packet_frames++;
    if (impl->packet_frames == impl->frames_per_packet) {
        impl->packet(impl->packet_ctx, impl->packet_samples, impl->packet_frames);
        impl->packet_frames = 0;
    }
}

bool native_audio_input_pcm_init(NativeAudioInputPcm *pcm, uint32_t input_rate, uint16_t input_channels,
                                 uint32_t frames_per_packet, int16_t gain_db, NativeAudioInputPacketFn packet,
                                 void *packet_ctx) {
    if (!pcm || !packet || input_rate < 8000u || input_rate > 192000u ||
        (input_channels != 1u && input_channels != 2u) || frames_per_packet == 0u || frames_per_packet > 4096u ||
        gain_db < -12 || gain_db > 18) {
        return false;
    }
    memset(pcm, 0, sizeof(*pcm));
    NativeAudioInputPcmImpl *impl = (NativeAudioInputPcmImpl *)calloc(1, sizeof(*impl));
    if (!impl) {
        return false;
    }
    size_t sample_count = (size_t)frames_per_packet * NATIVE_AUDIO_INPUT_WIRE_CHANNELS;
    if (sample_count > SIZE_MAX / sizeof(int16_t)) {
        free(impl);
        return false;
    }
    impl->packet_samples = (int16_t *)malloc(sample_count * sizeof(int16_t));
    if (!impl->packet_samples) {
        free(impl);
        return false;
    }
    float gain = powf(10.0f, (float)gain_db / 20.0f);
    impl->gain_q15 = (int32_t)lroundf(gain * 32768.0f);
    impl->input_rate = input_rate;
    impl->input_channels = input_channels;
    impl->frames_per_packet = frames_per_packet;
    impl->packet = packet;
    impl->packet_ctx = packet_ctx;
    pcm->impl = impl;
    clog(cLogLevelInfo, "PCM input converter ready: %u Hz/%u ch -> 44100 Hz/2 ch, packet=%u, gain=%d dB", input_rate,
         input_channels, frames_per_packet, gain_db);
    return true;
}

void native_audio_input_pcm_destroy(NativeAudioInputPcm *pcm) {
    if (!pcm || !pcm->impl) {
        return;
    }
    NativeAudioInputPcmImpl *impl = (NativeAudioInputPcmImpl *)pcm->impl;
    free(impl->packet_samples);
    free(impl);
    pcm->impl = NULL;
}

bool native_audio_input_pcm_push(NativeAudioInputPcm *pcm, const int16_t *samples, size_t frames) {
    if (!pcm || !pcm->impl || (!samples && frames != 0u)) {
        return false;
    }
    NativeAudioInputPcmImpl *impl = (NativeAudioInputPcmImpl *)pcm->impl;
    for (size_t frame = 0; frame < frames; frame++) {
        int16_t left = samples[frame * impl->input_channels];
        int16_t right = impl->input_channels == 1u ? left : samples[frame * impl->input_channels + 1u];
        uint64_t current_numerator = impl->input_index * NATIVE_AUDIO_INPUT_WIRE_RATE;
        if (!impl->have_previous) {
            impl->previous_left = left;
            impl->previous_right = right;
            impl->have_previous = true;
        }
        while (impl->next_output_numerator <= current_numerator) {
            int32_t out_left = left;
            int32_t out_right = right;
            if (impl->input_index > 0u) {
                uint64_t previous_numerator = (impl->input_index - 1u) * NATIVE_AUDIO_INPUT_WIRE_RATE;
                uint64_t fraction = impl->next_output_numerator - previous_numerator;
                if (fraction > NATIVE_AUDIO_INPUT_WIRE_RATE) {
                    fraction = NATIVE_AUDIO_INPUT_WIRE_RATE;
                }
                uint64_t inverse = NATIVE_AUDIO_INPUT_WIRE_RATE - fraction;
                out_left = (int32_t)(((int64_t)impl->previous_left * (int64_t)inverse +
                                      (int64_t)left * (int64_t)fraction) /
                                     NATIVE_AUDIO_INPUT_WIRE_RATE);
                out_right = (int32_t)(((int64_t)impl->previous_right * (int64_t)inverse +
                                       (int64_t)right * (int64_t)fraction) /
                                      NATIVE_AUDIO_INPUT_WIRE_RATE);
            }
            emit_frame(impl, (int16_t)out_left, (int16_t)out_right);
            impl->next_output_numerator += impl->input_rate;
        }
        impl->previous_left = left;
        impl->previous_right = right;
        impl->input_index++;
    }
    return true;
}
