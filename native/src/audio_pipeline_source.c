#include "audio_pipeline_internal.h"

#include <stdlib.h>
#include <string.h>

#include "clog.h"

clog_define(g_native_log_audio_source, cLogLevelInfo, "audio.source");

#define NATIVE_AUDIO_TARGET_INITIAL_MS NATIVE_AUDIO_JITTER_TARGET_INITIAL_MS
#define NATIVE_AUDIO_TARGET_MIN_MS NATIVE_AUDIO_JITTER_TARGET_MIN_MS
#define NATIVE_AUDIO_TARGET_MAX_MS NATIVE_AUDIO_JITTER_TARGET_MAX_MS
#define NATIVE_AUDIO_TARGET_HEADROOM_MS NATIVE_AUDIO_JITTER_TARGET_HEADROOM_MS
#define NATIVE_AUDIO_STALE_BURST_MS 100u
#define NATIVE_AUDIO_HARD_TRIM_ERROR_MS 80u
#define NATIVE_AUDIO_HARD_TRIM_KEEP_MS 20u
#define NATIVE_AUDIO_FADE_FRAMES 240u /* 5 ms at the fixed 48 kHz graph rate. */
#define NATIVE_AUDIO_MAX_CORRECTION_PPM 5000
#define NATIVE_AUDIO_CORRECTION_STEP_PPM 250

static uint32_t clamp_u32(uint32_t value, uint32_t low, uint32_t high) {
    if (value < low) {
        return low;
    }
    if (value > high) {
        return high;
    }
    return value;
}

static unsigned source_available_frames(const NativeAudioSource *source) {
    unsigned write = atomic_load_explicit(&source->write_cursor, memory_order_acquire);
    unsigned read = atomic_load_explicit(&source->read_cursor, memory_order_acquire);
    return write - read;
}

uint32_t native_audio_source_queue_ms(const NativeAudioSource *source) {
    uint32_t rate = atomic_load_explicit(&source->pending_sample_rate, memory_order_relaxed);
    return rate ? (uint32_t)((uint64_t)source_available_frames(source) * 1000u / rate) : 0;
}

static void source_raise_target(NativeAudioSource *source, uint32_t desired_ms) {
    desired_ms = clamp_u32(desired_ms, NATIVE_AUDIO_TARGET_MIN_MS, NATIVE_AUDIO_TARGET_MAX_MS);
    unsigned current = atomic_load_explicit(&source->target_delay_ms, memory_order_relaxed);
    while (current < desired_ms &&
           !atomic_compare_exchange_weak_explicit(&source->target_delay_ms, &current, desired_ms,
                                                  memory_order_relaxed, memory_order_relaxed)) {
    }
}

static void source_update_arrival_controller(NativeAudioSource *source, size_t frames, uint32_t timestamp_ms,
                                             uint32_t now_ms, unsigned write_boundary) {
    unsigned target = atomic_load_explicit(&source->target_delay_ms, memory_order_relaxed);
    unsigned underruns = atomic_load_explicit(&source->underruns, memory_order_relaxed);
    NativeAudioJitterUpdate update =
        native_audio_jitter_update(&source->jitter, frames, source->producer_sample_rate,
                                   timestamp_ms, now_ms, underruns, target);
    atomic_store_explicit(&source->jitter_p95_ms, update.jitter_p95_ms, memory_order_relaxed);

    if (update.target_action == NATIVE_AUDIO_JITTER_TARGET_SET) {
        atomic_store_explicit(&source->target_delay_ms, update.target_delay_ms, memory_order_relaxed);
    } else if (update.target_action == NATIVE_AUDIO_JITTER_TARGET_RAISE) {
        source_raise_target(source, update.target_delay_ms);
    } else if (update.target_action == NATIVE_AUDIO_JITTER_TARGET_DECAY) {
        (void)atomic_compare_exchange_strong_explicit(&source->target_delay_ms, &target,
                                                      update.target_delay_ms,
                                                      memory_order_relaxed, memory_order_relaxed);
    }

    if (update.talkspurt) {
        /* Silence/talkspurt boundary: discard PCM queued before the gap. */
        atomic_store_explicit(&source->talkspurt_cursor, write_boundary, memory_order_release);
        atomic_fetch_add_explicit(&source->talkspurt_generation, 1u, memory_order_release);
    }
}

static ma_data_converter_config source_converter_config(uint32_t sample_rate) {
    ma_data_converter_config config =
        ma_data_converter_config_init(ma_format_s16, ma_format_f32, NATIVE_AUDIO_PIPELINE_CHANNELS,
                                      NATIVE_AUDIO_PIPELINE_CHANNELS, sample_rate,
                                      NATIVE_AUDIO_PIPELINE_SAMPLE_RATE);
    config.allowDynamicSampleRate = MA_TRUE;
    config.resampling.algorithm = ma_resample_algorithm_linear;
    config.resampling.linear.lpfOrder = 1;
    return config;
}

