#include "native_statistics.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static void availability(void) {
    NativeDiagnosticsSnapshot s = {.connected = true, .at_ms = 10000};
    assert(native_statistics_value(&s, METRIC_AUS).state == NATIVE_METRIC_COLLECTING);
    assert(native_statistics_value(&s, METRIC_PRESENTED).state == NATIVE_METRIC_UNSUPPORTED);
    assert(native_statistics_value(&s, METRIC_BUFFER).state == NATIVE_METRIC_UNAVAILABLE);
    s.rates.valid = true;
    assert(!strcmp(native_statistics_value(&s, METRIC_AUS).text, "0.0"));
    s.bitmap = true;
    assert(native_statistics_value(&s, METRIC_AUS).state == NATIVE_METRIC_UNSUPPORTED);
    s.bitmap = false;
    s.decoder = (NativeDecoderStats){.available = true, .window_ms = 5000, .primary = 150, .at_ms = 5000};
    assert(native_statistics_value(&s, METRIC_DECODED).state == NATIVE_METRIC_COLLECTING);
    s.rates.decoded_valid = true; s.rates.decoded_fps = 30;
    assert(!strcmp(native_statistics_value(&s, METRIC_DECODED).text, "30.0"));
    s.at_ms = 12501;
    assert(native_statistics_value(&s, METRIC_DECODED).state == NATIVE_METRIC_VALID);
    assert(native_statistics_value(&s, METRIC_DECODE_TIME).state == NATIVE_METRIC_STALE);
    s.at_ms = 20; s.decoder.at_ms = UINT32_MAX - 100;
    assert(native_statistics_value(&s, METRIC_DECODE_TIME).state == NATIVE_METRIC_VALID);
    s.rates.decoded_valid = false;
    assert(native_statistics_value(&s, METRIC_DECODED).state == NATIVE_METRIC_COLLECTING);
    s.decoder.available = false;
    assert(native_statistics_value(&s, METRIC_DECODED).state == NATIVE_METRIC_UNAVAILABLE);
    s.audio[0].connected = s.audio[0].source.open = true;
    assert(native_statistics_value(&s, METRIC_AUDIO_AGE).state == NATIVE_METRIC_UNSUPPORTED);
    assert(native_statistics_value(&s, METRIC_JITTER).state == NATIVE_METRIC_VALID);
    assert(!strcmp(native_statistics_value(&s, METRIC_JITTER).text, "0"));
    const NativeMetricId unmeasured[] = {METRIC_RDP, METRIC_GRAPHICS_QUEUE, METRIC_GRAPHICS_AGE,
        METRIC_GRAPHICS_TIME, METRIC_GRAPHICS_WAIT, METRIC_GRAPHICS_MEMORY, METRIC_ALL_MEMORY,
        METRIC_AUDIO_AGE, METRIC_PCM_INTERVAL, METRIC_PCM_FEED, METRIC_DROPPED, METRIC_DISCONTINUITIES};
    for (unsigned i = 0; i < sizeof(unmeasured) / sizeof(unmeasured[0]); i++) {
        NativeMetricValue value = native_statistics_value(&s, unmeasured[i]);
        assert(value.state == NATIVE_METRIC_UNSUPPORTED && !strcmp(value.text, "Not measured"));
    }
    s.audio[0].source.underruns = UINT64_MAX;
    assert(!strcmp(native_statistics_value(&s, METRIC_UNDERRUNS).text, "18446744073709551615"));
    for (int i = 0; i < METRIC_COUNT; i++) {
        const NativeMetricDescription *m = native_statistics_metric(i);
        assert(m && m->id == i && m->label && m->unit && m->source && m->scope);
        NativeMetricValue v = native_statistics_value(&s, i);
        assert(v.reason && v.text[0]);
        if (v.state != NATIVE_METRIC_VALID) assert(strcmp(v.text, "0"));
    }
}

