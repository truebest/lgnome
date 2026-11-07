#include "native_statistics.h"

#include <inttypes.h>
#include <math.h>
#include <stdio.h>

static const NativeMetricDescription metrics[METRIC_COUNT] = {
    [METRIC_AUS] = {METRIC_AUS, "Video input", "AU/s", "Live video", "500 ms", 1},
    [METRIC_MBPS] = {METRIC_MBPS, "Total bitrate", "Mbit/s", "Live video", "500 ms", 2},
    [METRIC_RDP] = {METRIC_RDP, "RDP processing p95", "ms", "Not instrumented", "Published ~5 s window", 2},
    [METRIC_BUFFER] = {METRIC_BUFFER, "Buffer / target", "ms", "Audio source", "Current queue", 0},
    [METRIC_DECODED] = {METRIC_DECODED, "Primary decoded", "frames/s", "backend_ndl decoder", "Live counter delta; ~500 ms", 1},
    [METRIC_AU_AGE] = {METRIC_AU_AGE, "Last access unit", "ms", "Live video", "Monotonic age", 0},
    [METRIC_AUDIO_AGE] = {METRIC_AUDIO_AGE, "Queue age", "ms", "Not instrumented", "Last SRC output estimate", 0},
    [METRIC_JITTER] = {METRIC_JITTER, "Audio jitter p95", "ms", "Audio source", "Up to 10 s", 0},
    [METRIC_SRC] = {METRIC_SRC, "SRC correction", "ppm", "Audio source", "Current controller", 0},
    [METRIC_CPU] = {METRIC_CPU, "App CPU", "%", "/proc", "500 ms; 100% = one core", 0},
    [METRIC_RSS] = {METRIC_RSS, "Resident memory", "MiB", "/proc", "Current RSS", 1},
    [METRIC_NDL] = {METRIC_NDL, "NDL call EWMA", "ms", "NDL adapter", "Decoder load", 2},
    [METRIC_MAIN_AUX] = {METRIC_MAIN_AUX, "Main / auxiliary bitrate", "Mbit/s", "Live video", "500 ms", 2},
    [METRIC_VIDEO_LOSS] = {METRIC_VIDEO_LOSS, "Packet loss", "%", "Not instrumented", "5 s", 1},
    [METRIC_VIDEO_LATE] = {METRIC_VIDEO_LATE, "Late / reordered packets", "", "Not instrumented", "Lifetime", 0},
    [METRIC_VIDEO_JITTER] = {METRIC_VIDEO_JITTER, "Arrival jitter p95", "ms", "Not instrumented", "5 s", 2},
    [METRIC_RETRANSMIT] = {METRIC_RETRANSMIT, "Retransmissions", "", "Not instrumented", "Lifetime", 0},
    [METRIC_KEYFRAME_AGE] = {METRIC_KEYFRAME_AGE, "Last keyframe age", "ms", "Not instrumented", "Current", 0},
    [METRIC_DECODE_TIME] = {METRIC_DECODE_TIME, "Decode time p95", "ms", "backend_ndl decoder", "Published ~5 s; both views", 2},
    [METRIC_DECODE_QUEUE] = {METRIC_DECODE_QUEUE, "Decode queue", "frames / ms", "Not instrumented", "Current", 0},
    [METRIC_DECODE_ERRORS] = {METRIC_DECODE_ERRORS, "Decode errors", "", "Not instrumented", "Lifetime", 0},
    [METRIC_PREDECODE_DROP] = {METRIC_PREDECODE_DROP, "Dropped before decode", "", "Not instrumented", "Lifetime", 0},
    [METRIC_KEYFRAME_REQUESTS] = {METRIC_KEYFRAME_REQUESTS, "Keyframe requests", "", "Not instrumented", "Lifetime", 0},
    [METRIC_PRESENTED] = {METRIC_PRESENTED, "Presented output", "frames/s", "Not instrumented", "5 s", 1},
    [METRIC_PRESENT_INTERVAL] = {METRIC_PRESENT_INTERVAL, "Presentation interval p95", "ms", "Not instrumented", "5 s", 2},
    [METRIC_LATE_DROP] = {METRIC_LATE_DROP, "Late-frame drops", "", "Not instrumented", "Lifetime", 0},
    [METRIC_GRAPHICS_QUEUE] = {METRIC_GRAPHICS_QUEUE, "Graphics queue", "events", "Not instrumented", "Current data events", 0},
    [METRIC_GRAPHICS_AGE] = {METRIC_GRAPHICS_AGE, "Oldest graphics item", "ms", "Not instrumented", "Current", 0},
    [METRIC_GRAPHICS_TIME] = {METRIC_GRAPHICS_TIME, "Graphics callback p95", "ms", "Not instrumented", "Published ~5 s window", 2},
    [METRIC_PRIMARY_INTERVAL] = {METRIC_PRIMARY_INTERVAL, "Primary interval p95 / max", "ms", "backend_ndl decoder", "Published ~5 s window", 2},
    [METRIC_PRIMARY_GAPS] = {METRIC_PRIMARY_GAPS, "Primary gaps > 100 ms", "", "backend_ndl decoder", "Decoder load; includes idle", 0},
    [METRIC_STALL] = {METRIC_STALL, "Playback stall time", "ms", "Not instrumented", "5 s", 0},
    [METRIC_RECEIVE_PRESENT] = {METRIC_RECEIVE_PRESENT, "Receive-to-present p95", "ms", "Not instrumented", "5 s", 2},
    [METRIC_AV] = {METRIC_AV, "A/V offset (audio - video)", "ms", "Not instrumented", "Current", 2},
    [METRIC_FIRST_FRAME] = {METRIC_FIRST_FRAME, "Time to first frame", "ms", "Not instrumented", "Session", 0},
    [METRIC_UNDERRUNS] = {METRIC_UNDERRUNS, "Underruns", "", "Audio source", "Source lifetime", 0},
    [METRIC_TRIMS] = {METRIC_TRIMS, "Trims / overflows", "", "Audio source", "Source lifetime", 0},
    [METRIC_DROPPED] = {METRIC_DROPPED, "Dropped PCM frames", "fr", "Not instrumented", "Source lifetime", 0},
    [METRIC_PLAYOUT_GAPS] = {METRIC_PLAYOUT_GAPS, "Playout gaps > 20 ms", "", "Not instrumented", "5 s", 0},
    [METRIC_PLC] = {METRIC_PLC, "PLC concealed audio", "ms", "Not instrumented", "5 s", 0},
    [METRIC_FEC] = {METRIC_FEC, "FEC recovered packets", "", "Not instrumented", "Lifetime", 0},
    [METRIC_AUDIO_BITRATE] = {METRIC_AUDIO_BITRATE, "Received bitrate", "kbit/s", "Not instrumented", "5 s", 1},
    [METRIC_AUDIO_PACKETS] = {METRIC_AUDIO_PACKETS, "Packet arrival rate", "packets/s", "Not instrumented", "5 s", 1},
    [METRIC_AUDIO_LOSS] = {METRIC_AUDIO_LOSS, "Packet loss", "%", "Not instrumented", "5 s", 1},
    [METRIC_AUDIO_LATE] = {METRIC_AUDIO_LATE, "Late packets", "", "Not instrumented", "Lifetime", 0},
    [METRIC_AUDIO_REORDERED] = {METRIC_AUDIO_REORDERED, "Reordered packets", "", "Not instrumented", "Lifetime", 0},
    [METRIC_AUDIO_PACKET_AGE] = {METRIC_AUDIO_PACKET_AGE, "Last packet age", "ms", "Not instrumented", "Current", 0},
    [METRIC_AUDIO_DECODE_TIME] = {METRIC_AUDIO_DECODE_TIME, "Audio decode time p95", "ms", "Not instrumented", "5 s", 2},
    [METRIC_AUDIO_DECODE_ERRORS] = {METRIC_AUDIO_DECODE_ERRORS, "Audio decode errors", "", "Not instrumented", "Lifetime", 0},
    [METRIC_PCM_INTERVAL] = {METRIC_PCM_INTERVAL, "PCM interval error p95", "ms", "Not instrumented", "Published ~5 s window", 2},
    [METRIC_PCM_FEED] = {METRIC_PCM_FEED, "PCM feed p95 / block", "ms", "Not instrumented", "Published ~5 s window", 2},
    [METRIC_DEVICE_QUEUE] = {METRIC_DEVICE_QUEUE, "Output device queue", "ms", "Not instrumented", "Current", 0},
    [METRIC_GRAPHICS_MEMORY] = {METRIC_GRAPHICS_MEMORY, "Graphics payload", "MiB", "Not instrumented", "Current; includes callback", 2},
    [METRIC_ALL_MEMORY] = {METRIC_ALL_MEMORY, "All graphics payload", "MiB", "Not instrumented", "Current; all sessions", 2},
    [METRIC_GRAPHICS_WAIT] = {METRIC_GRAPHICS_WAIT, "Graphics queue wait p95", "ms", "Not instrumented", "Published ~5 s window", 2},
    [METRIC_NDL_MAX] = {METRIC_NDL_MAX, "NDL call maximum", "ms", "NDL adapter", "Decoder load", 2},
    [METRIC_NDL_ERRORS] = {METRIC_NDL_ERRORS, "NDL errors", "", "NDL adapter", "Decoder load", 0},
    [METRIC_NDL_QUEUE] = {METRIC_NDL_QUEUE, "NDL shared queue", "fr", "NDL cached report", "Last available report", 0},
    [METRIC_PENDING] = {METRIC_PENDING, "Decoder pending", "fr", "backend_ndl decoder", "Live queue", 0},
    [METRIC_REPLAY] = {METRIC_REPLAY, "Cache replay", "AU", "Video callback", "Graphics generation", 0},
    [METRIC_ACCEPTED] = {METRIC_ACCEPTED, "Accepted input", "AU", "Video callback", "Graphics generation", 0},
    [METRIC_DISCONTINUITIES] = {METRIC_DISCONTINUITIES, "Audio discontinuities", "", "Not instrumented", "Source lifetime", 0},
    [METRIC_NET_RX] = {METRIC_NET_RX, "Receive throughput", "Mbit/s", "PollingSocket RX incl. TLS", "Server to TV; 5 s average; sampled 500 ms", 2},
    [METRIC_NET_TX] = {METRIC_NET_TX, "Transmit throughput", "Mbit/s", "PollingSocket TX incl. TLS", "TV to server; 5 s average; sampled 500 ms", 2},
    [METRIC_NET_RTT] = {METRIC_NET_RTT, "RDP average RTT", "ms", "Server RDP Auto-Detect", "Round trip; latest active-session report", 2},
    [METRIC_NET_BANDWIDTH] = {METRIC_NET_BANDWIDTH, "RDP bandwidth estimate", "Mbit/s", "Server RDP Auto-Detect", "Server estimate; not interface speed", 2},
    [METRIC_NET_STATE] = {METRIC_NET_STATE, "Connection state", "", "RDP worker", "Current state", 0},
    [METRIC_NET_UPTIME] = {METRIC_NET_UPTIME, "Connection uptime", "s", "Worker monotonic clock", "Since Active; resets on reconnect; 500 ms", 0},
    [METRIC_NET_TRANSPORT] = {METRIC_NET_TRANSPORT, "Transport", "", "RDP transport", "TCP payload includes TLS; no IP/TCP headers", 0},
    [METRIC_NET_RX_AGE] = {METRIC_NET_RX_AGE, "Last received data age", "ms", "PollingSocket RX", "Server to TV; latest positive read; 500 ms", 0},
    [METRIC_NET_RX_BYTES] = {METRIC_NET_RX_BYTES, "Received bytes", "MiB", "PollingSocket RX incl. TLS", "Server to TV; since Active; no retransmits", 2},
    [METRIC_NET_TX_BYTES] = {METRIC_NET_TX_BYTES, "Transmitted bytes", "MiB", "PollingSocket TX incl. TLS", "TV to server; since Active; no retransmits", 2},
    [METRIC_NET_BASE_RTT] = {METRIC_NET_BASE_RTT, "RDP base RTT", "ms", "Server RDP Auto-Detect", "Lowest RTT; latest report containing field", 2},
    [METRIC_NET_REPORT_AGE] = {METRIC_NET_REPORT_AGE, "RDP report age", "ms", "Worker monotonic clock", "Latest active-session report; 500 ms", 0},
    [METRIC_NET_REPORTS] = {METRIC_NET_REPORTS, "RDP reports", "", "Server RDP Auto-Detect", "Active connection lifetime; 500 ms", 0},
    [METRIC_NET_QOS_STATE] = {METRIC_NET_QOS_STATE, "RDP measurement state", "", "Server RDP Auto-Detect", "No report: Collecting; age > 7.5 s: Stale", 0},
    [METRIC_NET_SRTT] = {METRIC_NET_SRTT, "TCP SRTT", "ms", "Linux TCP_INFO", "TV sender smoothed RTT; sampled 500 ms", 2},
    [METRIC_NET_RTTVAR] = {METRIC_NET_RTTVAR, "TCP RTTVAR", "ms", "Linux TCP_INFO", "TV sender RTT variation; not arrival jitter; 500 ms", 2},
    [METRIC_NET_RTO] = {METRIC_NET_RTO, "TCP RTO", "ms", "Linux TCP_INFO", "TV sender retransmission timeout; 500 ms", 2},
    [METRIC_NET_UNACKED] = {METRIC_NET_UNACKED, "Unacknowledged segments", "", "Linux TCP_INFO", "TV outgoing segments; sampled 500 ms", 0},
    [METRIC_NET_CWND] = {METRIC_NET_CWND, "Congestion window", "segments", "Linux TCP_INFO", "TV sender window; sampled 500 ms", 0},
    [METRIC_NET_RETRANSMITS] = {METRIC_NET_RETRANSMITS, "Outgoing retransmissions", "", "Linux TCP_INFO", "TV socket lifetime; segments, not video loss; 500 ms", 0},

};