static bool source_reset_converter(NativeAudioSource *source) {
    if (source->converter_initialized) {
        ma_data_converter_uninit(&source->converter, NULL);
        source->converter_initialized = false;
    }
    ma_data_converter_config config = source_converter_config(source->sample_rate);
    if (ma_data_converter_init_preallocated(&config, source->converter_heap, &source->converter) != MA_SUCCESS) {
        return false;
    }
    source->converter_initialized = true;
    return true;
}

static void source_advance_read_cursor(NativeAudioSource *source, unsigned boundary) {
    unsigned read = atomic_load_explicit(&source->read_cursor, memory_order_relaxed);
    unsigned advance = boundary - read;
    if (advance <= NATIVE_AUDIO_PIPELINE_CAPACITY_FRAMES) {
        atomic_store_explicit(&source->read_cursor, boundary, memory_order_release);
    }
}

static bool source_apply_generation(NativeAudioSource *source) {
    unsigned generation = atomic_load_explicit(&source->format_generation, memory_order_acquire);
    if (generation == source->applied_generation) {
        return false;
    }
    source->sample_rate = atomic_load_explicit(&source->pending_sample_rate, memory_order_relaxed);
    source->channels = (uint16_t)atomic_load_explicit(&source->pending_channels, memory_order_relaxed);
    unsigned boundary = atomic_load_explicit(&source->reset_cursor, memory_order_acquire);
    source_advance_read_cursor(source, boundary);
    source->applied_generation = generation;
    source->live = false;
    source->rebuffering = true;
    source->trim_after_fade = false;
    source->fade_out_remaining = 0;
    source->fade_in_remaining = 0;
    source->current_correction_ppm = 0;
    source->drift_integral_ppm = 0;
    atomic_store_explicit(&source->correction_ppm, 0, memory_order_relaxed);
    (void)source_reset_converter(source);
    return true;
}

static bool source_apply_talkspurt(NativeAudioSource *source) {
    unsigned generation = atomic_load_explicit(&source->talkspurt_generation, memory_order_acquire);
    if (generation == source->seen_talkspurt_generation) {
        return false;
    }
    unsigned boundary = atomic_load_explicit(&source->talkspurt_cursor, memory_order_acquire);
    source_advance_read_cursor(source, boundary);
    source->seen_talkspurt_generation = generation;
    source->live = false;
    source->rebuffering = true;
    source->trim_after_fade = false;
    source->fade_out_remaining = 0;
    source->fade_in_remaining = 0;
    source->current_correction_ppm = 0;
    atomic_store_explicit(&source->correction_ppm, 0, memory_order_relaxed);
    if (source->converter_initialized) {
        (void)ma_data_converter_reset(&source->converter);
    }
    return true;
}

static size_t source_trim_to_target(NativeAudioSource *source) {
    unsigned write = atomic_load_explicit(&source->write_cursor, memory_order_acquire);
    unsigned read = atomic_load_explicit(&source->read_cursor, memory_order_relaxed);
    unsigned available = write - read;
    uint32_t target_ms = atomic_load_explicit(&source->target_delay_ms, memory_order_relaxed);
    uint64_t keep_frames_64 = (uint64_t)(target_ms + NATIVE_AUDIO_HARD_TRIM_KEEP_MS) * source->sample_rate / 1000u;
    unsigned keep_frames = keep_frames_64 > UINT_MAX ? UINT_MAX : (unsigned)keep_frames_64;
    if (available <= keep_frames) {
        atomic_store_explicit(&source->trim_requested, false, memory_order_relaxed);
        return 0;
    }
    unsigned dropped = available - keep_frames;
    atomic_store_explicit(&source->read_cursor, read + dropped, memory_order_release);
    if (source->converter_initialized) {
        (void)ma_data_converter_reset(&source->converter);
    }
    source->current_correction_ppm = 0;
    atomic_store_explicit(&source->correction_ppm, 0, memory_order_relaxed);
    atomic_store_explicit(&source->trim_requested, false, memory_order_relaxed);
    atomic_fetch_add_explicit(&source->hard_corrections, 1u, memory_order_relaxed);
    return dropped;
}