static void decoder_pending(void) {
    NativeDiagnosticsSnapshot s = {.connected = true, .codec = NATIVE_VIDEO_CODEC_H264,
        .at_ms = 10000, .decoder = {.available = true, .pending = 3}};
    assert(native_statistics_value(&s, METRIC_DECODE_TIME).state == NATIVE_METRIC_COLLECTING);
    assert(native_statistics_value(&s, METRIC_PENDING).state == NATIVE_METRIC_VALID);
    assert(!strcmp(native_statistics_value(&s, METRIC_PENDING).text, "3"));
    s.decoder.window_ms = 5000;
    assert(native_statistics_value(&s, METRIC_DECODE_TIME).state == NATIVE_METRIC_STALE);
    assert(native_statistics_value(&s, METRIC_PENDING).state == NATIVE_METRIC_VALID);
    s.codec = NATIVE_VIDEO_CODEC_H265;
    assert(native_statistics_value(&s, METRIC_PENDING).state == NATIVE_METRIC_UNSUPPORTED);
    assert(!strcmp(native_statistics_value(&s, METRIC_PENDING).text, "Not measured"));
    s.codec = NATIVE_VIDEO_CODEC_H264;
    s.bitmap = true;
    assert(native_statistics_value(&s, METRIC_PENDING).state == NATIVE_METRIC_UNSUPPORTED);
    s.bitmap = false;
    s.decoder.available = false;
    assert(native_statistics_value(&s, METRIC_PENDING).state == NATIVE_METRIC_UNAVAILABLE);
}

static void network_metrics(void) {
    NativeDiagnosticsSnapshot s = {.connected = true, .connection_status = "Active"};
    for (int id = METRIC_NET_RX; id < METRIC_COUNT; id++) {
        if (id != METRIC_NET_STATE) assert(native_statistics_value(&s, id).state == NATIVE_METRIC_COLLECTING);
    }
    s.network = (RdpNetworkStats){.generation = 1, .active = 1, .started_ms = 1,
        .sampled_ms = 9000, .observed_ms = 9000, .report_ms = 9000, .reports = 2,
        .base_rtt_ms_at = 9000, .bandwidth_ms_at = 1, .bandwidth_kbps = 100000,
        .rate_valid = 1, .rx_kbps = 12000, .tx_kbps = 500,
        .rx_bytes = 1048576, .last_rx_ms = 2, .tcp_valid = 63, .tcp_srtt_us = 1250};
    assert(!strcmp(native_statistics_value(&s, METRIC_NET_TRANSPORT).text, "TCP / TLS"));
    assert(!strcmp(native_statistics_value(&s, METRIC_NET_RX).text, "12.00"));
    assert(!strcmp(native_statistics_value(&s, METRIC_NET_TX).text, "0.50"));
    assert(!strcmp(native_statistics_value(&s, METRIC_NET_RX_BYTES).text, "1.00"));
    assert(!strcmp(native_statistics_value(&s, METRIC_NET_RTT).text, "0.00"));
    assert(!strcmp(native_statistics_value(&s, METRIC_NET_BASE_RTT).text, "0.00"));
    assert(!strcmp(native_statistics_value(&s, METRIC_NET_SRTT).text, "1.25"));
    assert(native_statistics_value(&s, METRIC_NET_BANDWIDTH).state == NATIVE_METRIC_STALE);
    assert(native_statistics_value(&s, METRIC_NET_RX_AGE).state == NATIVE_METRIC_VALID);
    s.network.tcp_valid = 0;
    assert(native_statistics_value(&s, METRIC_NET_SRTT).state == NATIVE_METRIC_UNSUPPORTED);
    s.network.observed_ms += 7501;
    assert(native_statistics_value(&s, METRIC_NET_RX).state == NATIVE_METRIC_STALE);
    assert(native_statistics_value(&s, METRIC_NET_RTT).state == NATIVE_METRIC_STALE);
    s.connected = false;
    for (int id = METRIC_NET_RX; id < METRIC_COUNT; id++) {
        if (id != METRIC_NET_STATE) assert(native_statistics_value(&s, id).state == NATIVE_METRIC_UNAVAILABLE);
    }
    NativeStatisticsTimeline history = {0};
    s.connected = true; s.network.observed_ms = s.network.sampled_ms;
    s.at_ms = 1000; native_statistics_sample(&history, &s);
    s.at_ms += 500; native_statistics_sample(&history, &s);
    assert(history.series[NATIVE_STATS_NET_RX].count == 2);
    s.network.generation++; s.network.rate_valid = 0;
    s.at_ms += 500; native_statistics_sample(&history, &s);
    assert(history.series[NATIVE_STATS_NET_RX].count == 1);
    assert(!history.series[NATIVE_STATS_NET_RX].points[0].valid);
    s.slot = 1; s.at_ms += 500; native_statistics_sample(&history, &s);
    assert(history.series[NATIVE_STATS_NET_RX].count == 1);
}

