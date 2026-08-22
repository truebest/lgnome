#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "audio_pipeline_internal.h"
#include "native_time.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "clog.h"

clog_define(g_native_log_audio, cLogLevelInfo, cLogFlags_Default, "audio.pipeline", NULL);

_Static_assert(MA_VERSION_MAJOR == 0 && MA_VERSION_MINOR == 11 && MA_VERSION_REVISION == 25,
               "gnomecast audio pipeline is pinned to miniaudio 0.11.25");
_Static_assert(ATOMIC_INT_LOCK_FREE == 2, "the audio SPSC cursors require lock-free 32-bit atomics");
_Static_assert((NATIVE_AUDIO_PIPELINE_CAPACITY_FRAMES & (NATIVE_AUDIO_PIPELINE_CAPACITY_FRAMES - 1u)) == 0,
               "the SPSC capacity must preserve ring positions across cursor wrap");

#define NATIVE_AUDIO_STATS_LOG_MS 3000u

static uint64_t native_audio_monotonic_ms(void *ctx) {
    (void)ctx;
    return native_monotonic_ms64();
}

static void pipeline_impl_cleanup(NativeAudioPipelineImpl *pipeline) {
    if (!pipeline) {
        return;
    }
    native_audio_sources_uninit(pipeline);
    if (pipeline->engine_initialized) {
        ma_engine_uninit(&pipeline->engine);
    }
    if (pipeline->control_lock_initialized) {
        pthread_mutex_destroy(&pipeline->control_lock);
    }
    free(pipeline);
}

bool native_audio_pipeline_init_with_clock(NativeAudioPipeline *pipeline, NativeAudioPipelineClock clock,
                                           void *clock_ctx) {
    if (!pipeline || pipeline->impl) {
        return false;
    }
    NativeAudioPipelineImpl *impl = (NativeAudioPipelineImpl *)calloc(1, sizeof(*impl));
    if (!impl) {
        return false;
    }
    impl->clock = clock ? clock : native_audio_monotonic_ms;
    impl->clock_ctx = clock_ctx;
    atomic_init(&impl->output_peak_left, 0u);
    atomic_init(&impl->output_peak_right, 0u);
    atomic_init(&impl->output_peak_when_ms, 0u);
    atomic_init(&impl->pump_stop, false);
    atomic_init(&impl->duck_foreground_index, -1);
    atomic_init(&impl->duck_trigger_mask, 0u);
    atomic_init(&impl->duck_factor_q15, (unsigned)NATIVE_AUDIO_PIPELINE_GAIN_UNITY_Q15);
    impl->duck_cfg_foreground = -1;
    atomic_init(&impl->solo_mask, 0u);
    impl->duck_applied_index = -1;
    impl->duck_factor = 1.0f;
    if (pthread_mutex_init(&impl->control_lock, NULL) != 0) {
        pipeline_impl_cleanup(impl);
        return false;
    }
    impl->control_lock_initialized = true;

    ma_engine_config engine_config = ma_engine_config_init();
    engine_config.noDevice = MA_TRUE;
    engine_config.channels = NATIVE_AUDIO_PIPELINE_CHANNELS;
    engine_config.sampleRate = NATIVE_AUDIO_PIPELINE_SAMPLE_RATE;
    engine_config.periodSizeInFrames = NATIVE_AUDIO_PIPELINE_BLOCK_FRAMES;
    engine_config.defaultVolumeSmoothTimeInPCMFrames = NATIVE_AUDIO_PIPELINE_BLOCK_FRAMES;
    if (ma_engine_init(&engine_config, &impl->engine) != MA_SUCCESS) {
        pipeline_impl_cleanup(impl);
        return false;
    }
    impl->engine_initialized = true;

    if (!native_audio_sources_init(impl)) {
        pipeline_impl_cleanup(impl);
        return false;
    }

    pipeline->impl = impl;
    return true;
}

bool native_audio_pipeline_init(NativeAudioPipeline *pipeline) {
    return native_audio_pipeline_init_with_clock(pipeline, NULL, NULL);
}

bool native_audio_pipeline_is_initialized(const NativeAudioPipeline *pipeline) {
    return pipeline && pipeline->impl;
}

