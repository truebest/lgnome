#include "ui_preconnect_internal.h"
#include "ui_fonts.h"
#include "ui_slot_palette.h"
#include <stdio.h>
#include <string.h>

static const char *const statistics_tab_names[] = {"Overview", "Video", "Audio", "Network", "Performance", "Diagnostics"};
static const char *const video_categories[] = {"Delivery", "Decode", "Render", "Quality"};
static const char *const audio_categories[] = {"Continuity", "Transport", "Decode & sync"};
static const char *const network_categories[] = {"Connection", "RDP QoS", "TCP"};
/* Reference-only series have no producer or allocated timeline. */

static const NativeMetricId video_fields[4][6] = {
    {METRIC_MAIN_AUX, METRIC_VIDEO_LOSS, METRIC_VIDEO_LATE, METRIC_VIDEO_JITTER, METRIC_RETRANSMIT, METRIC_KEYFRAME_AGE},
    {METRIC_DECODED, METRIC_DECODE_TIME, METRIC_DECODE_QUEUE, METRIC_DECODE_ERRORS, METRIC_PREDECODE_DROP, METRIC_KEYFRAME_REQUESTS},
    {METRIC_PRESENTED, METRIC_PRESENT_INTERVAL, METRIC_LATE_DROP, METRIC_GRAPHICS_QUEUE, METRIC_GRAPHICS_AGE, METRIC_GRAPHICS_TIME},
    {METRIC_PRIMARY_INTERVAL, METRIC_PRIMARY_GAPS, METRIC_STALL, METRIC_RECEIVE_PRESENT, METRIC_AV, METRIC_FIRST_FRAME}
};
static const NativeMetricId audio_fields[3][6] = {
    {METRIC_UNDERRUNS, METRIC_TRIMS, METRIC_DROPPED, METRIC_PLAYOUT_GAPS, METRIC_PLC, METRIC_FEC},
    {METRIC_AUDIO_BITRATE, METRIC_AUDIO_PACKETS, METRIC_AUDIO_LOSS, METRIC_AUDIO_LATE, METRIC_AUDIO_REORDERED, METRIC_AUDIO_PACKET_AGE},
    {METRIC_AUDIO_DECODE_TIME, METRIC_AUDIO_DECODE_ERRORS, METRIC_PCM_INTERVAL, METRIC_PCM_FEED, METRIC_DEVICE_QUEUE, METRIC_AV}
};
static const NativeMetricId network_fields[3][6] = {
    {METRIC_NET_STATE, METRIC_NET_UPTIME, METRIC_NET_TRANSPORT, METRIC_NET_RX_AGE, METRIC_NET_RX_BYTES, METRIC_NET_TX_BYTES},
    {METRIC_NET_RTT, METRIC_NET_BASE_RTT, METRIC_NET_BANDWIDTH, METRIC_NET_REPORT_AGE, METRIC_NET_REPORTS, METRIC_NET_QOS_STATE},
    {METRIC_NET_SRTT, METRIC_NET_RTTVAR, METRIC_NET_RTO, METRIC_NET_UNACKED, METRIC_NET_CWND, METRIC_NET_RETRANSMITS}
};
static const NativeMetricId card_ids[NATIVE_STATS_TABS][4] = {
    [NATIVE_STATS_OVERVIEW] = {METRIC_AUS, METRIC_MBPS, METRIC_RDP, METRIC_BUFFER},
    [NATIVE_STATS_VIDEO] = {METRIC_AUS, METRIC_MBPS, METRIC_DECODED, METRIC_AU_AGE},
    [NATIVE_STATS_AUDIO] = {METRIC_BUFFER, METRIC_AUDIO_AGE, METRIC_JITTER, METRIC_SRC},
    [NATIVE_STATS_NETWORK] = {METRIC_NET_RX, METRIC_NET_TX, METRIC_NET_RTT, METRIC_NET_BANDWIDTH},
    [NATIVE_STATS_PERFORMANCE] = {METRIC_CPU, METRIC_RSS, METRIC_RDP, METRIC_NDL}
};