static void ranges(void) {
    assert(native_statistics_x(60000, 30000) == 0);
    assert(native_statistics_x(60000, 29999) == -1);
    assert(native_statistics_x(60000, 60000) == 300);
    assert(native_statistics_x(60000, 0) == -1);
    assert(native_statistics_x(10, UINT32_MAX - 89) == 299);
    assert(native_statistics_x(10, UINT32_MAX - 29989) == 0);
    assert(native_statistics_x(10, UINT32_MAX - 29990) == -1);
    assert(native_statistics_x(100, 101) == -1);
    NativeStatisticsSeries series = {0};
    for (uint32_t at = 0; at <= 100000; at += 500) native_statistics_series_add(&series, at, at, true);
    assert(series.count == 61 && series.points[0].at_ms == 70000);
    native_statistics_series_add(&series, 100000, 1, true);
    assert(series.count == 61 && series.points[60].value == 100000);
    native_statistics_series_add(&series, 100500, 0, false);
    assert(!series.points[60].valid);
    native_statistics_series_add(&series, 200000, NAN, true);
    assert(series.count == 1 && !series.points[0].valid);
}

static NativeStatisticsPoint latest(const NativeStatisticsTimeline *h, int id) {
    const NativeStatisticsSeries *s = &h->series[id];
    assert(s->count); return s->points[s->count - 1];
}

static void decoder_live_rates(void) {
    NativeStatisticsTimeline h = {0};
    NativeDiagnosticsSnapshot s = {.connected = true, .at_ms = UINT32_MAX - 499,
        .decoder = {.available = true, .primary_total = UINT32_MAX - 9}};
    native_statistics_sample(&h, &s);
    assert(!latest(&h, NATIVE_STATS_DECODED).valid);
    s.at_ms = 0; s.decoder.primary_total = 5;
    native_statistics_sample(&h, &s);
    assert(s.rates.decoded_valid && s.rates.decoded_fps == 30);
    assert(latest(&h, NATIVE_STATS_DECODED).valid && latest(&h, NATIVE_STATS_DECODED).value == 30);
    s.at_ms = 500;
    native_statistics_sample(&h, &s);
    assert(s.rates.decoded_valid && s.rates.decoded_fps == 0);
    assert(!strcmp(native_statistics_value(&s, METRIC_DECODED).text, "0.0"));
    s.at_ms = 1000; s.decoder.primary_total += 15;
    native_statistics_sample(&h, &s);
    assert(s.rates.decoded_valid && s.rates.decoded_fps == 30);
    native_statistics_sample(&h, &s);
    assert(h.series[NATIVE_STATS_DECODED].count == 4);
    s.at_ms = 1500; s.decoder.available = false; s.decoder.primary_total = 0;
    native_statistics_sample(&h, &s);
    assert(h.series[NATIVE_STATS_DECODED].count == 5 && !latest(&h, NATIVE_STATS_DECODED).valid);
    s.at_ms = 2000; s.decoder.available = true; s.decoder.primary_total = 60;
    native_statistics_sample(&h, &s);
    assert(!s.rates.decoded_valid);
    s.at_ms = 2500; s.decoder.primary_total += 15;
    native_statistics_sample(&h, &s);
    assert(s.rates.decoded_valid && s.rates.decoded_fps == 30);
    s.at_ms = 4000; s.decoder.primary_total += 30;
    native_statistics_sample(&h, &s);
    assert(!latest(&h, NATIVE_STATS_DECODED).valid);
    s.at_ms = 4500; s.decoder.primary_total += 15;
    native_statistics_sample(&h, &s);
    assert(s.rates.decoded_valid && s.rates.decoded_fps == 30);
    s.at_ms = 5000; s.decoder.primary_total = 0;
    native_statistics_sample(&h, &s);
    assert(!s.rates.decoded_valid && h.series[NATIVE_STATS_DECODED].count == 1);
    s.at_ms = 5500; s.decoder.primary_total = 15; s.decoder.generation++;
    native_statistics_sample(&h, &s);
    assert(!s.rates.decoded_valid && h.series[NATIVE_STATS_DECODED].count == 1);
    s.at_ms = 6000; s.decoder.primary_total += 15;
    native_statistics_sample(&h, &s);
    assert(s.rates.decoded_valid && s.rates.decoded_fps == 30);
    s.at_ms = 6500; s.bitmap = true;
    native_statistics_sample(&h, &s);
    assert(!s.rates.decoded_valid && !latest(&h, NATIVE_STATS_DECODED).valid);
}

