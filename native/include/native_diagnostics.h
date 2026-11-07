#ifndef LGNOME_NATIVE_DIAGNOSTICS_H
#define LGNOME_NATIVE_DIAGNOSTICS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "audio_pipeline.h"
#include "native_decoder_stats.h"
#include "rdp_ffi.h"
#include "native_settings.h"
#include "video_backend.h"

#define NATIVE_DIAGNOSTICS_INTERVAL_MS 500u
#define NATIVE_DIAGNOSTICS_HISTORY_MS 30000u
#define NATIVE_DIAGNOSTICS_HISTORY_POINTS (NATIVE_DIAGNOSTICS_HISTORY_MS / NATIVE_DIAGNOSTICS_INTERVAL_MS + 1u)
/* shortcut: eight cores fit the TV overlay; expand for a supported TV with more. */
#define NATIVE_DIAGNOSTICS_MAX_CPUS 8
/* All video counters are guarded by App.video_lock; replay is not live input. */
typedef struct NativeVideoCounters {
    unsigned epoch;
    unsigned generation;
    uint32_t received;
    uint32_t received_auxiliary;
    uint64_t bytes;
    uint64_t bytes_auxiliary;
    uint32_t accepted;
    uint32_t replayed;
    uint32_t last_au_ms;
    bool has_au;
    bool auxiliary;
} NativeVideoCounters;

static inline void native_diagnostics_counter_epoch(NativeVideoCounters *c, unsigned epoch) {
    if (c->epoch != epoch) {
        unsigned generation = c->generation + 1u;
        memset(c, 0, sizeof(*c));
        c->epoch = epoch;
        c->generation = generation;
    }
}

typedef struct NativeDiagnosticsRates {
    bool valid, discontinuity, decoded_valid;
    double input_aus;
    double input_mbps;
    double input_auxiliary_mbps;
    double decoded_fps;
} NativeDiagnosticsRates;

typedef struct NativeDiagnosticsWindow {
    bool started;
    int slot;
    uint32_t at_ms;
    NativeVideoCounters before;
} NativeDiagnosticsWindow;

typedef struct NativeDiagnosticsAudio {
    bool connected;
    uint32_t codec;
    uint32_t sample_rate;
    uint16_t channels;
    NativeAudioSourceStats source;
} NativeDiagnosticsAudio;

enum { NATIVE_STATS_AUS, NATIVE_STATS_MBPS, NATIVE_STATS_DECODED,
       NATIVE_STATS_PCM, NATIVE_STATS_SRC, NATIVE_STATS_BUFFER,
       NATIVE_STATS_TARGET, NATIVE_STATS_AGE, NATIVE_STATS_NET_RX, NATIVE_STATS_NET_TX,
       NATIVE_STATS_NET_RTT, NATIVE_STATS_NET_SRTT, NATIVE_STATS_SERIES };

typedef struct NativeStatisticsPoint {
    uint32_t at_ms;
    double value;
    bool valid;
} NativeStatisticsPoint;

typedef struct NativeStatisticsSeries {
    unsigned count;
    NativeStatisticsPoint points[NATIVE_DIAGNOSTICS_HISTORY_POINTS];
} NativeStatisticsSeries;

typedef struct NativeStatisticsTimeline {
    NativeStatisticsSeries series[NATIVE_STATS_SERIES];
    bool started, decoder_seen;
    int slot;
    unsigned epoch, generation;
    uint32_t at_ms, decoder_primary;
    uint64_t decoder_generation, network_generation;
    NativeDiagnosticsAudio audio;
} NativeStatisticsTimeline;

typedef struct NativeCpuCounters {
    bool present;
    uint64_t busy, idle, iowait;
} NativeCpuCounters;

typedef struct NativeCpuUsage {
    bool present, valid;
    double percent;
} NativeCpuUsage;

typedef struct NativeDiagnosticsSnapshot {
    RdpNetworkStats network;
    int slot;
    char name[NATIVE_SETTINGS_STRING_MAX];
    uint16_t width, height;
    bool bitmap;
    bool video_available;
    NativeVideoCodec codec;
    const char *status;
    NativeVideoCounters counters;
    NativeDiagnosticsRates rates;
    const NativeStatisticsTimeline *timeline;
    bool connected;
    NativeVideoDiagnostics ndl;
    const char *connection_status;
    bool session_uptime_valid;
    uint32_t session_uptime_s;
    NativeDecoderStats decoder;
    bool cpu_valid, memory_valid;
    double cpu_percent;
    uint64_t rss_bytes;
    NativeCpuUsage cores[NATIVE_DIAGNOSTICS_MAX_CPUS];
    NativeDiagnosticsAudio audio[NATIVE_SETTINGS_MAX_SESSIONS];
    uint32_t at_ms;
} NativeDiagnosticsSnapshot;

typedef struct NativeDiagnostics {
    bool enabled;
    bool sampled;
    uint32_t sampled_ms;
    NativeDiagnosticsWindow window;
    NativeStatisticsTimeline timeline;
    NativeDiagnosticsSnapshot snapshot;
    bool cpu_started;
    uint64_t cpu_ticks;
    uint32_t cpu_ms;
    NativeCpuCounters cores_before[NATIVE_DIAGNOSTICS_MAX_CPUS];
} NativeDiagnostics;

NativeDiagnosticsRates native_diagnostics_rates(NativeDiagnosticsWindow *window, int slot,
                                                 NativeVideoCounters counters, uint32_t now_ms);
bool native_diagnostics_parse_process(const char *text, uint64_t *ticks, uint64_t *rss_pages);
void native_diagnostics_update_cpus(NativeDiagnostics *diagnostics, const char *proc_stat);
void native_diagnostics_sample_system(NativeDiagnostics *diagnostics, uint32_t now_ms);

#ifdef LGNOME_TARGET_WEBOS
typedef struct App App;
void native_diagnostics_tick(App *app);
#endif
#endif