void native_audio_pipeline_destroy(NativeAudioPipeline *pipeline) {
    if (!pipeline || !pipeline->impl) {
        return;
    }
    native_audio_pipeline_pump_stop(pipeline);
    NativeAudioPipelineImpl *impl = (NativeAudioPipelineImpl *)pipeline->impl;
    pipeline->impl = NULL;
    pipeline_impl_cleanup(impl);
}

void native_audio_pipeline_set_source_gain(NativeAudioPipeline *pipeline, int source_index, int32_t gain_q15) {
    if (!pipeline || !pipeline->impl || !native_audio_source_index_valid(source_index)) {
        return;
    }
    if (gain_q15 < 0) {
        gain_q15 = 0;
    }
    if (gain_q15 > NATIVE_AUDIO_PIPELINE_GAIN_MAX_Q15) {
        gain_q15 = NATIVE_AUDIO_PIPELINE_GAIN_MAX_Q15;
    }
    NativeAudioSource *source = &((NativeAudioPipelineImpl *)pipeline->impl)->sources[source_index];
    atomic_store_explicit(&source->gain_q15, gain_q15, memory_order_relaxed);
}

void native_audio_pipeline_set_source_muted(NativeAudioPipeline *pipeline, int source_index, bool muted) {
    if (!pipeline || !pipeline->impl || !native_audio_source_index_valid(source_index)) {
        return;
    }
    NativeAudioSource *source = &((NativeAudioPipelineImpl *)pipeline->impl)->sources[source_index];
    atomic_store_explicit(&source->muted, muted, memory_order_relaxed);
}

void native_audio_pipeline_set_solo_mask(NativeAudioPipeline *pipeline, uint32_t solo_mask) {
    if (!pipeline || !pipeline->impl) {
        return;
    }
    solo_mask &= (1u << NATIVE_AUDIO_PIPELINE_MAX_SOURCES) - 1u;
    NativeAudioPipelineImpl *impl = (NativeAudioPipelineImpl *)pipeline->impl;
    atomic_store_explicit(&impl->solo_mask, solo_mask, memory_order_relaxed);
}

void native_audio_pipeline_set_duck_foreground(NativeAudioPipeline *pipeline, int foreground, uint32_t trigger_mask) {
    if (!pipeline || !pipeline->impl) {
        return;
    }
    if (foreground < -1 || foreground >= NATIVE_AUDIO_PIPELINE_MAX_SOURCES) {
        foreground = -1;
    }
    trigger_mask &= (1u << NATIVE_AUDIO_PIPELINE_MAX_SOURCES) - 1u;
    NativeAudioPipelineImpl *impl = (NativeAudioPipelineImpl *)pipeline->impl;
    atomic_store_explicit(&impl->duck_trigger_mask, trigger_mask, memory_order_relaxed);
    atomic_store_explicit(&impl->duck_foreground_index, foreground, memory_order_relaxed);
}

int32_t native_audio_pipeline_get_duck_factor_q15(NativeAudioPipeline *pipeline) {
    if (!pipeline || !pipeline->impl) {
        return NATIVE_AUDIO_PIPELINE_GAIN_UNITY_Q15;
    }
    NativeAudioPipelineImpl *impl = (NativeAudioPipelineImpl *)pipeline->impl;
    return (int32_t)atomic_load_explicit(&impl->duck_factor_q15, memory_order_relaxed);
}

/* Runs on the render thread once per graph read, before the source callbacks. Activity
 * is judged on the previous block's post-fader peaks (one block of detection latency),
 * so a background source whose fader is pulled down never triggers a duck. Attack snaps
 * the factor (the per-source block ramp smooths it); release is a timed linear ramp
 * stepped per block. duck_bg_seen keeps a fresh pipeline from ducking on startup while
 * now - duck_last_active_ms is still trivially inside the hold window. */