const NativeMetricDescription *native_statistics_metric(NativeMetricId id) {
    return id >= 0 && id < METRIC_COUNT ? &metrics[id] : NULL;
}

const char *native_statistics_state_name(NativeMetricState state) {
    static const char *const names[] = {"Valid", "Collecting", "Stale", "Unavailable", "Not measured"};
    return state >= NATIVE_METRIC_VALID && state <= NATIVE_METRIC_UNSUPPORTED ? names[state] : "Unavailable";
}

static NativeMetricState window_state(bool available, uint32_t samples, uint32_t age) {
    if (!available) return NATIVE_METRIC_UNAVAILABLE;
    if (!samples) return NATIVE_METRIC_COLLECTING;
    return age > 7500 ? NATIVE_METRIC_STALE : NATIVE_METRIC_VALID;
}

static NativeMetricState network_state(const NativeDiagnosticsSnapshot *s, uint64_t at) {
    const RdpNetworkStats *n = &s->network;
    if (!s->connected || (n->generation && !n->active)) return NATIVE_METRIC_UNAVAILABLE;
    if (!n->sampled_ms) return NATIVE_METRIC_COLLECTING;
    if (n->observed_ms - n->sampled_ms > 7500) return NATIVE_METRIC_STALE;
    if (!at) return NATIVE_METRIC_COLLECTING;
    return n->observed_ms - at > 7500 ? NATIVE_METRIC_STALE : NATIVE_METRIC_VALID;
}

