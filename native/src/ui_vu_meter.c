#ifdef LGNOME_TARGET_WEBOS

#include "ui_vu_meter.h"

#include <math.h>

#include "clog.h"

clog_define(g_native_log_vu_meter, cLogLevelInfo, "ui.mixer");

float native_ui_vu_meter_db_from_peak(int32_t peak) {
    return peak > 0 ? 20.0f * log10f((float)peak / 32768.0f) : NATIVE_MIXER_METER_FLOOR_DB;
}

static lv_obj_t *vu_box(lv_obj_t *parent, int x, int y, int w, int h) {
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_set_pos(box, x, y);
    lv_obj_set_size(box, w, h);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_border_width(box, 0, 0);
    lv_obj_set_style_pad_all(box, 0, 0);
    return box;
}

void native_ui_vu_meter_build(NativeUiVuMeter *meter, lv_obj_t *parent, const NativeUiVuMeterSpec *spec) {
    meter->spec = *spec;
    meter->level_db = NATIVE_MIXER_METER_FLOOR_DB;
    meter->level_px = 0;

    lv_obj_t *rail = vu_box(parent, spec->x, spec->y, spec->width, spec->height);
    lv_obj_set_style_bg_color(rail, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(rail, (lv_opa_t)15, 0);
    lv_obj_set_style_radius(rail, spec->radius, 0);

    meter->clip = vu_box(parent, spec->x, spec->y + spec->height, spec->width, 1);
    lv_obj_set_style_bg_opa(meter->clip, LV_OPA_TRANSP, 0);
    lv_obj_set_style_radius(meter->clip, spec->radius, 0);
    lv_obj_set_style_clip_corner(meter->clip, true, 0);
    lv_obj_add_flag(meter->clip, LV_OBJ_FLAG_HIDDEN);

    meter->grad = vu_box(meter->clip, 0, -spec->height, spec->width, spec->height);
    lv_obj_set_style_bg_color(meter->grad, lv_color_hex(0xf7d038), 0);
    lv_obj_set_style_bg_grad_color(meter->grad, lv_color_hex(0x27cf5a), 0);
    lv_obj_set_style_bg_grad_dir(meter->grad, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(meter->grad, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(meter->grad, spec->grad_radius, 0);
    lv_obj_set_style_clip_corner(meter->grad, true, 0);
}

static void vu_meter_paint(NativeUiVuMeter *meter, int px) {
    if (px == meter->level_px) {
        return;
    }
    meter->level_px = px;
    if (px == 0) {
        lv_obj_add_flag(meter->clip, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_clear_flag(meter->clip, LV_OBJ_FLAG_HIDDEN);
    /* The clip window's bottom stays pinned to the scale bottom; the gradient
     * child slides so its artwork stays anchored to the dB scale. */
    lv_obj_set_y(meter->clip, meter->spec.y + meter->spec.height - px);
    lv_obj_set_height(meter->clip, px);
    lv_obj_set_y(meter->grad, px - meter->spec.height);
}

float native_ui_vu_meter_update(NativeUiVuMeter *meter, int32_t peak, float fall_db) {
    float target_db = native_ui_vu_meter_db_from_peak(peak);
    float fallen_db = meter->level_db - fall_db;
    float level_db = target_db > fallen_db ? target_db : fallen_db;
    if (level_db < NATIVE_MIXER_METER_FLOOR_DB) {
        level_db = NATIVE_MIXER_METER_FLOOR_DB;
    }
    meter->level_db = level_db;

    int px = 0;
    if (level_db > (float)NATIVE_MIXER_FADER_MIN_DB) {
        float scaled = (level_db - (float)NATIVE_MIXER_FADER_MIN_DB) * (float)meter->spec.height /
                       (float)(NATIVE_MIXER_FADER_MAX_DB - NATIVE_MIXER_FADER_MIN_DB);
        px = (int)lroundf(scaled);
        if (px > meter->spec.height) {
            px = meter->spec.height;
        }
        if (px < meter->spec.min_px) {
            px = 0;
        }
    }
    vu_meter_paint(meter, px);
    return target_db;
}

void native_ui_vu_meter_invalidate(NativeUiVuMeter *meter) {
    meter->level_px = -1;
    meter->level_db = NATIVE_MIXER_METER_FLOOR_DB;
}

void native_ui_vu_meter_reset(NativeUiVuMeter *meter) {
    if (!meter->clip) {
        clog_once(cLogLevelWarning, "vu meter reset before build");
        return;
    }
    meter->level_db = NATIVE_MIXER_METER_FLOOR_DB;
    vu_meter_paint(meter, 0);
}

#endif
