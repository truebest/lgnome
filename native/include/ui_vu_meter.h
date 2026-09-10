#ifndef LGNOME_UI_VU_METER_H
#define LGNOME_UI_VU_METER_H

/* The shared vertical VU bar used by the HUB card meters and the mixer
 * overlay: a dark rail, a clip window pinned to the scale bottom, and a
 * yellow-to-green gradient child that slides so its full-scale artwork stays
 * anchored to the dB scale. Ballistics: instant attack to the chunk peak,
 * NATIVE_MIXER_METER_DECAY_DB_S release, floored at the meter floor, mapped
 * over the fader dB range. Peak-hold/clip marks are the mixer's own layer on
 * top. Preconnect-UI builds only. */
#ifdef LGNOME_TARGET_WEBOS

#include <stdint.h>

#include "lvgl.h"

#include "ui_mixer.h"

typedef struct NativeUiVuMeterSpec {
    int x;      /* rail position within the parent */
    int y;
    int width;
    int height; /* bar travel; the baseline is y + height */
    int radius; /* rail/clip corner radius */
    int grad_radius;
    int min_px; /* hide sub-radius slivers below this height */
} NativeUiVuMeterSpec;

typedef struct NativeUiVuMeter {
    NativeUiVuMeterSpec spec;
    lv_obj_t *clip;
    lv_obj_t *grad;
    float level_db;
    int level_px;
} NativeUiVuMeter;

/* dBFS of one pre-saturation chunk peak (0 for silence reads as the floor). */
float native_ui_vu_meter_db_from_peak(int32_t peak);

void native_ui_vu_meter_build(NativeUiVuMeter *meter, lv_obj_t *parent, const NativeUiVuMeterSpec *spec);

/* One frame: decays by fall_db, attacks to `peak`, repaints only on a pixel
 * change. Returns the instant target level in dB for peak-hold layers. */
float native_ui_vu_meter_update(NativeUiVuMeter *meter, int32_t peak, float fall_db);

/* Forces the next update to repaint even at an unchanged level (the LVGL
 * objects may be stale after a screen swap). */
void native_ui_vu_meter_invalidate(NativeUiVuMeter *meter);

/* Drops the bar to the floor immediately (stream closed). */
void native_ui_vu_meter_reset(NativeUiVuMeter *meter);

#endif
#endif