static NativeMetricValue network_value(const NativeDiagnosticsSnapshot *s, NativeMetricId id) {
    const RdpNetworkStats *n = &s->network;
    NativeMetricValue v = {.state = network_state(s, n->sampled_ms)};
    double value = 0;
    switch (id) {
    case METRIC_NET_STATE:
        v.state = s->connection_status ? NATIVE_METRIC_VALID : NATIVE_METRIC_UNAVAILABLE;
        snprintf(v.text, sizeof(v.text), "%s", s->connection_status ? s->connection_status : "Unavailable"); break;
    case METRIC_NET_TRANSPORT: snprintf(v.text, sizeof(v.text), "TCP / TLS"); break;
    case METRIC_NET_UPTIME: value = (n->observed_ms - n->started_ms) / 1000; break;
    case METRIC_NET_RX: case METRIC_NET_TX:
        if (v.state == NATIVE_METRIC_VALID && !n->rate_valid) v.state = NATIVE_METRIC_COLLECTING;
        value = (id == METRIC_NET_RX ? n->rx_kbps : n->tx_kbps) / 1000.0; break;
    case METRIC_NET_RX_BYTES: value = n->rx_bytes / 1048576.0; break;
    case METRIC_NET_TX_BYTES: value = n->tx_bytes / 1048576.0; break;
    case METRIC_NET_RX_AGE:
        if (v.state == NATIVE_METRIC_VALID && !n->last_rx_ms) v.state = NATIVE_METRIC_COLLECTING;
        value = n->observed_ms - n->last_rx_ms; break;
    case METRIC_NET_RTT: v.state = network_state(s, n->report_ms); value = n->average_rtt_ms; break;
    case METRIC_NET_BASE_RTT: v.state = network_state(s, n->base_rtt_ms_at); value = n->base_rtt_ms; break;
    case METRIC_NET_BANDWIDTH: v.state = network_state(s, n->bandwidth_ms_at); value = n->bandwidth_kbps / 1000.0; break;
    case METRIC_NET_REPORT_AGE: v.state = network_state(s, n->report_ms); value = n->observed_ms - n->report_ms; break;
    case METRIC_NET_REPORTS: value = n->reports; break;
    case METRIC_NET_QOS_STATE:
        v.state = network_state(s, n->report_ms);
        snprintf(v.text, sizeof(v.text), "%s", native_statistics_state_name(v.state)); break;
    case METRIC_NET_SRTT: case METRIC_NET_RTTVAR: case METRIC_NET_RTO:
    case METRIC_NET_UNACKED: case METRIC_NET_CWND: case METRIC_NET_RETRANSMITS: {
        unsigned bit = (unsigned)(id - METRIC_NET_SRTT);
        const uint32_t values[] = {n->tcp_srtt_us, n->tcp_rttvar_us, n->tcp_rto_us, n->tcp_unacked, n->tcp_cwnd, n->tcp_retransmits};
        if (v.state == NATIVE_METRIC_VALID && !(n->tcp_valid & (1u << bit))) v.state = NATIVE_METRIC_UNSUPPORTED;
        value = values[bit] / (bit < 3 ? 1000.0 : 1.0); break;
    }
    default: v.state = NATIVE_METRIC_UNSUPPORTED; break;
    }
    if (!v.text[0]) snprintf(v.text, sizeof(v.text), "%.*f", (int)metrics[id].decimals, value);
    return v;
}

