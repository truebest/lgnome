#ifndef LGNOME_NATIVE_STATISTICS_H
#define LGNOME_NATIVE_STATISTICS_H

#include "native_diagnostics.h"

typedef enum NativeMetricState {
    NATIVE_METRIC_VALID, NATIVE_METRIC_COLLECTING, NATIVE_METRIC_STALE,
    NATIVE_METRIC_UNAVAILABLE, NATIVE_METRIC_UNSUPPORTED
} NativeMetricState;

typedef enum NativeMetricId {
    METRIC_AUS, METRIC_MBPS, METRIC_RDP, METRIC_BUFFER, METRIC_DECODED, METRIC_AU_AGE,
    METRIC_AUDIO_AGE, METRIC_JITTER, METRIC_SRC, METRIC_CPU, METRIC_RSS, METRIC_NDL,
    METRIC_MAIN_AUX, METRIC_VIDEO_LOSS, METRIC_VIDEO_LATE, METRIC_VIDEO_JITTER,
    METRIC_RETRANSMIT, METRIC_KEYFRAME_AGE, METRIC_DECODE_TIME, METRIC_DECODE_QUEUE,
    METRIC_DECODE_ERRORS, METRIC_PREDECODE_DROP, METRIC_KEYFRAME_REQUESTS,
    METRIC_PRESENTED, METRIC_PRESENT_INTERVAL, METRIC_LATE_DROP, METRIC_GRAPHICS_QUEUE,
    METRIC_GRAPHICS_AGE, METRIC_GRAPHICS_TIME, METRIC_PRIMARY_INTERVAL, METRIC_PRIMARY_GAPS,
    METRIC_STALL, METRIC_RECEIVE_PRESENT, METRIC_AV, METRIC_FIRST_FRAME,
    METRIC_UNDERRUNS, METRIC_TRIMS, METRIC_DROPPED, METRIC_PLAYOUT_GAPS, METRIC_PLC,
    METRIC_FEC, METRIC_AUDIO_BITRATE, METRIC_AUDIO_PACKETS, METRIC_AUDIO_LOSS,
    METRIC_AUDIO_LATE, METRIC_AUDIO_REORDERED, METRIC_AUDIO_PACKET_AGE,
    METRIC_AUDIO_DECODE_TIME, METRIC_AUDIO_DECODE_ERRORS, METRIC_PCM_INTERVAL,
    METRIC_PCM_FEED, METRIC_DEVICE_QUEUE, METRIC_GRAPHICS_MEMORY, METRIC_ALL_MEMORY,
    METRIC_GRAPHICS_WAIT, METRIC_NDL_MAX, METRIC_NDL_ERRORS, METRIC_NDL_QUEUE,
    METRIC_PENDING, METRIC_REPLAY, METRIC_ACCEPTED, METRIC_DISCONTINUITIES,
    METRIC_NET_RX,
    METRIC_NET_TX,
    METRIC_NET_RTT,
    METRIC_NET_BANDWIDTH,
    METRIC_NET_STATE,
    METRIC_NET_UPTIME,
    METRIC_NET_TRANSPORT,
    METRIC_NET_RX_AGE,
    METRIC_NET_RX_BYTES,
    METRIC_NET_TX_BYTES,
    METRIC_NET_BASE_RTT,
    METRIC_NET_REPORT_AGE,
    METRIC_NET_REPORTS,
    METRIC_NET_QOS_STATE,
    METRIC_NET_SRTT,
    METRIC_NET_RTTVAR,
    METRIC_NET_RTO,
    METRIC_NET_UNACKED,
    METRIC_NET_CWND,
    METRIC_NET_RETRANSMITS,
    METRIC_COUNT
} NativeMetricId;

typedef struct NativeMetricDescription {
    NativeMetricId id;
    const char *label, *unit, *source, *scope;
    unsigned decimals;
} NativeMetricDescription;

typedef struct NativeMetricValue {
    NativeMetricState state;
    char text[80];
    const char *reason;
} NativeMetricValue;

const NativeMetricDescription *native_statistics_metric(NativeMetricId id);
NativeMetricValue native_statistics_value(const NativeDiagnosticsSnapshot *s, NativeMetricId id);
const char *native_statistics_state_name(NativeMetricState state);
void native_statistics_sample(NativeStatisticsTimeline *history, NativeDiagnosticsSnapshot *snapshot);
void native_statistics_series_add(NativeStatisticsSeries *series, uint32_t at_ms, double value, bool valid);
/* X is deciseconds; negative means outside the 30-second monotonic window. */
int native_statistics_x(uint32_t now_ms, uint32_t at_ms);

enum { NATIVE_STATS_OVERVIEW, NATIVE_STATS_VIDEO, NATIVE_STATS_AUDIO,
       NATIVE_STATS_NETWORK, NATIVE_STATS_PERFORMANCE, NATIVE_STATS_DIAGNOSTICS, NATIVE_STATS_TABS };
typedef enum NativeStatisticsKey {
    NATIVE_STATS_KEY_NONE, NATIVE_STATS_KEY_LEFT, NATIVE_STATS_KEY_RIGHT,
    NATIVE_STATS_KEY_UP, NATIVE_STATS_KEY_DOWN, NATIVE_STATS_KEY_OK, NATIVE_STATS_KEY_BACK
} NativeStatisticsKey;
typedef struct NativeStatisticsNavigation {
    unsigned tab, video_category, audio_category, network_category, focus, held, help_page;
    bool help;
} NativeStatisticsNavigation;
bool native_statistics_key(NativeStatisticsNavigation *nav, NativeStatisticsKey key,
                           bool down, bool repeat, bool *open_hub);
void native_statistics_focus_reset(NativeStatisticsNavigation *nav);

#endif