static void source_update_rate_correction(NativeAudioSource *source) {
    if (!source->converter_initialized || source->sample_rate == 0 || !source->live) {
        source->current_correction_ppm = 0;
        atomic_store_explicit(&source->correction_ppm, 0, memory_order_relaxed);
        return;
    }
    /* Control the post-read queue: the current graph block is about to consume 10 ms,
     * so its frames sit on top of the desired standing target at this instant. */
    int error_ms = (int)native_audio_source_queue_ms(source) -
                   (int)atomic_load_explicit(&source->target_delay_ms, memory_order_relaxed) -
                   (int)(NATIVE_AUDIO_PIPELINE_BLOCK_FRAMES * 1000u / NATIVE_AUDIO_PIPELINE_SAMPLE_RATE);
    int proportional = 0;
    int magnitude = error_ms < 0 ? -error_ms : error_ms;
    if (magnitude > 10) {
        if (magnitude > 50) {
            magnitude = 50;
        }
        proportional = magnitude * NATIVE_AUDIO_MAX_CORRECTION_PPM / 50;
        if (error_ms < 0) {
            proportional = -proportional;
        }
    }
    int integral_step = error_ms / 4;
    if (integral_step == 0 && error_ms != 0) {
        integral_step = error_ms < 0 ? -1 : 1;
    }
    if (integral_step != 0) {
        source->drift_integral_ppm += integral_step;
        if (source->drift_integral_ppm > NATIVE_AUDIO_MAX_CORRECTION_PPM) {
            source->drift_integral_ppm = NATIVE_AUDIO_MAX_CORRECTION_PPM;
        } else if (source->drift_integral_ppm < -NATIVE_AUDIO_MAX_CORRECTION_PPM) {
            source->drift_integral_ppm = -NATIVE_AUDIO_MAX_CORRECTION_PPM;
        }
    }
    int desired = proportional + source->drift_integral_ppm;
    if (desired > NATIVE_AUDIO_MAX_CORRECTION_PPM) {
        desired = NATIVE_AUDIO_MAX_CORRECTION_PPM;
    } else if (desired < -NATIVE_AUDIO_MAX_CORRECTION_PPM) {
        desired = -NATIVE_AUDIO_MAX_CORRECTION_PPM;
    }
    if (desired > source->current_correction_ppm + NATIVE_AUDIO_CORRECTION_STEP_PPM) {
        source->current_correction_ppm += NATIVE_AUDIO_CORRECTION_STEP_PPM;
    } else if (desired < source->current_correction_ppm - NATIVE_AUDIO_CORRECTION_STEP_PPM) {
        source->current_correction_ppm -= NATIVE_AUDIO_CORRECTION_STEP_PPM;
    } else {
        source->current_correction_ppm = desired;
    }
    float ratio = (float)source->sample_rate / (float)NATIVE_AUDIO_PIPELINE_SAMPLE_RATE;
    ratio *= 1.0f + (float)source->current_correction_ppm / 1000000.0f;
    (void)ma_data_converter_set_rate_ratio(&source->converter, ratio);
    atomic_store_explicit(&source->correction_ppm, source->current_correction_ppm, memory_order_relaxed);
}

static size_t source_convert_frames(NativeAudioSource *source, float *out, size_t frames,
                                    bool *discontinuity_applied) {
    size_t produced = 0;
    *discontinuity_applied = false;
    while (produced < frames && source->converter_initialized) {
        /* Load write_cursor before rechecking generations so new PCM cannot bypass discontinuity handling. */
        unsigned write = atomic_load_explicit(&source->write_cursor, memory_order_acquire);
        bool format_changed = source_apply_generation(source);
        bool talkspurt_changed = source_apply_talkspurt(source);
        if (format_changed || talkspurt_changed) {
            *discontinuity_applied = true;
            break;
        }
        unsigned read = atomic_load_explicit(&source->read_cursor, memory_order_relaxed);
        unsigned available = write - read;
        if (available == 0) {
            break;
        }
        unsigned ring_pos = read % NATIVE_AUDIO_PIPELINE_CAPACITY_FRAMES;
        unsigned contiguous = NATIVE_AUDIO_PIPELINE_CAPACITY_FRAMES - ring_pos;
        if (contiguous > available) {
            contiguous = available;
        }
        ma_uint64 input_count = contiguous;
        ma_uint64 output_count = frames - produced;
        ma_result result = ma_data_converter_process_pcm_frames(
            &source->converter, &source->ring[(size_t)ring_pos * NATIVE_AUDIO_PIPELINE_CHANNELS], &input_count,
            &out[produced * NATIVE_AUDIO_PIPELINE_CHANNELS], &output_count);
        if (input_count > 0) {
            atomic_store_explicit(&source->read_cursor, read + (unsigned)input_count, memory_order_release);
        }
        produced += (size_t)output_count;
        if (result != MA_SUCCESS || (input_count == 0 && output_count == 0)) {
            break;
        }
    }
    return produced;
}