NativeMetricValue native_statistics_value(const NativeDiagnosticsSnapshot *s, NativeMetricId id) {
    NativeMetricValue v = {.state = NATIVE_METRIC_VALID};
    const NativeMetricDescription *m = native_statistics_metric(id);
    if (!s || !m) { v.state = NATIVE_METRIC_UNAVAILABLE; goto finish; }
    if (id >= METRIC_NET_RX) { v = network_value(s, id); goto finish; }
    const NativeDiagnosticsAudio *audio = &s->audio[s->slot >= 0 && s->slot < 4 ? s->slot : 0];
    const NativeAudioSourceStats *a = &audio->source;
    NativeMetricState as = !audio->connected || !a->open ? NATIVE_METRIC_UNAVAILABLE : NATIVE_METRIC_VALID;
    NativeMetricState ds = s->bitmap || s->codec != NATIVE_VIDEO_CODEC_H264 ? NATIVE_METRIC_UNSUPPORTED :
        window_state(s->decoder.available, s->decoder.window_ms, s->at_ms - s->decoder.at_ms);
    double n = 0;
    uint64_t count = 0;
    bool integer = false;
    switch (id) {
    case METRIC_AUS: case METRIC_MBPS: case METRIC_MAIN_AUX:
        v.state = s->bitmap ? NATIVE_METRIC_UNSUPPORTED : !s->connected ? NATIVE_METRIC_UNAVAILABLE :
            s->rates.valid ? NATIVE_METRIC_VALID : NATIVE_METRIC_COLLECTING;
        n = id == METRIC_AUS ? s->rates.input_aus : s->rates.input_mbps;
        if (id == METRIC_MAIN_AUX) snprintf(v.text, sizeof(v.text), "%.2f / %.2f", s->rates.input_mbps - s->rates.input_auxiliary_mbps, s->rates.input_auxiliary_mbps);
        break;
    case METRIC_AU_AGE:
        v.state = s->bitmap ? NATIVE_METRIC_UNSUPPORTED : !s->connected ? NATIVE_METRIC_UNAVAILABLE :
            s->counters.has_au ? NATIVE_METRIC_VALID : NATIVE_METRIC_COLLECTING;
        n = (uint32_t)(s->at_ms - s->counters.last_au_ms); break;
    case METRIC_DECODED:
        v.state = s->bitmap || s->codec != NATIVE_VIDEO_CODEC_H264 ? NATIVE_METRIC_UNSUPPORTED :
            !s->connected || !s->decoder.available ? NATIVE_METRIC_UNAVAILABLE :
            s->rates.decoded_valid ? NATIVE_METRIC_VALID : NATIVE_METRIC_COLLECTING;
        n = s->rates.decoded_fps; break;
    case METRIC_DECODE_TIME: case METRIC_PRIMARY_INTERVAL: case METRIC_PRIMARY_GAPS: case METRIC_PENDING:
        v.state = ds;
        if (id == METRIC_DECODE_TIME) n = s->decoder.decode_p95_us / 1000.0;
        if (id == METRIC_PENDING) {
            if (ds != NATIVE_METRIC_UNSUPPORTED)
                v.state = s->decoder.available ? NATIVE_METRIC_VALID : NATIVE_METRIC_UNAVAILABLE;
            n = s->decoder.pending;
        }
        if (id == METRIC_PRIMARY_GAPS) n = s->decoder.gaps;
        if (id == METRIC_PRIMARY_INTERVAL) {
            if (v.state == NATIVE_METRIC_VALID && !s->decoder.intervals) v.state = NATIVE_METRIC_COLLECTING;
            snprintf(v.text, sizeof(v.text), "%.2f / %.2f", s->decoder.interval_p95_us / 1000.0, s->decoder.interval_max_us / 1000.0);
        }
        break;
    case METRIC_NDL: case METRIC_NDL_MAX: case METRIC_NDL_ERRORS:
        v.state = s->bitmap ? NATIVE_METRIC_UNSUPPORTED : s->ndl.timing_valid ? NATIVE_METRIC_VALID :
            s->video_available ? NATIVE_METRIC_COLLECTING : NATIVE_METRIC_UNAVAILABLE;
        n = id == METRIC_NDL ? s->ndl.play_us_ewma / 1000.0 : id == METRIC_NDL_MAX ? s->ndl.play_us_max / 1000.0 : s->ndl.errors; break;
    case METRIC_NDL_QUEUE:
        v.state = s->bitmap ? NATIVE_METRIC_UNSUPPORTED : window_state(s->ndl.queue_valid, 1, s->at_ms - s->ndl.sampled_ms);
        n = s->ndl.queue_frames; break;
    case METRIC_CPU: v.state = s->cpu_valid ? NATIVE_METRIC_VALID : NATIVE_METRIC_COLLECTING; n = s->cpu_percent; break;
    case METRIC_RSS: v.state = s->memory_valid ? NATIVE_METRIC_VALID : NATIVE_METRIC_UNAVAILABLE; n = s->rss_bytes / 1048576.0; break;
    case METRIC_BUFFER: v.state = as; snprintf(v.text, sizeof(v.text), "%u / %u", a->queue_ms, a->target_delay_ms); break;
    case METRIC_JITTER: v.state = as; n = a->jitter_p95_ms; break;
    case METRIC_SRC: v.state = as; n = a->src_correction_ppm; break;
    case METRIC_UNDERRUNS: v.state = as; count = a->underruns; integer = true; break;
    case METRIC_TRIMS: v.state = as; snprintf(v.text, sizeof(v.text), "%" PRIu64 " / %" PRIu64, a->hard_corrections, a->overflows); break;
    case METRIC_REPLAY: case METRIC_ACCEPTED:
        v.state = s->bitmap ? NATIVE_METRIC_UNSUPPORTED : s->video_available ? NATIVE_METRIC_VALID : NATIVE_METRIC_COLLECTING;
        count = id == METRIC_REPLAY ? s->counters.replayed : s->counters.accepted; integer = true; break;
    default: v.state = NATIVE_METRIC_UNSUPPORTED; break;
    }
    if (!v.text[0]) {
        if (integer) snprintf(v.text, sizeof(v.text), "%" PRIu64, count);
        else snprintf(v.text, sizeof(v.text), "%.*f", (int)m->decimals, n);
    }
finish:
    if (v.state != NATIVE_METRIC_VALID) snprintf(v.text, sizeof(v.text), "%s", native_statistics_state_name(v.state));
    static const char *const reasons[] = {
        "Available from the stated source and scope.", "Waiting for a usable sample or published window.",
        "The producer's last report is older than 7.5 seconds; no new value is invented.",
        "The source is closed, absent, or has no usable report.", "This pipeline does not measure this field. No zero is substituted."
    };
    v.reason = reasons[v.state];
    return v;
}

