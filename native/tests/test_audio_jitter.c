#include "audio_jitter.h"

#include <assert.h>
#include <stdint.h>

static NativeAudioJitterUpdate update(NativeAudioJitterController *controller, uint64_t now_ms, uint32_t timestamp_ms,
                                      unsigned underruns, uint32_t target_ms) {
    return native_audio_jitter_update(controller, 960u, 48000u, timestamp_ms, now_ms, underruns, target_ms);
}

static void test_growth_decay_and_talkspurt(void) {
    NativeAudioJitterController controller;
    native_audio_jitter_reset(&controller, 0u, 0u);

    NativeAudioJitterUpdate result = update(&controller, 20u, 20u, 0u, 60u);
    assert(result.target_action == NATIVE_AUDIO_JITTER_TARGET_UNCHANGED);
    result = update(&controller, 40u, 40u, 0u, 60u);
    assert(result.jitter_p95_ms == 0u);

    result = update(&controller, 100u, 60u, 0u, 60u);
    assert(result.target_action == NATIVE_AUDIO_JITTER_TARGET_UNCHANGED);
    result = update(&controller, 330u, 80u, 0u, 60u);
    assert(result.target_action == NATIVE_AUDIO_JITTER_TARGET_RAISE);
    assert(result.target_delay_ms == NATIVE_AUDIO_JITTER_TARGET_MAX_MS);

    result = update(&controller, 5350u, 100u, 0u, result.target_delay_ms);
    assert(result.target_action == NATIVE_AUDIO_JITTER_TARGET_SET);
    assert(result.talkspurt);
    assert(result.jitter_p95_ms == 0u);
    assert(result.target_delay_ms == NATIVE_AUDIO_JITTER_TARGET_INITIAL_MS);

    result = update(&controller, 5370u, 120u, 0u, 100u);
    assert(result.target_action == NATIVE_AUDIO_JITTER_TARGET_UNCHANGED);
    result = update(&controller, 10370u, 140u, 0u, 100u);
    assert(result.target_action == NATIVE_AUDIO_JITTER_TARGET_SET);
    assert(result.talkspurt);
}

static void test_quantile_bucket_clamp_and_expiry(void) {
    NativeAudioJitterController controller;
    native_audio_jitter_reset(&controller, 0u, 0u);
    (void)update(&controller, 20u, 20u, 0u, 60u);

    uint64_t now_ms = 20u;
    uint32_t timestamp_ms = 20u;
    NativeAudioJitterUpdate result = {0};
    for (unsigned i = 0; i < 18u; i++) {
        now_ms += 20u;
        timestamp_ms += 20u;
        result = update(&controller, now_ms, timestamp_ms, 0u, 60u);
    }
    assert(result.jitter_p95_ms == 0u);

    now_ms += 170u;
    timestamp_ms += 20u;
    result = update(&controller, now_ms, timestamp_ms, 0u, 60u);
    assert(result.jitter_p95_ms == 150u);
    now_ms += 170u;
    timestamp_ms += 20u;
    result = update(&controller, now_ms, timestamp_ms, 0u, 150u);
    assert(result.jitter_p95_ms == 150u);

    native_audio_jitter_reset(&controller, 0u, 0u);
    (void)update(&controller, 1u, 1u, 0u, 60u);
    for (unsigned i = 0; i < NATIVE_AUDIO_JITTER_SAMPLES + 1u; i++) {
        result = update(&controller, 1u, 1u, 0u, 60u);
    }
    assert(controller.count == NATIVE_AUDIO_JITTER_SAMPLES);
    result = update(&controller, 10002u, 1u, 0u, 60u);
    assert(controller.count == 0u);
    assert(result.talkspurt);
    assert(result.jitter_p95_ms == 0u);
}

static void test_decay_and_underrun_holdoff(void) {
    NativeAudioJitterController controller;
    native_audio_jitter_reset(&controller, 0u, 0u);
    (void)update(&controller, 20u, 20u, 0u, 100u);
    NativeAudioJitterUpdate result = {0};
    for (uint32_t now_ms = 40u; now_ms <= 5020u; now_ms += 20u) {
        result = update(&controller, now_ms, now_ms, 0u, 100u);
    }
    assert(result.target_action == NATIVE_AUDIO_JITTER_TARGET_DECAY);
    assert(result.target_delay_ms == 90u);

    native_audio_jitter_reset(&controller, 0u, 0u);
    (void)update(&controller, 20u, 20u, 0u, 100u);
    for (uint32_t now_ms = 40u; now_ms < 5020u; now_ms += 20u) {
        result = update(&controller, now_ms, now_ms, 0u, 100u);
    }
    result = update(&controller, 5020u, 5020u, 1u, 100u);
    assert(result.target_action == NATIVE_AUDIO_JITTER_TARGET_UNCHANGED);
    for (uint32_t now_ms = 5040u; now_ms <= 10020u; now_ms += 20u) {
        result = update(&controller, now_ms, now_ms, 1u, 100u);
    }
    assert(result.target_action == NATIVE_AUDIO_JITTER_TARGET_DECAY);
    assert(result.target_delay_ms == 90u);
}

int main(void) {
    test_growth_decay_and_talkspurt();
    test_quantile_bucket_clamp_and_expiry();
    test_decay_and_underrun_holdoff();
    return 0;
}