static void source_apply_discontinuity_envelope(NativeAudioSource *source, float *frames, size_t frame_count,
                                                float meter_gain, float *peak_left, float *peak_right) {
    /* Keep trim fades at the converter reset; ma_sound handles user-gain smoothing. */
    for (size_t frame = 0; frame < frame_count; frame++) {
        float envelope = 1.0f;
        if (source->fade_out_remaining > 0) {
            envelope = (float)source->fade_out_remaining / (float)NATIVE_AUDIO_FADE_FRAMES;
            source->fade_out_remaining--;
        } else if (source->fade_in_remaining > 0) {
            envelope = (float)(NATIVE_AUDIO_FADE_FRAMES - source->fade_in_remaining + 1u) /
                       (float)NATIVE_AUDIO_FADE_FRAMES;
            source->fade_in_remaining--;
        }
        float left = frames[frame * 2] * envelope;
        float right = frames[frame * 2 + 1] * envelope;
        frames[frame * 2] = left;
        frames[frame * 2 + 1] = right;
        float metered_left = left * meter_gain;
        float metered_right = right * meter_gain;
        float abs_left = metered_left < 0.0f ? -metered_left : metered_left;
        float abs_right = metered_right < 0.0f ? -metered_right : metered_right;
        if (abs_left > *peak_left) {
            *peak_left = abs_left;
        }
        if (abs_right > *peak_right) {
            *peak_right = abs_right;
        }
    }
}

static void source_publish_peaks(NativeAudioSource *source, float left, float right, uint32_t now_ms) {
    atomic_store_explicit(&source->peak_left, (unsigned)native_audio_float_peak_to_i32(left), memory_order_relaxed);
    atomic_store_explicit(&source->peak_right, (unsigned)native_audio_float_peak_to_i32(right), memory_order_relaxed);
    atomic_store_explicit(&source->peak_when_ms, (unsigned)now_ms, memory_order_release);
}

typedef struct NativeSourceReadBlock {
    NativeAudioSource *source;
    float *out;
    size_t frames;
    uint32_t now_ms;
    float meter_gain;
    float peak_left;
    float peak_right;
} NativeSourceReadBlock;

static float source_apply_routing(NativeAudioSource *source) {
    /* duck_applied_index/duck_factor are render-thread state: the engine read that
     * invokes this callback runs the duck controller first, on the same thread. */
    float volume = (float)atomic_load_explicit(&source->gain_q15, memory_order_relaxed) / 32768.0f;
    if (source->index == source->pipeline->duck_applied_index) {
        volume *= source->pipeline->duck_factor;
    }

    /* Routing cuts last, console-style: mute wins over solo. */
    unsigned solo_mask = atomic_load_explicit(&source->pipeline->solo_mask, memory_order_relaxed);
    if (atomic_load_explicit(&source->muted, memory_order_relaxed) ||
        (solo_mask != 0 && (solo_mask & (1u << source->index)) == 0)) {
        volume = 0.0f;
    }
    if (source->sound_initialized && volume != source->applied_volume) {
        ma_sound_set_volume(&source->sound, volume);
        source->applied_volume = volume;
    }
    return volume;
}

static void source_render_silence(NativeSourceReadBlock *block) {
    source_apply_discontinuity_envelope(block->source, block->out, block->frames,
                                        block->meter_gain, &block->peak_left, &block->peak_right);
}

static void source_finish_read(NativeSourceReadBlock *block, ma_uint64 frame_count,
                               ma_uint64 *frames_read) {
    source_publish_peaks(block->source, block->peak_left, block->peak_right, block->now_ms);
    if (frames_read) {
        *frames_read = frame_count;
    }
}

static void source_mark_unavailable(NativeAudioSource *source) {
    unsigned write = atomic_load_explicit(&source->write_cursor, memory_order_acquire);
    atomic_store_explicit(&source->read_cursor, write, memory_order_release);
    source->live = false;
    source->rebuffering = true;
}