static lv_obj_t *statistics_box(lv_obj_t *parent, int x, int y, int width, int height, bool filled) {
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, x, y); lv_obj_set_size(o, width, height);
    lv_obj_set_style_bg_color(o, lv_color_hex(STATISTICS_PANEL), 0);
    lv_obj_set_style_bg_opa(o, filled ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_grad_color(o, lv_color_hex(STATISTICS_PANEL_END), 0);
    lv_obj_set_style_bg_grad_dir(o, filled ? LV_GRAD_DIR_VER : LV_GRAD_DIR_NONE, 0);
    lv_obj_set_style_radius(o, 18, 0);
    lv_obj_set_style_border_color(o, lv_color_white(), 0);
    lv_obj_set_style_border_opa(o, LV_OPA_10, 0);
    lv_obj_set_style_border_width(o, filled ? 1 : 0, 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

static lv_obj_t *statistics_label(lv_obj_t *parent, int x, int y, int width, const char *text,
                                   const lv_font_t *font, uint32_t color) {
    lv_obj_t *o = lv_label_create(parent);
    lv_obj_set_pos(o, x, y); lv_obj_set_width(o, width);
    lv_obj_set_style_text_font(o, font, 0);
    lv_obj_set_style_text_color(o, lv_color_hex(color), 0);
    lv_label_set_long_mode(o, LV_LABEL_LONG_DOT);
    lv_obj_set_height(o, font->line_height);
    lv_label_set_text(o, text);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE);
    return o;
}

static void statistics_text(lv_obj_t *o, const char *text) {
    if (o && strcmp(lv_label_get_text(o), text)) lv_label_set_text(o, text);
}

static void statistics_field_build(NativeStatisticsField *f, lv_obj_t *parent, NativeMetricId id,
                                    int x, int y, int width, bool card) {
    lv_obj_t *box = statistics_box(parent, x, y, width, card ? 80 : 40, card);
    f->metric = id;
    f->label = statistics_label(box, 10, card ? 8 : 1, width - 20, "", &lv_font_ibm_plex_mono_semibold_12, STATISTICS_MUTED);
    f->value = statistics_label(box, 10, card ? 28 : 20, width - 20, "", card ? &lv_font_ibm_plex_mono_regular_22 : &lv_font_ibm_plex_mono_semibold_12, STATISTICS_TEXT);
    if (card) f->unit = statistics_label(box, 10, 60, width - 20, "", &lv_font_ibm_plex_mono_semibold_12, STATISTICS_MUTED);
    else {
        lv_obj_set_style_border_width(box, 1, 0);
        lv_obj_set_style_border_side(box, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_radius(box, 0, 0);
    }
}

static void statistics_field_update(NativeStatisticsField *f, const NativeDiagnosticsSnapshot *s, NativeMetricId id) {
    f->metric = id;
    const NativeMetricDescription *m = native_statistics_metric(id);
    NativeMetricValue v = native_statistics_value(s, id);
    statistics_text(f->label, m->label);
    char text[112];
    snprintf(text, sizeof(text), "%s%s%s", v.text, !f->unit && v.state == NATIVE_METRIC_VALID && *m->unit ? " " : "",
        !f->unit && v.state == NATIVE_METRIC_VALID ? m->unit : "");
    statistics_text(f->value, text);
    if (f->unit) {
        const lv_font_t *font = v.state == NATIVE_METRIC_VALID ? &lv_font_ibm_plex_mono_regular_22 : &lv_font_ibm_plex_sans_semibold_16;
        lv_obj_set_style_text_font(f->value, font, 0);
        statistics_text(f->unit, m->unit);
    }
    lv_obj_set_style_text_color(f->value, lv_color_hex(v.state == NATIVE_METRIC_VALID ? STATISTICS_TEXT : STATISTICS_MUTED), 0);
}

static void statistics_chart_tick(lv_event_t *event) {
    NativeStatisticsChart *c = lv_event_get_user_data(event);
    lv_obj_draw_part_dsc_t *d = lv_event_get_draw_part_dsc(event);
    if (!d || d->type != LV_CHART_DRAW_PART_TICK_LABEL || !d->text) return;
    if (d->id == LV_CHART_AXIS_PRIMARY_X) {
        if ((unsigned)d->value == NATIVE_DIAGNOSTICS_HISTORY_MS / 100u) lv_snprintf(d->text, d->text_length, "now");
        else lv_snprintf(d->text, d->text_length, "-%us", NATIVE_DIAGNOSTICS_HISTORY_MS / 1000u - d->value / 10);
    } else {
        unsigned axis = d->id == LV_CHART_AXIS_SECONDARY_Y;
        lv_snprintf(d->text, d->text_length, "%.0f", d->value * c->maximum[axis] / 1000.0);
        d->label_dsc->color = lv_color_hex(axis ? STATISTICS_AMBER : STATISTICS_CYAN);
    }
}

static void statistics_chart_build(NativeStatisticsChart *c, lv_obj_t *parent, int x, int y, int width,
                                    const char *title, int first, int second, int third) {
    lv_obj_t *box = statistics_box(parent, x, y, width, 176, true);
    statistics_label(box, 12, 8, width - 24, title, &lv_font_ibm_plex_sans_semibold_16, STATISTICS_TEXT);
    lv_obj_t *chart = c->object = lv_chart_create(box);
    lv_obj_set_pos(chart, 50, 54); lv_obj_set_size(chart, width - 100, 92);
    lv_obj_set_style_bg_opa(chart, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(chart, 0, 0);
    /* LVGL scatter places zero at the content's lower edge; keep the stroke inside the clip. */
    lv_obj_set_style_pad_all(chart, 2, 0);
    lv_obj_set_style_line_color(chart, lv_color_hex(STATISTICS_LINE), 0);
    lv_obj_set_style_line_width(chart, 1, 0);
    lv_obj_set_style_line_width(chart, 2, LV_PART_ITEMS);
    lv_obj_set_style_size(chart, first == NATIVE_STATS_DECODED ? 3 : 0, LV_PART_INDICATOR);
    lv_obj_set_style_text_font(chart, &lv_font_ibm_plex_mono_semibold_12, LV_PART_TICKS);
    lv_obj_set_style_text_color(chart, lv_color_hex(STATISTICS_MUTED), LV_PART_TICKS);
    lv_obj_clear_flag(chart, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_chart_set_type(chart, LV_CHART_TYPE_SCATTER);
    lv_chart_set_point_count(chart, STATISTICS_CHART_POINTS);
    lv_chart_set_div_line_count(chart, 3, 3);
    lv_chart_set_axis_tick(chart, LV_CHART_AXIS_PRIMARY_X, 2, 0, 3, 1, true, 24);
    lv_chart_set_axis_tick(chart, LV_CHART_AXIS_PRIMARY_Y, 2, 0, 3, 1, true, 45);
    if (first == NATIVE_STATS_AUS) lv_chart_set_axis_tick(chart, LV_CHART_AXIS_SECONDARY_Y, 2, 0, 3, 1, true, 45);
    c->ids[0] = first; c->ids[1] = second; c->ids[2] = third;
    c->count = third >= 0 ? 3 : second >= 0 ? 2 : 1;
    const uint32_t colors[] = {STATISTICS_CYAN, STATISTICS_AMBER, STATISTICS_PURPLE};
    static const char *const labels[] = {
        [NATIVE_STATS_AUS] = "Input [AU/s]",
        [NATIVE_STATS_MBPS] = "Bitrate [Mbit/s]",
        [NATIVE_STATS_DECODED] = "Primary [frames/s]",
        [NATIVE_STATS_PCM] = "PCM input [ms audio/s]",
        [NATIVE_STATS_SRC] = "SRC output [ms audio/s]",
        [NATIVE_STATS_BUFFER] = "Buffer [ms]",
        [NATIVE_STATS_TARGET] = "Target [ms]",
        [NATIVE_STATS_AGE] = "Age: N/A",
        [NATIVE_STATS_NET_RX] = "Receive [Mbit/s]",
        [NATIVE_STATS_NET_TX] = "Transmit [Mbit/s]",
        [NATIVE_STATS_NET_RTT] = "RDP avg RTT [ms]",
        [NATIVE_STATS_NET_SRTT] = "TCP SRTT [ms]"
    };
    const lv_font_t *font = &lv_font_ibm_plex_mono_semibold_12;
    int legend_x = 12;
    for (unsigned i = 0; i < c->count; i++) {
        lv_color_t color = lv_color_hex(colors[i]);
        c->series[i] = lv_chart_add_series(chart, color,
            first == NATIVE_STATS_AUS && i == 1 ? LV_CHART_AXIS_SECONDARY_Y : LV_CHART_AXIS_PRIMARY_Y);
        lv_obj_t *dot = statistics_box(box, legend_x, 30 + (font->line_height - 8) / 2, 8, 8, true);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(dot, 0, 0);
        lv_obj_set_style_bg_color(dot, color, 0);
        lv_obj_set_style_bg_grad_dir(dot, LV_GRAD_DIR_NONE, 0);
        const char *label = labels[c->ids[i]];
        int label_width = lv_txt_get_width(label, strlen(label), font, 0, LV_TEXT_FLAG_NONE);
        statistics_label(box, legend_x + 14, 30, label_width, label, font, STATISTICS_MUTED);
        legend_x += label_width + 28;
    }
    if (first == NATIVE_STATS_DECODED)
        statistics_label(box, legend_x, 30, width - legend_x - 12, "Presented: Not measured", font, STATISTICS_MUTED);
    lv_obj_add_event_cb(chart, statistics_chart_tick, LV_EVENT_DRAW_PART_BEGIN, c);
}

static void statistics_chart_update(NativeStatisticsChart *c, const NativeDiagnosticsSnapshot *s) {
    if (!c->object) return;
    c->maximum[0] = c->ids[0] == NATIVE_STATS_PCM ? 1200 : c->ids[0] == NATIVE_STATS_BUFFER ? 100 : 60;
    if (c->ids[0] == NATIVE_STATS_NET_RX) c->maximum[0] = 10;
    if (c->ids[0] == NATIVE_STATS_NET_RTT) c->maximum[0] = 10;
    c->maximum[1] = 10;
    for (unsigned j = 0; s->timeline && j < c->count; j++) {
        if (c->ids[j] >= NATIVE_STATS_SERIES) continue;
        const NativeStatisticsSeries *series = &s->timeline->series[c->ids[j]];
        unsigned axis = c->ids[j] == NATIVE_STATS_MBPS;
        for (unsigned i = 0; i < series->count; i++) {
            const NativeStatisticsPoint *p = &series->points[i];
            if (p->valid && native_statistics_x(s->at_ms, p->at_ms) >= 0)
                while (p->value > c->maximum[axis]) c->maximum[axis] *= 2;
        }
    }
    lv_chart_set_range(c->object, LV_CHART_AXIS_PRIMARY_X, 0, NATIVE_DIAGNOSTICS_HISTORY_MS / 100u);
    lv_chart_set_range(c->object, LV_CHART_AXIS_PRIMARY_Y, 0, 1000);
    lv_chart_set_range(c->object, LV_CHART_AXIS_SECONDARY_Y, 0, 1000);
    for (unsigned j = 0; j < c->count; j++) {
        lv_coord_t *x = lv_chart_get_x_array(c->object, c->series[j]);
        lv_coord_t *y = lv_chart_get_y_array(c->object, c->series[j]);
        for (unsigned i = 0; i < STATISTICS_CHART_POINTS; i++) { x[i] = 0; y[i] = LV_CHART_POINT_NONE; }
        unsigned at = 0;
        const NativeStatisticsSeries *series = s->timeline && c->ids[j] < NATIVE_STATS_SERIES ? &s->timeline->series[c->ids[j]] : NULL;
        uint32_t gap = 2 * NATIVE_DIAGNOSTICS_INTERVAL_MS;
        for (unsigned i = 0; series && i < series->count; i++) {
            const NativeStatisticsPoint *p = &series->points[i];
            int pos = native_statistics_x(s->at_ms, p->at_ms);
            if (pos < 0) continue;
            if (at && p->at_ms - series->points[i - 1].at_ms > gap) at++;
            x[at] = pos;
            if (p->valid) y[at] = (lv_coord_t)(p->value * 1000.0 / c->maximum[c->ids[j] == NATIVE_STATS_MBPS] + 0.5);
            at++;
        }
    }
    lv_chart_refresh(c->object);
}

void ui_statistics_tabs_build(NativePreconnectUi *ui) {
    lv_obj_t *tabs = ui->statistics_tabs = lv_tabview_create(ui->statistics_panel, LV_DIR_TOP, 36);
    lv_obj_set_pos(tabs, 16, 72); lv_obj_set_size(tabs, 928, 570);
    lv_obj_set_style_bg_opa(tabs, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(tabs, 0, 0); lv_obj_set_style_pad_all(tabs, 0, 0);
    lv_obj_t *buttons = lv_tabview_get_tab_btns(tabs);
    lv_obj_set_style_text_font(buttons, &lv_font_ibm_plex_sans_semibold_16, 0);
    lv_obj_set_style_text_color(buttons, lv_color_hex(STATISTICS_MUTED), LV_PART_ITEMS);
    lv_obj_set_style_border_color(buttons, lv_color_white(), 0);
    lv_obj_set_style_border_opa(buttons, LV_OPA_10, 0);
    lv_obj_set_style_radius(buttons, 16, LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(buttons, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_color(buttons, lv_color_white(), LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(buttons, LV_OPA_10, LV_PART_ITEMS);
    lv_obj_set_style_border_width(buttons, 0, LV_PART_ITEMS);
    lv_obj_set_style_text_color(buttons, lv_color_hex(STATISTICS_SELECTED_TEXT), LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_border_opa(buttons, LV_OPA_TRANSP, LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(buttons, lv_color_hex(STATISTICS_SELECTED), LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_bg_opa(buttons, LV_OPA_COVER, LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_group_remove_obj(buttons);
    lv_obj_clear_flag(buttons, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    for (unsigned t = 0; t < NATIVE_STATS_TABS; t++) {
        NativeStatisticsPage *p = &ui->statistics_pages[t];
        lv_obj_t *page = p->object = lv_tabview_add_tab(tabs, statistics_tab_names[t]);
        lv_obj_remove_style_all(page);
        lv_obj_set_size(page, lv_pct(100), lv_pct(100));
        lv_obj_clear_flag(page, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        statistics_label(page, 4, 12, 400, t == 0 ? "Session at a glance" : statistics_tab_names[t], &lv_font_ibm_plex_sans_semibold_16, STATISTICS_TEXT);
        if (t < NATIVE_STATS_DIAGNOSTICS) for (int i = 0; i < 4; i++) statistics_field_build(&p->cards[i], page, card_ids[t][i], i * 235, 42, 223, true);
        if (t == NATIVE_STATS_OVERVIEW || t == NATIVE_STATS_VIDEO) {
            statistics_chart_build(&p->charts[0], page, 0, 134, t == 0 ? 600 : 457, "Video input", NATIVE_STATS_AUS, NATIVE_STATS_MBPS, -1);
            if (t == NATIVE_STATS_VIDEO) statistics_chart_build(&p->charts[1], page, 471, 134, 457, "Decoder output", NATIVE_STATS_DECODED, -1, -1);
        }
        if (t == NATIVE_STATS_AUDIO) {
            statistics_chart_build(&p->charts[0], page, 0, 134, 457, "Audio flow", NATIVE_STATS_PCM, NATIVE_STATS_SRC, -1);
            statistics_chart_build(&p->charts[1], page, 471, 134, 457, "Buffer & queue", NATIVE_STATS_BUFFER, NATIVE_STATS_AGE, NATIVE_STATS_TARGET);
            for (int i = 0; i < 4; i++) p->sessions[i] = statistics_label(page, 10, 317 + i * 18, 906, "", &lv_font_ibm_plex_mono_semibold_12, native_ui_slot_rgb(i));
        }
        if (t == NATIVE_STATS_NETWORK) {
            statistics_chart_build(&p->charts[0], page, 0, 134, 457, "Connection throughput", NATIVE_STATS_NET_RX, NATIVE_STATS_NET_TX, -1);
            statistics_chart_build(&p->charts[1], page, 471, 134, 457, "Round-trip time", NATIVE_STATS_NET_RTT, NATIVE_STATS_NET_SRTT, -1);
            statistics_label(page, 10, 330, 230, "Connection metrics", &lv_font_ibm_plex_sans_semibold_16, STATISTICS_TEXT);
            p->note = statistics_label(page, 10, 476, 906,
                "TCP payload incl. TLS; excludes IP/TCP headers and retransmitted bytes.\nRX: server to TV. TX: TV to server. 5 s average; samples every 500 ms.\nRDP: server reports. TCP: TV sender. Bandwidth estimate is not interface speed.",
                &lv_font_ibm_plex_mono_semibold_12, STATISTICS_MUTED);
            lv_obj_set_height(p->note, 48); lv_label_set_long_mode(p->note, LV_LABEL_LONG_WRAP);
        }
        if (t == NATIVE_STATS_VIDEO || t == NATIVE_STATS_AUDIO || t == NATIVE_STATS_NETWORK) {
            int y = t == NATIVE_STATS_AUDIO ? 393 : 330;
            unsigned count = t == NATIVE_STATS_VIDEO ? 4 : 3;
            for (unsigned i = 0; i < count; i++) {
                p->categories[i] = statistics_label(page, (t == NATIVE_STATS_NETWORK ? 260 : 10) + i * 220, y, 210,
                    t == NATIVE_STATS_VIDEO ? video_categories[i] : t == NATIVE_STATS_AUDIO ? audio_categories[i] : network_categories[i],
                    &lv_font_ibm_plex_sans_semibold_16, STATISTICS_MUTED);
            }
            p->field_count = 6;
            for (int i = 0; i < 6; i++) statistics_field_build(&p->fields[i], page, METRIC_AUS, (i / 3) * 471, y + 25 + (i % 3) * 38, 457, false);
        } else if (t == NATIVE_STATS_OVERVIEW) {
            const NativeMetricId ids[] = {METRIC_CPU, METRIC_RSS, METRIC_NDL, METRIC_AUDIO_AGE, METRIC_UNDERRUNS, METRIC_DISCONTINUITIES};
            p->field_count = 6;
            for (int i = 0; i < 6; i++) statistics_field_build(&p->fields[i], page, ids[i], i < 3 ? i * 309 : 615,
                i < 3 ? 346 : 140 + (i - 3) * 52, i < 3 ? 297 : 313, false);
            p->note = statistics_label(page, 12, 415, 902,
                "Audio corrections: source lifetime. An old underrun does not mean a current fault.\n\nVideo input may pause on an idle desktop. Decoded output is not screen FPS.\nPCM age and RDP timing are not measured on this playback path.",
                &lv_font_ibm_plex_mono_semibold_12, STATISTICS_MUTED);
            lv_obj_set_height(p->note, 100); lv_label_set_long_mode(p->note, LV_LABEL_LONG_WRAP);
        } else if (t == NATIVE_STATS_PERFORMANCE) {
            statistics_label(page, 10, 145, 440, "TV CPU - all processes", &lv_font_ibm_plex_sans_semibold_16, STATISTICS_TEXT);
            for (int i = 0; i < 8; i++) {
                p->core_labels[i] = statistics_label(page, 10, 180 + i * 38, 425, "", &lv_font_ibm_plex_mono_semibold_12, STATISTICS_MUTED);
                lv_obj_t *bar = p->cores[i] = lv_bar_create(page);
                lv_obj_set_pos(bar, 10, 198 + i * 38); lv_obj_set_size(bar, 425, 5);
                lv_obj_set_style_bg_color(bar, lv_color_hex(STATISTICS_LINE), 0);
                lv_obj_set_style_bg_color(bar, lv_color_hex(STATISTICS_CYAN), LV_PART_INDICATOR);
                lv_obj_clear_flag(bar, LV_OBJ_FLAG_CLICKABLE);
            }
            const NativeMetricId ids[] = {METRIC_GRAPHICS_QUEUE, METRIC_GRAPHICS_MEMORY, METRIC_ALL_MEMORY, METRIC_GRAPHICS_AGE, METRIC_GRAPHICS_TIME, METRIC_GRAPHICS_WAIT, METRIC_NDL_MAX, METRIC_PENDING};
            p->field_count = 8;
            for (int i = 0; i < 8; i++) statistics_field_build(&p->fields[i], page, ids[i], 471, 150 + i * 44, 457, false);
        } else {
            const NativeMetricId ids[] = {METRIC_NDL_QUEUE, METRIC_NDL_ERRORS, METRIC_NDL_MAX, METRIC_PENDING, METRIC_REPLAY, METRIC_ACCEPTED,
                METRIC_PCM_INTERVAL, METRIC_PCM_FEED, METRIC_DISCONTINUITIES, METRIC_PRIMARY_GAPS, METRIC_PRESENTED, METRIC_AV};
            p->field_count = 12;
            for (int i = 0; i < 12; i++) statistics_field_build(&p->fields[i], page, ids[i], (i / 6) * 471, 52 + (i % 6) * 48, 457, false);
            p->note = statistics_label(page, 10, 365, 906,
                "Valid: producer has a usable value. Collecting: waiting for its first window.\nStale: last report > 7.5 s old. Unavailable: source is absent or closed.\nNot measured: no instrument for this field on this path.\n\nDecoder timing: published ~5 s windows. Jitter: current controller estimate.\nAudio corrections: source lifetime. Decoder gaps: decoder-load lifetime, includes idle.\nPending is the live decoder queue; graphics queue/timing are not measured.\n\np95: 95th percentile. EWMA: exponentially weighted moving average.",
                &lv_font_ibm_plex_mono_semibold_12, STATISTICS_MUTED);
            lv_obj_set_height(p->note, 162); lv_label_set_long_mode(p->note, LV_LABEL_LONG_WRAP);
        }
    }
    lv_obj_t *content = lv_tabview_get_content(tabs);
    lv_obj_set_style_pad_all(content, 0, 0); lv_obj_set_style_border_width(content, 0, 0);
    lv_obj_set_style_bg_opa(content, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(content, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(tabs, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
}

void ui_statistics_build(NativePreconnectUi *ui) {
    ui->statistics_enabled = false;
    lv_label_set_text(lv_obj_get_child(ui->statistics_btn, 0), "Stats: Off");
    ui->statistics_panel = statistics_box(lv_scr_act(), 24, 24, 960, 680, true);
    lv_obj_set_style_bg_color(ui->statistics_panel, lv_color_hex(STATISTICS_BG), 0);
    lv_obj_set_style_bg_grad_dir(ui->statistics_panel, LV_GRAD_DIR_NONE, 0);
    lv_obj_set_style_radius(ui->statistics_panel, 12, 0);
    ui->statistics_name = statistics_label(ui->statistics_panel, 20, 14, 660, "", &lv_font_ibm_plex_sans_semibold_16, STATISTICS_TEXT);
    ui->statistics_format = statistics_label(ui->statistics_panel, 20, 40, 600, "", &lv_font_ibm_plex_mono_semibold_12, STATISTICS_MUTED);
    ui->statistics_status = statistics_label(ui->statistics_panel, 685, 14, 255, "", &lv_font_ibm_plex_sans_semibold_16, STATISTICS_TEXT);
    ui->statistics_activity = statistics_label(ui->statistics_panel, 625, 40, 315, "", &lv_font_ibm_plex_mono_semibold_12, STATISTICS_MUTED);
    ui_statistics_tabs_build(ui);
    ui->statistics_hint = statistics_label(ui->statistics_panel, 20, 655, 920, "", &lv_font_ibm_plex_mono_semibold_12, STATISTICS_MUTED);
    ui->statistics_help_panel = statistics_box(ui->statistics_panel, 36, 130, 888, 490, true);
    lv_obj_set_style_bg_color(ui->statistics_help_panel, lv_color_hex(STATISTICS_HELP), 0);
    lv_obj_set_style_bg_grad_dir(ui->statistics_help_panel, LV_GRAD_DIR_NONE, 0);
    lv_obj_set_style_border_opa(ui->statistics_help_panel, LV_OPA_20, 0);
    lv_obj_set_style_radius(ui->statistics_help_panel, 12, 0);
    statistics_label(ui->statistics_help_panel, 18, 16, 850, "Metric notes        Left / Right: page        OK / Back: close", &lv_font_ibm_plex_sans_semibold_16, STATISTICS_TEXT);
    ui->statistics_help_text = statistics_label(ui->statistics_help_panel, 18, 50, 850, "", &lv_font_ibm_plex_mono_semibold_12, STATISTICS_TEXT);
    lv_label_set_long_mode(ui->statistics_help_text, LV_LABEL_LONG_WRAP);
    lv_obj_set_height(ui->statistics_help_text, 420);
    lv_obj_add_flag(ui->statistics_help_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ui->statistics_panel, LV_OBJ_FLAG_HIDDEN);
    native_statistics_focus_reset(&ui->statistics_navigation);
}

static void statistics_focus(lv_obj_t *o, bool focused) {
    if (!o) return;
    lv_obj_set_style_outline_width(o, focused ? 1 : 0, 0);
    lv_obj_set_style_outline_pad(o, 3, 0);
    lv_obj_set_style_outline_color(o, lv_color_hex(STATISTICS_FOCUS), 0);
}

void ui_statistics_update(NativePreconnectUi *ui, const NativeDiagnosticsSnapshot *s) {
    NativeStatisticsNavigation *n = &ui->statistics_navigation;
    NativeStatisticsPage *p = &ui->statistics_pages[n->tab];
    char text[512];
    snprintf(text, sizeof(text), "%s  %s", native_session_slot_name(s->slot), s->name);
    statistics_text(ui->statistics_name, text);
    lv_obj_set_style_text_color(ui->statistics_name, lv_color_hex(native_ui_slot_rgb(s->slot)), 0);
    snprintf(text, sizeof(text), "%ux%u / %s%s", s->width, s->height, s->bitmap ? "Bitmap / RGBA" : s->codec == NATIVE_VIDEO_CODEC_H265 ? "H.265 / NDL" : "H.264 / NDL", s->counters.auxiliary ? " / AVC444 aux" : "");
    statistics_text(ui->statistics_format, text);
    statistics_text(ui->statistics_status, s->connected ? "Connected" : s->status ? s->status : "Disconnected");
    if (s->bitmap) snprintf(text, sizeof(text), "Video input: RGBA (AU not measured)");
    else if (!s->counters.has_au) snprintf(text, sizeof(text), "Video input: waiting for first AU");
    else snprintf(text, sizeof(text), "Video: %s / last AU %.1f s ago", s->at_ms - s->counters.last_au_ms > 1500 ? "idle" : "receiving", (s->at_ms - s->counters.last_au_ms) / 1000.0);
    statistics_text(ui->statistics_activity, text);
    if (n->tab < NATIVE_STATS_DIAGNOSTICS) for (int i = 0; i < 4; i++) statistics_field_update(&p->cards[i], s, card_ids[n->tab][i]);
    const NativeMetricId *selected = n->tab == NATIVE_STATS_VIDEO ? video_fields[n->video_category] :
        n->tab == NATIVE_STATS_AUDIO ? audio_fields[n->audio_category] : n->tab == NATIVE_STATS_NETWORK ? network_fields[n->network_category] : NULL;
    for (unsigned i = 0; i < p->field_count; i++) statistics_field_update(&p->fields[i], s, selected ? selected[i] : p->fields[i].metric);
    for (int i = 0; i < 2; i++) statistics_chart_update(&p->charts[i], s);
    if (n->tab == NATIVE_STATS_AUDIO) for (int i = 0; i < 4; i++) {
        const NativeDiagnosticsAudio *a = &s->audio[i];
        if (!a->connected) snprintf(text, sizeof(text), "%s%s  Off / disconnected", i == s->slot ? "> " : "  ", native_session_slot_name(i));
        else if (!a->source.open) snprintf(text, sizeof(text), "%s%s  Connected / waiting for audio", i == s->slot ? "> " : "  ", native_session_slot_name(i));
        else {
            NativeDiagnosticsSnapshot source = *s;
            source.slot = i;
            NativeMetricValue jitter = native_statistics_value(&source, METRIC_JITTER);
            snprintf(text, sizeof(text), "%s%s  %s %u Hz/%uch  buffer %u/%u ms  jitter %s%s  SRC %+d ppm", i == s->slot ? "> " : "  ", native_session_slot_name(i), a->codec == RDP_AUDIO_CODEC_OPUS ? "Opus" : "PCM", a->sample_rate, a->channels, a->source.queue_ms, a->source.target_delay_ms, jitter.text, jitter.state == NATIVE_METRIC_VALID ? " ms" : "", a->source.src_correction_ppm);
        }
        statistics_text(p->sessions[i], text);
    }
    if (n->tab == NATIVE_STATS_PERFORMANCE) for (int i = 0; i < 8; i++) {
        if (!s->cores[i].present) {
            lv_obj_add_flag(p->core_labels[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(p->cores[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_clear_flag(p->core_labels[i], LV_OBJ_FLAG_HIDDEN);
        if (s->cores[i].valid) snprintf(text, sizeof(text), "CPU %d                                  %.0f%%", i, s->cores[i].percent);
        else snprintf(text, sizeof(text), "CPU %d  Collecting", i);
        statistics_text(p->core_labels[i], text);
        lv_bar_set_value(p->cores[i], s->cores[i].valid ? (int)s->cores[i].percent : 0, LV_ANIM_OFF);
        if (s->cores[i].valid) lv_obj_clear_flag(p->cores[i], LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(p->cores[i], LV_OBJ_FLAG_HIDDEN);
    }
    unsigned category = n->tab == NATIVE_STATS_VIDEO ? n->video_category : n->tab == NATIVE_STATS_AUDIO ? n->audio_category : n->network_category;
    for (int i = 0; i < 4; i++) if (p->categories[i]) {
        lv_obj_set_style_text_color(p->categories[i], lv_color_hex((unsigned)i == category ? STATISTICS_TEXT : STATISTICS_MUTED), 0);
        statistics_focus(p->categories[i], n->focus == 1 && (unsigned)i == category);
    }
    unsigned last = 1u + (selected != NULL);
    statistics_focus(lv_tabview_get_tab_btns(ui->statistics_tabs), n->focus == 0);
    statistics_focus(ui->statistics_hint, n->focus == last && !n->help);
    const char *hint = !n->focus ? "Left / Right: tabs   Down: controls   OK: HUB" : n->focus == last ? "OK: metric notes   Up: controls   Back: tabs" : "Left / Right or OK: change   Up / Down: controls   Back: tabs";
    statistics_text(ui->statistics_hint, n->help ? "Left / Right: notes page   OK / Back: close metric notes" : hint);
    if (n->help) {
        char notes[4096];
        size_t used = (size_t)snprintf(notes, sizeof(notes), "Page %u / 2. Connection and input activity are independent; an idle desktop is normal.\nPCM age and graphics queue/timing are not measured on this path.\nPresented FPS, A/V offset and physical latency are not measured.\n\n", n->help_page + 1);
        unsigned cards = n->tab < NATIVE_STATS_DIAGNOSTICS ? 4 : 0;
        unsigned count = cards + p->field_count;
        for (unsigned i = n->help_page * 6; i < count && i < (n->help_page + 1) * 6 && used < sizeof(notes); i++) {
            const NativeMetricDescription *m = native_statistics_metric(i < cards ? p->cards[i].metric : p->fields[i - cards].metric);
            NativeMetricValue v = native_statistics_value(s, m->id);
            used += (size_t)snprintf(notes + used, sizeof(notes) - used, "%s: %s\n%s / %s. %s\n\n", m->label,
                native_statistics_state_name(v.state), m->source, m->scope, v.reason);
        }
        statistics_text(ui->statistics_help_text, notes);
        lv_obj_clear_flag(ui->statistics_help_panel, LV_OBJ_FLAG_HIDDEN);
    } else lv_obj_add_flag(ui->statistics_help_panel, LV_OBJ_FLAG_HIDDEN);
}
