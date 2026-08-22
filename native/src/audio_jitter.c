#include "audio_jitter.h"

#include <limits.h>
#include <string.h>

#include "clog.h"

clog_define(g_native_log_audio_jitter, cLogLevelInfo, cLogFlags_Default, "audio.jitter", NULL);

#define NATIVE_AUDIO_JITTER_WINDOW_MS 10000u
#define NATIVE_AUDIO_JITTER_TARGET_DECAY_MS 5000u
#define NATIVE_AUDIO_JITTER_TARGET_DECAY_STEP_MS 10u
#define NATIVE_AUDIO_JITTER_LONG_GAP_MS 500u

static uint32_t clamp_u32(uint32_t value, uint32_t low, uint32_t high) {
    if (value < low) {
        return low;
    }
    if (value > high) {
        return high;
    }
    return value;
}

static void jitter_samples_clear(NativeAudioJitterController *controller) {
    memset(controller->histogram, 0, sizeof(controller->histogram));
    controller->head = 0;
    controller->count = 0;
}

void native_audio_jitter_reset(NativeAudioJitterController *controller, uint64_t now_ms, unsigned underruns) {
    if (!controller) {
        return;
    }
    memset(controller, 0, sizeof(*controller));
    controller->last_target_change_ms = now_ms;
    controller->seen_underruns = underruns;
    clog(cLogLevelTrace, "jitter controller reset");
}

void native_audio_jitter_mark_discontinuity(NativeAudioJitterController *controller) {
    if (controller) {
        controller->have_arrival = false;
    }
}

static void jitter_remove_oldest(NativeAudioJitterController *controller) {
    if (controller->count == 0) {
        return;
    }
    NativeAudioJitterSample *sample = &controller->samples[controller->head];
    if (sample->bucket < NATIVE_AUDIO_JITTER_BUCKETS && controller->histogram[sample->bucket] > 0) {
        controller->histogram[sample->bucket]--;
    }
    controller->head = (controller->head + 1u) % NATIVE_AUDIO_JITTER_SAMPLES;
    controller->count--;
}

static void jitter_expire(NativeAudioJitterController *controller, uint64_t now_ms) {
    while (controller->count > 0) {
        const NativeAudioJitterSample *sample = &controller->samples[controller->head];
        if (now_ms >= sample->arrival_ms && now_ms - sample->arrival_ms <= NATIVE_AUDIO_JITTER_WINDOW_MS) {
            break;
        }
        jitter_remove_oldest(controller);
    }
}

static uint32_t jitter_p95(const NativeAudioJitterController *controller) {
    if (controller->count == 0) {
        return 0;
    }
    unsigned threshold = (controller->count * 95u + 99u) / 100u;
    unsigned seen = 0;
    for (unsigned bucket = 0; bucket < NATIVE_AUDIO_JITTER_BUCKETS; bucket++) {
        seen += controller->histogram[bucket];
        if (seen >= threshold) {
            return bucket * NATIVE_AUDIO_JITTER_BUCKET_MS;
        }
    }
    return (NATIVE_AUDIO_JITTER_BUCKETS - 1u) * NATIVE_AUDIO_JITTER_BUCKET_MS;
}

static uint32_t jitter_add(NativeAudioJitterController *controller, uint64_t now_ms, uint32_t variation_ms) {
    jitter_expire(controller, now_ms);
    if (controller->count == NATIVE_AUDIO_JITTER_SAMPLES) {
        jitter_remove_oldest(controller);
    }
    unsigned bucket = variation_ms / NATIVE_AUDIO_JITTER_BUCKET_MS;
    if (bucket >= NATIVE_AUDIO_JITTER_BUCKETS) {
        bucket = NATIVE_AUDIO_JITTER_BUCKETS - 1u;
    }
    unsigned tail = (controller->head + controller->count) % NATIVE_AUDIO_JITTER_SAMPLES;
    controller->samples[tail].arrival_ms = now_ms;
    controller->samples[tail].bucket = (uint8_t)bucket;
    controller->histogram[bucket]++;
    controller->count++;
    return jitter_p95(controller);
}