static bool source_prepare_playout(NativeAudioSource *source, uint32_t now_ms,
                                   bool *trim_requested_out) {
    uint32_t target_ms = atomic_load_explicit(&source->target_delay_ms, memory_order_relaxed);
    uint32_t queue_ms = native_audio_source_queue_ms(source);
    bool trim_requested = atomic_load_explicit(&source->trim_requested, memory_order_relaxed);
    bool excessive_backlog = queue_ms > target_ms + NATIVE_AUDIO_HARD_TRIM_ERROR_MS;

    if (source->rebuffering && (trim_requested || excessive_backlog)) {
        if (source_trim_to_target(source) > 0) {
            source->fade_in_remaining = NATIVE_AUDIO_FADE_FRAMES;
        }
        queue_ms = native_audio_source_queue_ms(source);
        trim_requested = atomic_load_explicit(&source->trim_requested, memory_order_relaxed);
        excessive_backlog = queue_ms > target_ms + NATIVE_AUDIO_HARD_TRIM_ERROR_MS;
    }

    if (source->rebuffering) {
        unsigned last_push = atomic_load_explicit(&source->last_push_ms, memory_order_acquire);
        bool stale_short_burst =
            queue_ms > 0 && (uint32_t)((unsigned)now_ms - last_push) >= NATIVE_AUDIO_STALE_BURST_MS;
        if (queue_ms >= target_ms || stale_short_burst) {
            source->rebuffering = false;
            source->live = true;
            source->fade_in_remaining = NATIVE_AUDIO_FADE_FRAMES;
        }
    }

    *trim_requested_out = trim_requested || excessive_backlog;
    return source->live;
}

static void source_prepare_live_read(NativeAudioSource *source, bool trim_requested) {
    if (!source->trim_after_fade && source->fade_out_remaining == 0 && trim_requested) {
        source->trim_after_fade = true;
        source->fade_out_remaining = NATIVE_AUDIO_FADE_FRAMES;
        source->fade_in_remaining = 0;
    }
    source_update_rate_correction(source);
}

static bool source_render_live(NativeSourceReadBlock *block) {
    NativeAudioSource *source = block->source;
    size_t offset = 0;
    bool underrun = false;
    while (offset < block->frames) {
        if (source->trim_after_fade && source->fade_out_remaining == 0) {
            (void)source_trim_to_target(source);
            source->trim_after_fade = false;
            source->fade_in_remaining = NATIVE_AUDIO_FADE_FRAMES;
            source_update_rate_correction(source);
        }
        size_t segment = block->frames - offset;
        if (source->fade_out_remaining > 0 && segment > source->fade_out_remaining) {
            segment = source->fade_out_remaining;
        }
        bool discontinuity_applied = false;
        size_t produced = source_convert_frames(source, &block->out[offset * NATIVE_AUDIO_PIPELINE_CHANNELS],
                                                segment, &discontinuity_applied);
        if (!discontinuity_applied && produced < segment) {
            underrun = true;
        }
        size_t gain_frames = discontinuity_applied ? block->frames - offset : segment;
        source_apply_discontinuity_envelope(source,
                                            &block->out[offset * NATIVE_AUDIO_PIPELINE_CHANNELS],
                                            gain_frames, block->meter_gain,
                                            &block->peak_left, &block->peak_right);
        offset += gain_frames;
    }
    return underrun;
}

static void source_handle_underrun(NativeAudioSource *source) {
    atomic_fetch_add_explicit(&source->underruns, 1u, memory_order_relaxed);
    unsigned target = atomic_load_explicit(&source->target_delay_ms, memory_order_relaxed);
    source_raise_target(source, target + NATIVE_AUDIO_TARGET_HEADROOM_MS);
    source->live = false;
    source->rebuffering = true;
    source->trim_after_fade = false;
    source->fade_out_remaining = 0;
    source->fade_in_remaining = 0;
    source->current_correction_ppm = 0;
    atomic_store_explicit(&source->correction_ppm, 0, memory_order_relaxed);
    if (source->converter_initialized) {
        (void)ma_data_converter_reset(&source->converter);
    }
}

static ma_result source_read(ma_data_source *data_source, void *frames_out, ma_uint64 frame_count,
                             ma_uint64 *frames_read) {
    NativeAudioSource *source = (NativeAudioSource *)data_source;
    float *out = (float *)frames_out;
    if (frames_read) {
        *frames_read = 0;
    }
    if (!source || !out || frame_count == 0 || frame_count > SIZE_MAX) {
        return MA_INVALID_ARGS;
    }
    size_t frames = (size_t)frame_count;
    memset(out, 0, frames * NATIVE_AUDIO_PIPELINE_CHANNELS * sizeof(float));
    (void)source_apply_generation(source);
    (void)source_apply_talkspurt(source);
#ifdef LGNOME_AUDIO_PIPELINE_TESTING
    if (source->pipeline->before_ring_read) {
        source->pipeline->before_ring_read(source->pipeline->before_ring_read_ctx, source->index);
    }
#endif
    NativeSourceReadBlock block = {
        .source = source,
        .out = out,
        .frames = frames,
        .now_ms = native_audio_pipeline_now_ms(source->pipeline),
        .meter_gain = source_apply_routing(source),
    };

    if (!atomic_load_explicit(&source->open, memory_order_acquire) || !source->converter_initialized) {
        source_mark_unavailable(source);
        source_render_silence(&block);
        source_finish_read(&block, frame_count, frames_read);
        return MA_SUCCESS;
    }

    bool trim_requested = false;
    if (!source_prepare_playout(source, block.now_ms, &trim_requested)) {
        source->current_correction_ppm = 0;
        atomic_store_explicit(&source->correction_ppm, 0, memory_order_relaxed);
        source_render_silence(&block);
        source_finish_read(&block, frame_count, frames_read);
        return MA_SUCCESS;
    }

    source_prepare_live_read(source, trim_requested);
    if (source_render_live(&block)) {
        source_handle_underrun(source);
    }

    source_finish_read(&block, frame_count, frames_read);
    return MA_SUCCESS;
}

