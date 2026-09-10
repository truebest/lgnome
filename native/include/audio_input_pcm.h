#ifndef LGNOME_AUDIO_INPUT_PCM_H
#define LGNOME_AUDIO_INPUT_PCM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NATIVE_AUDIO_INPUT_WIRE_RATE 44100u
#define NATIVE_AUDIO_INPUT_WIRE_CHANNELS 2u

typedef void (*NativeAudioInputPacketFn)(void *ctx, const int16_t *samples, size_t frames);

/* Streaming S16 mono/stereo -> 44.1 kHz stereo packetizer. The state is opaque so the
 * capture manager never depends on resampler storage details. */
typedef struct NativeAudioInputPcm {
    void *impl;
} NativeAudioInputPcm;

bool native_audio_input_pcm_init(NativeAudioInputPcm *pcm, uint32_t input_rate, uint16_t input_channels,
                                 uint32_t frames_per_packet, int16_t gain_db,
                                 NativeAudioInputPacketFn packet, void *packet_ctx);
void native_audio_input_pcm_destroy(NativeAudioInputPcm *pcm);
bool native_audio_input_pcm_push(NativeAudioInputPcm *pcm, const int16_t *samples, size_t frames);

#endif