int native_statistics_x(uint32_t now_ms, uint32_t at_ms) {
    uint32_t age = now_ms - at_ms;
    if (age > NATIVE_DIAGNOSTICS_HISTORY_MS) return -1;
    return (int)((NATIVE_DIAGNOSTICS_HISTORY_MS - age) / 100u);
}

void native_statistics_series_add(NativeStatisticsSeries *s, uint32_t at, double value, bool valid) {
    if (s->count && s->points[s->count - 1].at_ms == at) return;
    unsigned expired = 0;
    while (expired < s->count && at - s->points[expired].at_ms > NATIVE_DIAGNOSTICS_HISTORY_MS) expired++;
    if (s->count - expired == NATIVE_DIAGNOSTICS_HISTORY_POINTS) expired++;
    s->count -= expired;
    memmove(s->points, s->points + expired, s->count * sizeof(s->points[0]));
    s->points[s->count++] = (NativeStatisticsPoint){at, value, valid && isfinite(value) && value >= 0};
}

void native_statistics_sample(NativeStatisticsTimeline *h, NativeDiagnosticsSnapshot *s) {
    const NativeDiagnosticsAudio *a = &s->audio[s->slot >= 0 && s->slot < 4 ? s->slot : 0];
    bool source_changed = !h->started || s->rates.discontinuity || h->slot != s->slot || h->epoch != s->counters.epoch || h->generation != s->counters.generation;
    if (source_changed) memset(h, 0, sizeof(*h));
    const NativeAudioSourceStats *before = &h->audio.source, *now = &a->source;
    bool audio_changed = source_changed || a->connected != h->audio.connected || now->open != before->open ||
        a->codec != h->audio.codec || a->channels != h->audio.channels || a->sample_rate != h->audio.sample_rate ||
        now->generation != before->generation || now->input_sample_rate != before->input_sample_rate;
    if (audio_changed) for (int i = NATIVE_STATS_PCM; i < NATIVE_STATS_NET_RX; i++) h->series[i].count = 0;
    uint32_t dt = s->at_ms - h->at_ms;
    bool video = s->rates.valid && s->connected && !s->bitmap;
    native_statistics_series_add(&h->series[NATIVE_STATS_AUS], s->at_ms, s->rates.input_aus, video);
    native_statistics_series_add(&h->series[NATIVE_STATS_MBPS], s->at_ms, s->rates.input_mbps, video);
    bool decoder = s->decoder.available && s->connected && !s->bitmap && s->codec == NATIVE_VIDEO_CODEC_H264;
    uint32_t decoded = s->decoder.primary_total - h->decoder_primary;
    if (source_changed || h->decoder_generation != s->decoder.generation || (decoder && h->decoder_seen && decoded > INT32_MAX)) {
        h->series[NATIVE_STATS_DECODED].count = 0;
        h->decoder_seen = false;
    }
    s->rates.decoded_valid = decoder && h->decoder_seen && dt && dt <= 2 * NATIVE_DIAGNOSTICS_INTERVAL_MS;
    s->rates.decoded_fps = s->rates.decoded_valid ? decoded * 1000.0 / dt : 0;
    native_statistics_series_add(&h->series[NATIVE_STATS_DECODED], s->at_ms, s->rates.decoded_fps, s->rates.decoded_valid);
    h->decoder_primary = s->decoder.primary_total;
    h->decoder_seen = decoder;
    uint32_t received = now->received_frames - before->received_frames;
    uint32_t output = now->output_frames - before->output_frames;
    bool flow = !audio_changed && a->connected && now->open && now->input_sample_rate &&
        dt && dt <= 2 * NATIVE_DIAGNOSTICS_INTERVAL_MS && received <= INT32_MAX && output <= INT32_MAX;
    double pcm = flow ? received * 1000000.0 / now->input_sample_rate / dt : 0;
    double src = flow ? output * 1000000.0 / NATIVE_AUDIO_PIPELINE_SAMPLE_RATE / dt : 0;
    native_statistics_series_add(&h->series[NATIVE_STATS_PCM], s->at_ms, pcm, flow);
    native_statistics_series_add(&h->series[NATIVE_STATS_SRC], s->at_ms, src, flow);
    bool open = a->connected && now->open;
    native_statistics_series_add(&h->series[NATIVE_STATS_BUFFER], s->at_ms, now->queue_ms, open);
    native_statistics_series_add(&h->series[NATIVE_STATS_TARGET], s->at_ms, now->target_delay_ms, open);
    native_statistics_series_add(&h->series[NATIVE_STATS_AGE], s->at_ms, 0, false);
    if (source_changed || h->network_generation != s->network.generation)
        for (int i = NATIVE_STATS_NET_RX; i < NATIVE_STATS_SERIES; i++) h->series[i].count = 0;
    const RdpNetworkStats *net = &s->network;
    bool fresh = network_state(s, net->sampled_ms) == NATIVE_METRIC_VALID;
    native_statistics_series_add(&h->series[NATIVE_STATS_NET_RX], s->at_ms, net->rx_kbps / 1000.0, fresh && net->rate_valid);
    native_statistics_series_add(&h->series[NATIVE_STATS_NET_TX], s->at_ms, net->tx_kbps / 1000.0, fresh && net->rate_valid);
    native_statistics_series_add(&h->series[NATIVE_STATS_NET_RTT], s->at_ms, net->average_rtt_ms, network_state(s, net->report_ms) == NATIVE_METRIC_VALID);
    native_statistics_series_add(&h->series[NATIVE_STATS_NET_SRTT], s->at_ms, net->tcp_srtt_us / 1000.0, fresh && (net->tcp_valid & 1));
    h->network_generation = net->generation;
    h->slot = s->slot; h->epoch = s->counters.epoch; h->generation = s->counters.generation;
    h->audio = *a; h->at_ms = s->at_ms; h->decoder_generation = s->decoder.generation; h->started = true;
    s->timeline = h;
}