static ma_result source_seek(ma_data_source *data_source, ma_uint64 frame_index) {
    (void)data_source;
    (void)frame_index;
    return MA_NOT_IMPLEMENTED;
}

static ma_result source_get_data_format(ma_data_source *data_source, ma_format *format, ma_uint32 *channels,
                                        ma_uint32 *sample_rate, ma_channel *channel_map, size_t channel_map_cap) {
    (void)data_source;
    if (format) {
        *format = ma_format_f32;
    }
    if (channels) {
        *channels = NATIVE_AUDIO_PIPELINE_CHANNELS;
    }
    if (sample_rate) {
        *sample_rate = NATIVE_AUDIO_PIPELINE_SAMPLE_RATE;
    }
    if (channel_map && channel_map_cap > 0) {
        ma_channel_map_init_standard(ma_standard_channel_map_default, channel_map, channel_map_cap,
                                     NATIVE_AUDIO_PIPELINE_CHANNELS);
    }
    return MA_SUCCESS;
}

static ma_result source_get_cursor(ma_data_source *data_source, ma_uint64 *cursor) {
    (void)data_source;
    if (cursor) {
        *cursor = 0;
    }
    return MA_NOT_IMPLEMENTED;
}

static ma_result source_get_length(ma_data_source *data_source, ma_uint64 *length) {
    (void)data_source;
    if (length) {
        *length = 0;
    }
    return MA_NOT_IMPLEMENTED;
}

static ma_data_source_vtable source_vtable = {
    source_read,
    source_seek,
    source_get_data_format,
    source_get_cursor,
    source_get_length,
    NULL,
    0,
};

bool native_audio_sources_init(NativeAudioPipelineImpl *pipeline) {
    ma_data_converter_config converter_config = source_converter_config(NATIVE_AUDIO_PIPELINE_SAMPLE_RATE);
    size_t converter_heap_size = 0;
    if (ma_data_converter_get_heap_size(&converter_config, &converter_heap_size) != MA_SUCCESS) {
        return false;
    }

    for (int i = 0; i < NATIVE_AUDIO_PIPELINE_MAX_SOURCES; i++) {
        NativeAudioSource *source = &pipeline->sources[i];
        source->pipeline = pipeline;
        source->index = i;
        source->ring = (int16_t *)calloc((size_t)NATIVE_AUDIO_PIPELINE_CAPACITY_FRAMES *
                                            NATIVE_AUDIO_PIPELINE_CHANNELS,
                                        sizeof(int16_t));
        source->converter_heap = converter_heap_size ? malloc(converter_heap_size) : NULL;
        if (!source->ring || (converter_heap_size && !source->converter_heap)) {
            return false;
        }
        atomic_init(&source->read_cursor, 0u);
        atomic_init(&source->write_cursor, 0u);
        atomic_init(&source->reset_cursor, 0u);
        atomic_init(&source->format_generation, 0u);
        atomic_init(&source->pending_sample_rate, NATIVE_AUDIO_PIPELINE_SAMPLE_RATE);
        atomic_init(&source->pending_channels, NATIVE_AUDIO_PIPELINE_CHANNELS);
        atomic_init(&source->open, false);
        atomic_init(&source->trim_requested, false);
        atomic_init(&source->talkspurt_cursor, 0u);
        atomic_init(&source->talkspurt_generation, 0u);
        atomic_init(&source->last_push_ms, 0u);
        atomic_init(&source->gain_q15, NATIVE_AUDIO_PIPELINE_GAIN_UNITY_Q15);
        atomic_init(&source->muted, false);
        atomic_init(&source->target_delay_ms, NATIVE_AUDIO_TARGET_INITIAL_MS);
        atomic_init(&source->jitter_p95_ms, 0u);
        atomic_init(&source->correction_ppm, 0);
        atomic_init(&source->underruns, 0u);
        atomic_init(&source->hard_corrections, 0u);
        atomic_init(&source->overflows, 0u);
        atomic_init(&source->peak_left, 0u);
        atomic_init(&source->peak_right, 0u);
        atomic_init(&source->peak_when_ms, 0u);
        source->sample_rate = NATIVE_AUDIO_PIPELINE_SAMPLE_RATE;
        source->channels = NATIVE_AUDIO_PIPELINE_CHANNELS;
        source->producer_sample_rate = NATIVE_AUDIO_PIPELINE_SAMPLE_RATE;
        source->producer_channels = NATIVE_AUDIO_PIPELINE_CHANNELS;
        source->rebuffering = true;
        source->applied_volume = 1.0f;
        native_audio_jitter_reset(&source->jitter, native_audio_pipeline_now_ms(pipeline), 0u);

        ma_data_source_config data_source_config = ma_data_source_config_init();
        data_source_config.vtable = &source_vtable;
        if (ma_data_source_init(&data_source_config, &source->base) != MA_SUCCESS) {
            return false;
        }
        source->base_initialized = true;
        ma_sound_config sound_config = ma_sound_config_init_2(&pipeline->engine);
        sound_config.pDataSource = (ma_data_source *)source;
        sound_config.flags = MA_SOUND_FLAG_NO_PITCH | MA_SOUND_FLAG_NO_SPATIALIZATION;
        if (ma_sound_init_ex(&pipeline->engine, &sound_config, &source->sound) != MA_SUCCESS) {
            return false;
        }
        source->sound_initialized = true;
        if (ma_sound_start(&source->sound) != MA_SUCCESS) {
            return false;
        }
    }

    clog(cLogLevelTrace, "initialized %d source voices", NATIVE_AUDIO_PIPELINE_MAX_SOURCES);
    return true;
}