static void pipeline_duck_update(NativeAudioPipelineImpl *impl) {
    uint64_t now = native_audio_pipeline_now_ms(impl);
    int fg = atomic_load_explicit(&impl->duck_foreground_index, memory_order_relaxed);
    unsigned mask = atomic_load_explicit(&impl->duck_trigger_mask, memory_order_relaxed);
    unsigned triggers = fg >= 0 ? (mask & ~(1u << fg)) : 0u;
    if (fg != impl->duck_cfg_foreground || triggers != impl->duck_cfg_triggers) {
        /* The activity history was collected under the previous routing: drop it, or a
         * hold armed by the OLD mask would transfer to a foreground whose own triggers
         * were quiet (e.g. right after a session switch). A trigger that is loud under
         * the new config re-arms on this very block, so a legitimate duck carries over
         * seamlessly. */
        impl->duck_cfg_foreground = fg;
        impl->duck_cfg_triggers = triggers;
        impl->duck_bg_seen = false;
    }
    float target = 1.0f;
    if (triggers != 0) {
        impl->duck_applied_index = fg;
        bool bg_active = false;
        for (int i = 0; i < NATIVE_AUDIO_PIPELINE_MAX_SOURCES; i++) {
            if (!(triggers & (1u << i))) {
                continue;
            }
            NativeAudioSource *source = &impl->sources[i];
            if (!atomic_load_explicit(&source->open, memory_order_relaxed)) {
                continue;
            }
            if (atomic_load_explicit(&source->peak_left, memory_order_relaxed) >= NATIVE_AUDIO_DUCK_PEAK_THRESHOLD ||
                atomic_load_explicit(&source->peak_right, memory_order_relaxed) >= NATIVE_AUDIO_DUCK_PEAK_THRESHOLD) {
                bg_active = true;
                break;
            }
        }
        if (bg_active) {
            impl->duck_bg_seen = true;
            impl->duck_last_active_ms = now;
        }
        if (impl->duck_bg_seen && now - impl->duck_last_active_ms < NATIVE_AUDIO_DUCK_HOLD_MS) {
            target = (float)NATIVE_AUDIO_DUCK_GAIN_Q15 / 32768.0f;
        }
    } else if (impl->duck_factor >= 1.0f) {
        /* Disabled (no foreground or empty trigger mask) and fully released: detach.
         * Until then the previously ducked source keeps riding the release ramp so a
         * disable doesn't snap the gain back — and the hold window is deliberately
         * skipped so a toggle-off starts releasing on the next block. */
        impl->duck_applied_index = -1;
        impl->duck_bg_seen = false;
    }
    if (target < impl->duck_factor) {
        impl->duck_factor = target;
    } else if (target > impl->duck_factor) {
        float step = (1.0f - (float)NATIVE_AUDIO_DUCK_GAIN_Q15 / 32768.0f) *
                     (float)(now - impl->duck_last_update_ms) / (float)NATIVE_AUDIO_DUCK_RELEASE_MS;
        impl->duck_factor += step;
        if (impl->duck_factor > target) {
            impl->duck_factor = target;
        }
    }
    impl->duck_last_update_ms = now;
    atomic_store_explicit(&impl->duck_factor_q15, (unsigned)(impl->duck_factor * 32768.0f), memory_order_relaxed);
}

bool native_audio_pipeline_read_f32(NativeAudioPipeline *pipeline, float *out, size_t frames) {
    if (!pipeline || !pipeline->impl || !out || frames == 0) {
        return false;
    }
    NativeAudioPipelineImpl *impl = (NativeAudioPipelineImpl *)pipeline->impl;
    pipeline_duck_update(impl);
    ma_uint64 frames_read = 0;
    ma_result result = ma_engine_read_pcm_frames(&impl->engine, out, frames, &frames_read);
    if (frames_read < frames) {
        memset(&out[(size_t)frames_read * NATIVE_AUDIO_PIPELINE_CHANNELS], 0,
               (frames - (size_t)frames_read) * NATIVE_AUDIO_PIPELINE_CHANNELS * sizeof(float));
    }
    float peak_left = 0.0f;
    float peak_right = 0.0f;
    for (size_t frame = 0; frame < frames; frame++) {
        float left = out[frame * 2];
        float right = out[frame * 2 + 1];
        left = left < 0.0f ? -left : left;
        right = right < 0.0f ? -right : right;
        if (left > peak_left) {
            peak_left = left;
        }
        if (right > peak_right) {
            peak_right = right;
        }
    }
    uint64_t now_ms = native_audio_pipeline_now_ms(impl);
    atomic_store_explicit(&impl->output_peak_left, (unsigned)native_audio_float_peak_to_i32(peak_left),
                          memory_order_relaxed);
    atomic_store_explicit(&impl->output_peak_right, (unsigned)native_audio_float_peak_to_i32(peak_right),
                          memory_order_relaxed);
    atomic_store_explicit(&impl->output_peak_when_ms, (unsigned)now_ms, memory_order_release);
    return result == MA_SUCCESS;
}