static void audio_recovery_preserves_history(void) {
    NativeStatisticsTimeline h = {0};
    NativeDiagnosticsSnapshot s = {.connected = true, .rates = {.valid = true, .input_aus = 30}};
    s.audio[0] = (NativeDiagnosticsAudio){.connected = true, .sample_rate = 48000, .channels = 2,
        .source = {.open = true, .queue_ms = 40, .target_delay_ms = 40}};
    NativeAudioSourceStats *a = &s.audio[0].source;
    for (unsigned step = 0; step < 5; step++) {
        s.at_ms = 1000 + step * 500;
        a->queue_ms = step == 1 ? 0 : 40;
        if (step == 1) a->underruns++;
        if (step == 2) a->overflows++;
        if (step == 3) a->hard_corrections++;
        native_statistics_sample(&h, &s);
        for (int id = NATIVE_STATS_PCM; id < NATIVE_STATS_NET_RX; id++) {
            assert(h.series[id].count == step + 1);
            assert(h.series[id].points[0].at_ms == 1000);
        }
        assert(latest(&h, NATIVE_STATS_BUFFER).valid && latest(&h, NATIVE_STATS_BUFFER).value == a->queue_ms);
        assert(latest(&h, NATIVE_STATS_TARGET).valid && latest(&h, NATIVE_STATS_TARGET).value == 40);
        assert(!latest(&h, NATIVE_STATS_PCM).valid && !latest(&h, NATIVE_STATS_SRC).valid);
        assert(!latest(&h, NATIVE_STATS_AGE).valid);
    }
}

static void audio_flow(void) {
    NativeStatisticsTimeline h = {0};
    NativeDiagnosticsSnapshot s = {.connected = true, .at_ms = 1000};
    s.audio[0].connected = true;
    NativeAudioSourceStats *a = &s.audio[0].source;
    a->open = true; a->input_sample_rate = 44100;
    a->received_frames = UINT32_MAX - 100; a->output_frames = UINT32_MAX - 100;
    native_statistics_sample(&h, &s);
    assert(!latest(&h, NATIVE_STATS_PCM).valid);
    s.at_ms += 500; a->received_frames += 22050; a->output_frames += 24000;
    native_statistics_sample(&h, &s);
    assert(latest(&h, NATIVE_STATS_PCM).valid && latest(&h, NATIVE_STATS_PCM).value == 1000);
    assert(latest(&h, NATIVE_STATS_SRC).valid && latest(&h, NATIVE_STATS_SRC).value == 1000);
    s.at_ms += 500;
    native_statistics_sample(&h, &s);
    assert(latest(&h, NATIVE_STATS_PCM).valid && latest(&h, NATIVE_STATS_PCM).value == 0);
    s.at_ms += 500; a->generation++;
    native_statistics_sample(&h, &s);
    assert(!latest(&h, NATIVE_STATS_PCM).valid);
    s.at_ms += 500; a->received_frames += 22050;
    native_statistics_sample(&h, &s);
    assert(latest(&h, NATIVE_STATS_PCM).valid);
    s.at_ms += 10000;
    native_statistics_sample(&h, &s);
    assert(!latest(&h, NATIVE_STATS_PCM).valid);
}

static void histories(void) {
    NativeStatisticsTimeline h = {0};
    NativeDiagnosticsSnapshot s = {.connected = true, .at_ms = UINT32_MAX - 499,
        .rates = {.valid = true, .input_aus = 30}};
    s.audio[0] = (NativeDiagnosticsAudio){.connected = true, .sample_rate = 44100, .channels = 2,
        .source = {.open = true, .queue_ms = 40, .target_delay_ms = 60}};
    native_statistics_sample(&h, &s);
    assert(latest(&h, NATIVE_STATS_BUFFER).valid && latest(&h, NATIVE_STATS_BUFFER).value == 40);
    s.at_ms = 0;
    native_statistics_sample(&h, &s);
    assert(h.series[NATIVE_STATS_BUFFER].count == 2);
    s.at_ms = 500; s.audio[0].sample_rate = 96000;
    native_statistics_sample(&h, &s);
    assert(h.series[NATIVE_STATS_BUFFER].count == 1);
    s.at_ms = 1000; s.audio[0].source.open = false;
    native_statistics_sample(&h, &s);
    assert(!latest(&h, NATIVE_STATS_BUFFER).valid && h.series[NATIVE_STATS_BUFFER].count == 1);
    s.at_ms = 1500; s.audio[0].source.open = true;
    native_statistics_sample(&h, &s);
    assert(latest(&h, NATIVE_STATS_BUFFER).valid);
    s.at_ms = 2000; s.decoder.generation++;
    native_statistics_sample(&h, &s);
    assert(h.series[NATIVE_STATS_DECODED].count == 1);
    s.at_ms = 2500; s.counters.generation++;
    native_statistics_sample(&h, &s);
    assert(h.series[NATIVE_STATS_AUS].count == 1);
    s.at_ms = 3000; s.slot = 1;
    native_statistics_sample(&h, &s);
    assert(h.series[NATIVE_STATS_AUS].count == 1 && !latest(&h, NATIVE_STATS_BUFFER).valid);
}

