#ifndef LGNOME_UI_PRECONNECT_DISPLAY_H
#define LGNOME_UI_PRECONNECT_DISPLAY_H

#include "ui_preconnect.h"

#include "lvgl.h"

void native_ui_preconnect_display_flush(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *src);
void native_ui_preconnect_display_clear(lv_disp_drv_t *drv, uint8_t *buf, uint32_t size);
void native_ui_preconnect_pointer_read(lv_indev_drv_t *drv, lv_indev_data_t *data);
void native_ui_preconnect_key_read(lv_indev_drv_t *drv, lv_indev_data_t *data);
void native_ui_preconnect_reset_input_state(NativePreconnectUi *ui);

#endif