bool native_audio_pipeline_read_s16(NativeAudioPipeline *pipeline, int16_t *out, size_t frames) {
    if (!pipeline || !pipeline->impl || !out || frames == 0) {
        return false;
    }
    NativeAudioPipelineImpl *impl = (NativeAudioPipelineImpl *)pipeline->impl;
    size_t offset = 0;
    bool ok = true;
    while (offset < frames) {
        size_t block = frames - offset;
        if (block > NATIVE_AUDIO_PIPELINE_BLOCK_FRAMES) {
            block = NATIVE_AUDIO_PIPELINE_BLOCK_FRAMES;
        }
        if (!native_audio_pipeline_read_f32(pipeline, impl->conversion_buffer, block)) {
            ok = false;
        }
        ma_convert_pcm_frames_format(&out[offset * NATIVE_AUDIO_PIPELINE_CHANNELS], ma_format_s16,
                                     impl->conversion_buffer, ma_format_f32, block,
                                     NATIVE_AUDIO_PIPELINE_CHANNELS, ma_dither_mode_none);
        offset += block;
    }
    return ok;
}

static void get_stale_aware_peaks(NativeAudioPipelineImpl *impl, atomic_uint *left_peak, atomic_uint *right_peak,
                                  atomic_uint *when_ms, int32_t *left, int32_t *right) {
    if (left) {
        *left = 0;
    }
    if (right) {
        *right = 0;
    }
    unsigned when = atomic_load_explicit(when_ms, memory_order_acquire);
    unsigned now = (unsigned)native_audio_pipeline_now_ms(impl);
    if ((uint32_t)(now - when) > 100u) {
        return;
    }
    if (left) {
        *left = (int32_t)atomic_load_explicit(left_peak, memory_order_relaxed);
    }
    if (right) {
        *right = (int32_t)atomic_load_explicit(right_peak, memory_order_relaxed);
    }
}

void native_audio_pipeline_get_source_peaks(NativeAudioPipeline *pipeline, int source_index, int32_t *left,
                                            int32_t *right) {
    if (!pipeline || !pipeline->impl || !native_audio_source_index_valid(source_index)) {
        if (left) {
            *left = 0;
        }
        if (right) {
            *right = 0;
        }
        return;
    }
    NativeAudioPipelineImpl *impl = (NativeAudioPipelineImpl *)pipeline->impl;
    NativeAudioSource *source = &impl->sources[source_index];
    get_stale_aware_peaks(impl, &source->peak_left, &source->peak_right, &source->peak_when_ms, left, right);
}

void native_audio_pipeline_get_output_peaks(NativeAudioPipeline *pipeline, int32_t *left, int32_t *right) {
    if (!pipeline || !pipeline->impl) {
        if (left) {
            *left = 0;
        }
        if (right) {
            *right = 0;
        }
        return;
    }
    NativeAudioPipelineImpl *impl = (NativeAudioPipelineImpl *)pipeline->impl;
    get_stale_aware_peaks(impl, &impl->output_peak_left, &impl->output_peak_right, &impl->output_peak_when_ms,
                          left, right);
}