NativeAudioJitterUpdate native_audio_jitter_update(NativeAudioJitterController *controller, size_t frames,
                                                   uint32_t sample_rate, uint32_t timestamp_ms, uint64_t now_ms,
                                                   unsigned underruns, uint32_t current_target_ms) {
    NativeAudioJitterUpdate update = {
        .target_delay_ms = current_target_ms,
    };
    if (!controller) {
        return update;
    }
    update.jitter_p95_ms = jitter_p95(controller);

    uint32_t duration_ms = sample_rate ? (uint32_t)(((uint64_t)frames * 1000u + sample_rate / 2u) / sample_rate) : 0;
    if (duration_ms == 0) {
        duration_ms = 1;
    }

    if (!controller->have_arrival || now_ms < controller->last_arrival_ms) {
        controller->have_arrival = true;
        controller->last_arrival_ms = now_ms;
        controller->last_timestamp_ms = timestamp_ms;
        controller->last_target_change_ms = now_ms;
        return update;
    }

    uint64_t arrival_delta_64 = now_ms - controller->last_arrival_ms;
    uint32_t arrival_delta = arrival_delta_64 > UINT32_MAX ? UINT32_MAX : (uint32_t)arrival_delta_64;
    if (arrival_delta > NATIVE_AUDIO_JITTER_LONG_GAP_MS) {
        jitter_samples_clear(controller);
        update.jitter_p95_ms = 0;
        update.target_delay_ms = NATIVE_AUDIO_JITTER_TARGET_INITIAL_MS;
        update.target_action = NATIVE_AUDIO_JITTER_TARGET_SET;
        update.talkspurt = true;
        controller->last_target_change_ms = now_ms;
        controller->last_arrival_ms = now_ms;
        controller->last_timestamp_ms = timestamp_ms;
        return update;
    }

    uint32_t expected_ms = duration_ms;
    if (timestamp_ms != 0 && controller->last_timestamp_ms != 0) {
        uint32_t timestamp_delta = timestamp_ms - controller->last_timestamp_ms;
        if (timestamp_delta != 0 && timestamp_delta <= NATIVE_AUDIO_JITTER_LONG_GAP_MS) {
            expected_ms = timestamp_delta;
        }
    }
    uint32_t variation_ms = arrival_delta > expected_ms ? arrival_delta - expected_ms : expected_ms - arrival_delta;
    update.jitter_p95_ms = jitter_add(controller, now_ms, variation_ms);

    if (underruns != controller->seen_underruns) {
        controller->seen_underruns = underruns;
        controller->last_target_change_ms = now_ms;
    }

    uint32_t peak_target = clamp_u32(variation_ms + NATIVE_AUDIO_JITTER_TARGET_HEADROOM_MS,
                                     NATIVE_AUDIO_JITTER_TARGET_MIN_MS, NATIVE_AUDIO_JITTER_TARGET_MAX_MS);
    if (peak_target > current_target_ms) {
        update.target_delay_ms = peak_target;
        update.target_action = NATIVE_AUDIO_JITTER_TARGET_RAISE;
        controller->last_target_change_ms = now_ms;
    } else {
        uint32_t calculated = clamp_u32(update.jitter_p95_ms + NATIVE_AUDIO_JITTER_TARGET_HEADROOM_MS,
                                        NATIVE_AUDIO_JITTER_TARGET_MIN_MS, NATIVE_AUDIO_JITTER_TARGET_MAX_MS);
        if (calculated < current_target_ms &&
            now_ms - controller->last_target_change_ms >= NATIVE_AUDIO_JITTER_TARGET_DECAY_MS) {
            uint32_t reduced = current_target_ms > NATIVE_AUDIO_JITTER_TARGET_DECAY_STEP_MS
                                   ? current_target_ms - NATIVE_AUDIO_JITTER_TARGET_DECAY_STEP_MS
                                   : NATIVE_AUDIO_JITTER_TARGET_MIN_MS;
            if (reduced < calculated) {
                reduced = calculated;
            }
            update.target_delay_ms = reduced;
            update.target_action = NATIVE_AUDIO_JITTER_TARGET_DECAY;
            controller->last_target_change_ms = now_ms;
        }
    }

    controller->last_arrival_ms = now_ms;
    controller->last_timestamp_ms = timestamp_ms;
    return update;
}