void native_statistics_focus_reset(NativeStatisticsNavigation *n) {
    n->focus = n->held = 0;
    n->help = false;
}

bool native_statistics_key(NativeStatisticsNavigation *n, NativeStatisticsKey key, bool down, bool repeat, bool *hub) {
    *hub = false;
    if (key <= NATIVE_STATS_KEY_NONE || key > NATIVE_STATS_KEY_BACK) return false;
    unsigned bit = 1u << key;
    bool held = (n->held & bit) != 0;
    if (down) n->held |= bit;
    else n->held &= ~bit;
    if ((down && (repeat || held)) || (!down && (!held || key != NATIVE_STATS_KEY_OK))) return true;
    if (down && key == NATIVE_STATS_KEY_OK) return true;
    if (n->help) {
        if (key == NATIVE_STATS_KEY_BACK || key == NATIVE_STATS_KEY_OK) n->help = false;
        else if (key == NATIVE_STATS_KEY_LEFT || key == NATIVE_STATS_KEY_RIGHT) n->help_page ^= 1u;
        return true;
    }
    if (key == NATIVE_STATS_KEY_BACK) { n->focus = 0; return true; }
    bool categories = n->tab == NATIVE_STATS_VIDEO || n->tab == NATIVE_STATS_AUDIO || n->tab == NATIVE_STATS_NETWORK;
    unsigned last = 1u + categories;
    if (key == NATIVE_STATS_KEY_DOWN) { if (n->focus < last) n->focus++; }
    else if (key == NATIVE_STATS_KEY_UP) { if (n->focus) n->focus--; }
    else if (key == NATIVE_STATS_KEY_OK && !n->focus) *hub = true;
    else if (n->focus == last && key == NATIVE_STATS_KEY_OK) { n->help = true; n->help_page = 0; }
    else if (key == NATIVE_STATS_KEY_LEFT || key == NATIVE_STATS_KEY_RIGHT || key == NATIVE_STATS_KEY_OK) {
        int direction = key == NATIVE_STATS_KEY_LEFT ? -1 : 1;
        if (!n->focus) n->tab = (n->tab + NATIVE_STATS_TABS + direction) % NATIVE_STATS_TABS;
        else if (categories && n->focus == 1) {
            unsigned *category = n->tab == NATIVE_STATS_VIDEO ? &n->video_category :
                n->tab == NATIVE_STATS_AUDIO ? &n->audio_category : &n->network_category;
            unsigned count = n->tab == NATIVE_STATS_VIDEO ? 4 : 3;
            *category = (*category + count + direction) % count;
        }
    }
    return true;
}
