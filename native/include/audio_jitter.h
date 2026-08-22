#ifndef GNOMECAST_AUDIO_JITTER_H
#define GNOMECAST_AUDIO_JITTER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NATIVE_AUDIO_JITTER_TARGET_INITIAL_MS 60u
#define NATIVE_AUDIO_JITTER_TARGET_MIN_MS 40u
#define NATIVE_AUDIO_JITTER_TARGET_MAX_MS 150u
#define NATIVE_AUDIO_JITTER_TARGET_HEADROOM_MS 20u

#define NATIVE_AUDIO_JITTER_BUCKET_MS 5u
#define NATIVE_AUDIO_JITTER_BUCKETS 31u
#define NATIVE_AUDIO_JITTER_SAMPLES 2048u

typedef struct NativeAudioJitterSample {
    uint64_t arrival_ms;
    uint8_t bucket;
} NativeAudioJitterSample;

typedef struct NativeAudioJitterController {
    bool have_arrival;
    uint64_t last_arrival_ms;
    uint32_t last_timestamp_ms;
    uint64_t last_target_change_ms;
    unsigned seen_underruns;
    NativeAudioJitterSample samples[NATIVE_AUDIO_JITTER_SAMPLES];
    uint16_t histogram[NATIVE_AUDIO_JITTER_BUCKETS];
    unsigned head;
    unsigned count;
} NativeAudioJitterController;

typedef enum NativeAudioJitterTargetAction {
    NATIVE_AUDIO_JITTER_TARGET_UNCHANGED = 0,
    NATIVE_AUDIO_JITTER_TARGET_SET,
    NATIVE_AUDIO_JITTER_TARGET_RAISE,
    NATIVE_AUDIO_JITTER_TARGET_DECAY,
} NativeAudioJitterTargetAction;

typedef struct NativeAudioJitterUpdate {
    uint32_t jitter_p95_ms;
    uint32_t target_delay_ms;
    NativeAudioJitterTargetAction target_action;
    bool talkspurt;
} NativeAudioJitterUpdate;

void native_audio_jitter_reset(NativeAudioJitterController *controller, uint64_t now_ms, unsigned underruns);
void native_audio_jitter_mark_discontinuity(NativeAudioJitterController *controller);
NativeAudioJitterUpdate native_audio_jitter_update(NativeAudioJitterController *controller, size_t frames,
                                                   uint32_t sample_rate, uint32_t timestamp_ms, uint64_t now_ms,
                                                   unsigned underruns, uint32_t current_target_ms);

#endif