void native_audio_sources_uninit(NativeAudioPipelineImpl *pipeline) {
    if (!pipeline) {
        return;
    }
    for (int i = NATIVE_AUDIO_PIPELINE_MAX_SOURCES - 1; i >= 0; i--) {
        NativeAudioSource *source = &pipeline->sources[i];
        if (source->sound_initialized) {
            ma_sound_uninit(&source->sound);
        }
        if (source->converter_initialized) {
            ma_data_converter_uninit(&source->converter, NULL);
        }
        if (source->base_initialized) {
            ma_data_source_uninit(&source->base);
        }
        free(source->converter_heap);
        free(source->ring);
    }
}

bool native_audio_pipeline_set_source_format(NativeAudioPipeline *pipeline, int source_index, uint32_t sample_rate,
                                             uint16_t channels) {
    if (!pipeline || !pipeline->impl || !native_audio_source_index_valid(source_index) || sample_rate < 8000u ||
        sample_rate > 192000u || channels == 0 || channels > NATIVE_AUDIO_PIPELINE_CHANNELS) {
        return false;
    }
    NativeAudioSource *source = &((NativeAudioPipelineImpl *)pipeline->impl)->sources[source_index];
    source->producer_sample_rate = sample_rate;
    source->producer_channels = channels;
    unsigned underruns = atomic_load_explicit(&source->underruns, memory_order_relaxed);
    native_audio_jitter_reset(&source->jitter, native_audio_pipeline_now_ms(source->pipeline), underruns);
    atomic_store_explicit(&source->jitter_p95_ms, 0u, memory_order_relaxed);
    atomic_store_explicit(&source->target_delay_ms, NATIVE_AUDIO_TARGET_INITIAL_MS, memory_order_relaxed);
    unsigned boundary = atomic_load_explicit(&source->write_cursor, memory_order_acquire);
    atomic_store_explicit(&source->pending_sample_rate, sample_rate, memory_order_relaxed);
    atomic_store_explicit(&source->pending_channels, channels, memory_order_relaxed);
    atomic_store_explicit(&source->reset_cursor, boundary, memory_order_release);
    atomic_fetch_add_explicit(&source->format_generation, 1u, memory_order_release);
    atomic_store_explicit(&source->open, true, memory_order_release);
    return true;
}

void native_audio_pipeline_close_source(NativeAudioPipeline *pipeline, int source_index) {
    if (!pipeline || !pipeline->impl || !native_audio_source_index_valid(source_index)) {
        return;
    }
    NativeAudioSource *source = &((NativeAudioPipelineImpl *)pipeline->impl)->sources[source_index];
    atomic_store_explicit(&source->open, false, memory_order_release);
    unsigned boundary = atomic_load_explicit(&source->write_cursor, memory_order_acquire);
    atomic_store_explicit(&source->reset_cursor, boundary, memory_order_release);
    atomic_fetch_add_explicit(&source->format_generation, 1u, memory_order_release);
    native_audio_jitter_mark_discontinuity(&source->jitter);
}