static bool press(NativeStatisticsNavigation *n, NativeStatisticsKey key) {
    bool hub;
    assert(native_statistics_key(n, key, true, false, &hub) && !hub);
    assert(native_statistics_key(n, key, false, false, &hub));
    return hub;
}

static void navigation(void) {
    NativeStatisticsNavigation n = {0}; native_statistics_focus_reset(&n);
    bool hub;
    native_statistics_key(&n, NATIVE_STATS_KEY_RIGHT, true, false, &hub);
    native_statistics_key(&n, NATIVE_STATS_KEY_RIGHT, true, true, &hub);
    native_statistics_key(&n, NATIVE_STATS_KEY_RIGHT, true, false, &hub);
    assert(n.tab == NATIVE_STATS_VIDEO);
    native_statistics_key(&n, NATIVE_STATS_KEY_RIGHT, false, false, &hub);
    press(&n, NATIVE_STATS_KEY_DOWN); press(&n, NATIVE_STATS_KEY_OK);
    assert(n.focus == 1 && n.video_category == 1);
    press(&n, NATIVE_STATS_KEY_RIGHT); assert(n.video_category == 2);
    press(&n, NATIVE_STATS_KEY_LEFT); assert(n.video_category == 1);
    press(&n, NATIVE_STATS_KEY_DOWN); press(&n, NATIVE_STATS_KEY_OK);
    assert(n.help && n.focus == 2);
    press(&n, NATIVE_STATS_KEY_RIGHT); assert(n.help_page == 1);
    press(&n, NATIVE_STATS_KEY_BACK); assert(!n.help && n.focus == 2);
    press(&n, NATIVE_STATS_KEY_BACK); assert(n.focus == 0);
    assert(press(&n, NATIVE_STATS_KEY_OK));
    native_statistics_key(&n, NATIVE_STATS_KEY_OK, true, false, &hub);
    native_statistics_focus_reset(&n);
    native_statistics_key(&n, NATIVE_STATS_KEY_OK, false, false, &hub); assert(!hub);
    assert(n.video_category == 1 && n.tab == NATIVE_STATS_VIDEO);
    press(&n, NATIVE_STATS_KEY_RIGHT); press(&n, NATIVE_STATS_KEY_RIGHT);
    assert(n.tab == NATIVE_STATS_NETWORK);
    press(&n, NATIVE_STATS_KEY_DOWN); press(&n, NATIVE_STATS_KEY_LEFT);
    assert(n.focus == 1 && n.network_category == 2);
    press(&n, NATIVE_STATS_KEY_OK); assert(n.network_category == 0);
    press(&n, NATIVE_STATS_KEY_RIGHT); assert(n.network_category == 1);
    press(&n, NATIVE_STATS_KEY_DOWN); press(&n, NATIVE_STATS_KEY_OK);
    assert(n.help && n.focus == 2);
    press(&n, NATIVE_STATS_KEY_BACK); press(&n, NATIVE_STATS_KEY_BACK);
    native_statistics_focus_reset(&n); assert(n.network_category == 1);
    press(&n, NATIVE_STATS_KEY_RIGHT); assert(n.tab == NATIVE_STATS_PERFORMANCE);
    press(&n, NATIVE_STATS_KEY_DOWN); assert(n.focus == 1);
    press(&n, NATIVE_STATS_KEY_OK); assert(n.help);
    native_statistics_focus_reset(&n);
    press(&n, NATIVE_STATS_KEY_RIGHT); press(&n, NATIVE_STATS_KEY_RIGHT);
    assert(n.tab == NATIVE_STATS_OVERVIEW);
    press(&n, NATIVE_STATS_KEY_DOWN); press(&n, NATIVE_STATS_KEY_DOWN);
    assert(n.focus == 1);
    press(&n, NATIVE_STATS_KEY_OK); assert(n.help);
}

int main(void) { audio_flow(); availability(); decoder_pending(); network_metrics(); ranges(); decoder_live_rates(); audio_recovery_preserves_history(); histories(); navigation(); }
