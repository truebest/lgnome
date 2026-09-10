#ifndef LGNOME_AUDIO_PIPELINE_INTERNAL_H
#define LGNOME_AUDIO_PIPELINE_INTERNAL_H

#include "audio_jitter.h"
#include "audio_pipeline.h"

#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>

/* Keep every pipeline translation unit and the separately compiled upstream
 * miniaudio.c on the same headless feature set. */
#ifndef MA_NO_DEVICE_IO
#define MA_NO_DEVICE_IO
#endif
#ifndef MA_NO_DECODING
#define MA_NO_DECODING
#endif
#ifndef MA_NO_ENCODING
#define MA_NO_ENCODING
#endif
#ifndef MA_NO_RESOURCE_MANAGER
#define MA_NO_RESOURCE_MANAGER
#endif
#ifndef MA_NO_GENERATION
#define MA_NO_GENERATION
#endif
#ifndef MA_NO_THREADING
#define MA_NO_THREADING
#endif
#ifndef MA_NO_RUNTIME_LINKING
#define MA_NO_RUNTIME_LINKING
#endif
#if defined(MA_NO_ENGINE) || defined(MA_NO_NODE_GRAPH)
#error "lgnome audio requires the miniaudio engine and node graph"
#endif
#include "miniaudio.h"

typedef struct NativeAudioPipelineImpl NativeAudioPipelineImpl;

typedef struct NativeAudioSource {
    /* Required first member for a custom miniaudio data source. */
    ma_data_source_base base;
    bool base_initialized;
    ma_sound sound;
    bool sound_initialized;
    NativeAudioPipelineImpl *pipeline;
    int index;

    /* Fixed stereo storage avoids changing the physical ring stride across a format
     * generation. Mono input is duplicated by the producer before publication. */
    int16_t *ring;
    void *converter_heap;
    ma_data_converter converter;
    bool converter_initialized;

    atomic_uint read_cursor;
    atomic_uint write_cursor;
    atomic_uint reset_cursor;
    atomic_uint format_generation;
    atomic_uint pending_sample_rate;
    atomic_uint pending_channels;
    atomic_bool open;
    atomic_bool trim_requested;
    atomic_uint talkspurt_cursor;
    atomic_uint talkspurt_generation;
    atomic_uint last_push_ms;

    atomic_int gain_q15;
    atomic_bool muted;
    atomic_uint target_delay_ms;
    atomic_uint jitter_p95_ms;
    atomic_int correction_ppm;
    atomic_uint underruns;
    atomic_uint hard_corrections;
    atomic_uint overflows;
    atomic_uint peak_left;
    atomic_uint peak_right;
    atomic_uint peak_when_ms;

    /* Producer-owned controller state (one RDP worker per source). */
    uint32_t producer_sample_rate;
    uint16_t producer_channels;
    NativeAudioJitterController jitter;

    /* Consumer-owned converter/playout state. */
    unsigned applied_generation;
    unsigned seen_talkspurt_generation;
    uint32_t sample_rate;
    uint16_t channels;
    bool live;
    bool rebuffering;
    bool trim_after_fade;
    unsigned fade_out_remaining;
    unsigned fade_in_remaining;
    int current_correction_ppm;
    int drift_integral_ppm;
    float applied_volume; /* last volume pushed to the ma_sound voice (fader x duck) */
} NativeAudioSource;

struct NativeAudioPipelineImpl {
    ma_engine engine;
    bool engine_initialized;
    NativeAudioSource sources[NATIVE_AUDIO_PIPELINE_MAX_SOURCES];
    NativeAudioPipelineClock clock;
    void *clock_ctx;

    atomic_uint output_peak_left;
    atomic_uint output_peak_right;
    atomic_uint output_peak_when_ms;

    /* SDL thread sets routing; render thread owns the envelope.
     * Relaxed index/mask atomics may differ for one block. */
    atomic_int duck_foreground_index;
    atomic_uint duck_trigger_mask;
    atomic_uint duck_factor_q15;
    /* Render-thread copy of the routing the activity history was collected under: a
     * foreground/mask change invalidates duck_bg_seen (see pipeline_duck_update). */
    int duck_cfg_foreground;
    unsigned duck_cfg_triggers;
    /* Solo routing: non-zero = only sources in the mask reach the mix (SDL thread
     * writes, render thread reads once per source callback). */
    atomic_uint solo_mask;
    int duck_applied_index;
    float duck_factor;
    bool duck_bg_seen;
    uint32_t duck_last_active_ms;
    uint32_t duck_last_update_ms;

    float conversion_buffer[NATIVE_AUDIO_PIPELINE_BLOCK_FRAMES * NATIVE_AUDIO_PIPELINE_CHANNELS];
    int16_t pump_buffer[NATIVE_AUDIO_PIPELINE_BLOCK_FRAMES * NATIVE_AUDIO_PIPELINE_CHANNELS];

    pthread_mutex_t control_lock;
    bool control_lock_initialized;
    pthread_t pump_thread;
    bool pump_running;
    atomic_bool pump_stop;
    void (*feed)(void *ctx, const int16_t *samples, size_t frames);
    void *feed_ctx;
    uint32_t last_stats_log_ms;
    /* Per-source glitch counters as of the last report, so only new ones are logged. */
    unsigned reported_underruns[NATIVE_AUDIO_PIPELINE_MAX_SOURCES];
    unsigned reported_hard_corrections[NATIVE_AUDIO_PIPELINE_MAX_SOURCES];

#ifdef LGNOME_AUDIO_PIPELINE_TESTING
    NativeAudioPipelineTestHook before_ring_read;
    void *before_ring_read_ctx;
#endif
};

static inline uint32_t native_audio_pipeline_now_ms(const NativeAudioPipelineImpl *pipeline) {
    return pipeline->clock(pipeline->clock_ctx);
}

static inline bool native_audio_source_index_valid(int source) {
    return source >= 0 && source < NATIVE_AUDIO_PIPELINE_MAX_SOURCES;
}

static inline int32_t native_audio_float_peak_to_i32(float magnitude) {
    if (magnitude <= 0.0f) {
        return 0;
    }
    double scaled = (double)magnitude * 32768.0;
    return scaled >= INT32_MAX ? INT32_MAX : (int32_t)scaled;
}

bool native_audio_sources_init(NativeAudioPipelineImpl *pipeline);
void native_audio_sources_uninit(NativeAudioPipelineImpl *pipeline);
uint32_t native_audio_source_queue_ms(const NativeAudioSource *source);

#endif