size_t native_audio_pipeline_push(NativeAudioPipeline *pipeline, int source_index, const int16_t *samples,
                                  size_t frames, uint32_t timestamp_ms) {
    if (!pipeline || !pipeline->impl || !native_audio_source_index_valid(source_index) || !samples || frames == 0) {
        return 0;
    }
    NativeAudioSource *source = &((NativeAudioPipelineImpl *)pipeline->impl)->sources[source_index];
    if (!atomic_load_explicit(&source->open, memory_order_acquire) || source->producer_sample_rate == 0 ||
        source->producer_channels == 0) {
        return 0;
    }
    uint32_t now_ms = native_audio_pipeline_now_ms(source->pipeline);
    unsigned write = atomic_load_explicit(&source->write_cursor, memory_order_relaxed);
    source_update_arrival_controller(source, frames, timestamp_ms, now_ms, write);
    unsigned read = atomic_load_explicit(&source->read_cursor, memory_order_acquire);
    unsigned used = write - read;
    unsigned available = used < NATIVE_AUDIO_PIPELINE_CAPACITY_FRAMES
                             ? NATIVE_AUDIO_PIPELINE_CAPACITY_FRAMES - used
                             : 0u;
    size_t to_write = frames < available ? frames : available;
    size_t dropped = frames - to_write;
    if (dropped > 0) {
        atomic_store_explicit(&source->trim_requested, true, memory_order_release);
        atomic_fetch_add_explicit(&source->overflows, 1u, memory_order_relaxed);
    }

    size_t copied = 0;
    while (copied < to_write) {
        unsigned ring_pos = (write + (unsigned)copied) % NATIVE_AUDIO_PIPELINE_CAPACITY_FRAMES;
        size_t contiguous = NATIVE_AUDIO_PIPELINE_CAPACITY_FRAMES - ring_pos;
        if (contiguous > to_write - copied) {
            contiguous = to_write - copied;
        }
        int16_t *dest = &source->ring[(size_t)ring_pos * NATIVE_AUDIO_PIPELINE_CHANNELS];
        if (source->producer_channels == NATIVE_AUDIO_PIPELINE_CHANNELS) {
            memcpy(dest, &samples[copied * NATIVE_AUDIO_PIPELINE_CHANNELS],
                   contiguous * NATIVE_AUDIO_PIPELINE_CHANNELS * sizeof(int16_t));
        } else {
            for (size_t i = 0; i < contiguous; i++) {
                int16_t mono;
                memcpy(&mono, &samples[copied + i], sizeof(mono));
                dest[i * 2] = mono;
                dest[i * 2 + 1] = mono;
            }
        }
        copied += contiguous;
    }
    if (to_write > 0) {
        atomic_store_explicit(&source->write_cursor, write + (unsigned)to_write, memory_order_release);
        atomic_store_explicit(&source->last_push_ms, (unsigned)now_ms, memory_order_release);
    }
    return dropped;
}

#ifdef LGNOME_AUDIO_PIPELINE_TESTING
void native_audio_pipeline_set_test_before_ring_read(NativeAudioPipeline *pipeline,
                                                     NativeAudioPipelineTestHook hook, void *ctx) {
    if (!pipeline || !pipeline->impl) {
        return;
    }
    NativeAudioPipelineImpl *impl = (NativeAudioPipelineImpl *)pipeline->impl;
    impl->before_ring_read = hook;
    impl->before_ring_read_ctx = ctx;
}
#endif

bool native_audio_pipeline_get_source_stats(NativeAudioPipeline *pipeline, int source_index,
                                            NativeAudioSourceStats *stats) {
    if (!stats) {
        return false;
    }
    memset(stats, 0, sizeof(*stats));
    if (!pipeline || !pipeline->impl || !native_audio_source_index_valid(source_index)) {
        return false;
    }
    NativeAudioSource *source = &((NativeAudioPipelineImpl *)pipeline->impl)->sources[source_index];
    stats->open = atomic_load_explicit(&source->open, memory_order_relaxed);
    stats->queue_ms = native_audio_source_queue_ms(source);
    stats->target_delay_ms = atomic_load_explicit(&source->target_delay_ms, memory_order_relaxed);
    stats->jitter_p95_ms = atomic_load_explicit(&source->jitter_p95_ms, memory_order_relaxed);
    stats->src_correction_ppm = atomic_load_explicit(&source->correction_ppm, memory_order_relaxed);
    stats->underruns = atomic_load_explicit(&source->underruns, memory_order_relaxed);
    stats->hard_corrections = atomic_load_explicit(&source->hard_corrections, memory_order_relaxed);
    stats->overflows = atomic_load_explicit(&source->overflows, memory_order_relaxed);
    return true;
}