static void pipeline_log_stats(NativeAudioPipelineImpl *pipeline) {
    uint64_t now_ms = native_audio_pipeline_now_ms(pipeline);
    if (now_ms - pipeline->last_stats_log_ms < NATIVE_AUDIO_STATS_LOG_MS) {
        return;
    }
    pipeline->last_stats_log_ms = now_ms;
    for (int i = 0; i < NATIVE_AUDIO_PIPELINE_MAX_SOURCES; i++) {
        NativeAudioSource *source = &pipeline->sources[i];
        if (!atomic_load_explicit(&source->open, memory_order_relaxed)) {
            continue;
        }
        clog(cLogLevelDebug,
             "source %d queue=%ums target=%ums jitter-p95=%ums src=%dppm underruns=%u hard=%u overflow=%u",
             i, native_audio_source_queue_ms(source),
             atomic_load_explicit(&source->target_delay_ms, memory_order_relaxed),
             atomic_load_explicit(&source->jitter_p95_ms, memory_order_relaxed),
             atomic_load_explicit(&source->correction_ppm, memory_order_relaxed),
             atomic_load_explicit(&source->underruns, memory_order_relaxed),
             atomic_load_explicit(&source->hard_corrections, memory_order_relaxed),
             atomic_load_explicit(&source->overflows, memory_order_relaxed));
    }
}

static void *pipeline_pump_main(void *ctx) {
    NativeAudioPipelineImpl *pipeline = (NativeAudioPipelineImpl *)ctx;
    struct timespec next;
    clock_gettime(CLOCK_MONOTONIC, &next);
    while (!atomic_load_explicit(&pipeline->pump_stop, memory_order_acquire)) {
        NativeAudioPipeline wrapper = {.impl = pipeline};
        (void)native_audio_pipeline_read_s16(&wrapper, pipeline->pump_buffer,
                                             NATIVE_AUDIO_PIPELINE_BLOCK_FRAMES);
        pipeline->feed(pipeline->feed_ctx, pipeline->pump_buffer, NATIVE_AUDIO_PIPELINE_BLOCK_FRAMES);
        pipeline_log_stats(pipeline);

        native_timespec_add_ns(&next, 10000000u);
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (native_timespec_after(&now, &next)) {
            /* A blocked NDL feed is an outage, not cadence debt. Resume one block from
             * now; each source's backlog controller will trim its own old audio. */
            next = now;
            native_timespec_add_ns(&next, 10000000u);
        }
        while (!atomic_load_explicit(&pipeline->pump_stop, memory_order_acquire) &&
               clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL) == EINTR) {
        }
    }
    return NULL;
}

bool native_audio_pipeline_pump_start(NativeAudioPipeline *pipeline,
                                      void (*feed)(void *ctx, const int16_t *samples, size_t frames), void *feed_ctx) {
    if (!pipeline || !pipeline->impl || !feed) {
        return false;
    }
    NativeAudioPipelineImpl *impl = (NativeAudioPipelineImpl *)pipeline->impl;
    pthread_mutex_lock(&impl->control_lock);
    if (impl->pump_running) {
        pthread_mutex_unlock(&impl->control_lock);
        return false;
    }
    impl->feed = feed;
    impl->feed_ctx = feed_ctx;
    impl->last_stats_log_ms = native_audio_pipeline_now_ms(impl);
    atomic_store_explicit(&impl->pump_stop, false, memory_order_release);
    if (pthread_create(&impl->pump_thread, NULL, pipeline_pump_main, impl) != 0) {
        impl->feed = NULL;
        impl->feed_ctx = NULL;
        pthread_mutex_unlock(&impl->control_lock);
        return false;
    }
    impl->pump_running = true;
    pthread_mutex_unlock(&impl->control_lock);
    return true;
}

void native_audio_pipeline_pump_stop(NativeAudioPipeline *pipeline) {
    if (!pipeline || !pipeline->impl) {
        return;
    }
    NativeAudioPipelineImpl *impl = (NativeAudioPipelineImpl *)pipeline->impl;
    pthread_mutex_lock(&impl->control_lock);
    if (!impl->pump_running) {
        pthread_mutex_unlock(&impl->control_lock);
        return;
    }
    atomic_store_explicit(&impl->pump_stop, true, memory_order_release);
    pthread_join(impl->pump_thread, NULL);
    impl->pump_running = false;
    impl->feed = NULL;
    impl->feed_ctx = NULL;
    pthread_mutex_unlock(&impl->control_lock);
}
