#ifndef LGNOME_UI_STATISTICS_INTERNAL_H
#define LGNOME_UI_STATISTICS_INTERNAL_H
#include "native_statistics.h"
#include "lvgl.h"

#define STATISTICS_CHART_POINTS (2 * NATIVE_DIAGNOSTICS_HISTORY_POINTS)
#define STATISTICS_BG 0x101319
#define STATISTICS_PANEL 0x11151c
#define STATISTICS_PANEL_END 0x080a0e
#define STATISTICS_HELP 0x171b23
#define STATISTICS_SELECTED 0xf2f5fb
#define STATISTICS_SELECTED_TEXT 0x0b0d11
#define STATISTICS_FOCUS 0xeef3fd
#define STATISTICS_LINE 0x2b2e34
#define STATISTICS_TEXT 0xeef1f7
#define STATISTICS_MUTED 0x92969f
#define STATISTICS_CYAN 0x70d9df
#define STATISTICS_AMBER 0xe7bd77
#define STATISTICS_PURPLE 0xb3a0e5

typedef struct NativeStatisticsField {
    lv_obj_t *label, *value, *unit;
    NativeMetricId metric;
} NativeStatisticsField;

typedef struct NativeStatisticsChart {
    lv_obj_t *object;
    lv_chart_series_t *series[3];
    int ids[3];
    unsigned count;
    double maximum[2];
    const char *units[2];
} NativeStatisticsChart;

typedef struct NativeStatisticsPage {
    lv_obj_t *object, *categories[4];
    NativeStatisticsField cards[4], fields[12];
    unsigned field_count;
    NativeStatisticsChart charts[2];
    lv_obj_t *sessions[4], *cores[8], *core_labels[8], *note;
} NativeStatisticsPage;
#endif
