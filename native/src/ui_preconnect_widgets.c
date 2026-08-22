#include "ui_preconnect_internal.h"

#include <string.h>

#include "ui_fonts.h"

#include "clog.h"

clog_define(g_native_log_ui_widgets, cLogLevelInfo, cLogFlags_Default, "ui.widgets", NULL);

lv_obj_t *native_ui_preconnect_make_label(lv_obj_t *parent, const char *text, lv_style_t *style) {
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_remove_style_all(label);
    lv_obj_add_style(label, style, 0);
    lv_label_set_text(label, text);
    return label;
}

lv_obj_t *native_ui_preconnect_make_dropdown(NativePreconnectUi *ui, lv_obj_t *parent, lv_coord_t width) {
    lv_obj_t *dropdown = lv_dropdown_create(parent);
    lv_obj_remove_style_all(dropdown);
    lv_obj_add_style(dropdown, &ui->dropdown_style, 0);
    lv_obj_add_style(dropdown, &ui->dropdown_focus_style, LV_STATE_FOCUSED);
    lv_obj_set_size(dropdown, width, 58);
    lv_obj_set_style_text_color(dropdown, lv_color_hex(0xf6f8fb), LV_PART_INDICATOR);
    /* JetBrains Mono intentionally contains no FontAwesome private-use glyphs. Keep
     * LVGL's dropdown chevron on the built-in icon-capable font. */
    lv_obj_set_style_text_font(dropdown, &lv_font_montserrat_20, LV_PART_INDICATOR);
    lv_obj_set_style_pad_right(dropdown, 12, LV_PART_INDICATOR);
    lv_dropdown_set_symbol(dropdown, LV_SYMBOL_DOWN);
    lv_dropdown_set_dir(dropdown, LV_DIR_BOTTOM);
    lv_dropdown_set_selected_highlight(dropdown, true);
    lv_obj_t *list = lv_dropdown_get_list(dropdown);
    lv_obj_remove_style_all(list);
    lv_obj_add_style(list, &ui->dropdown_list_style, 0);
    lv_obj_add_style(list, &ui->dropdown_selected_style, LV_PART_SELECTED);
    return dropdown;
}

lv_obj_t *native_ui_preconnect_make_box(lv_obj_t *parent, int x, int y, int width, int height) {
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_set_pos(box, x, y);
    lv_obj_set_size(box, width, height);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    return box;
}

static lv_color_t ui_color_with_alpha(uint32_t rgb, lv_opa_t alpha) {
    lv_color_t color = lv_color_hex(rgb);
    LV_COLOR_SET_A(color, alpha);
    return color;
}

lv_obj_t *native_ui_preconnect_make_alpha_gradient(lv_obj_t *parent, int x, int y, int width, int height,
                                                   lv_grad_dir_t direction, lv_opa_t start_alpha, lv_opa_t end_alpha,
                                                   lv_grad_dsc_t *gradient) {
    memset(gradient, 0, sizeof(*gradient));
    gradient->dir = direction;
    gradient->stops_count = 2;
    gradient->stops[0].color = ui_color_with_alpha(UI_HUB_MASK_COLOR, start_alpha);
    gradient->stops[0].frac = 0;
    gradient->stops[1].color = ui_color_with_alpha(UI_HUB_MASK_COLOR, end_alpha);
    gradient->stops[1].frac = 255;

    lv_obj_t *layer = native_ui_preconnect_make_box(parent, x, y, width, height);
    lv_obj_clear_flag(layer, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_opa(layer, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_grad(layer, gradient, 0);
    return layer;
}

lv_obj_t *native_ui_preconnect_make_audio_indicator(NativePreconnectUi *ui, lv_obj_t *parent, int x, int y,
                                                    bool compact, NativeUiVuMeter meters[UI_CARD_AUDIO_METER_COUNT],
                                                    lv_obj_t **label_out) {
    lv_obj_t *group = native_ui_preconnect_make_box(parent, x, y, compact ? 40 : 600, compact ? 38 : 30);
    lv_obj_clear_flag(group, LV_OBJ_FLAG_CLICKABLE);
    if (compact) {
        lv_obj_set_style_bg_color(group, lv_color_hex(0x040508), 0);
        lv_obj_set_style_bg_opa(group, LV_OPA_50, 0);
        lv_obj_set_style_radius(group, 8, 0);
        lv_obj_set_style_clip_corner(group, true, 0);
        for (int side = 0; side < UI_CARD_AUDIO_METER_COUNT; side++) {
            if (!meters) {
                continue;
            }
            NativeUiVuMeterSpec spec = {
                .x = side == 0 ? 11 : 23,
                .y = UI_CARD_AUDIO_METER_TOP,
                .width = UI_CARD_AUDIO_METER_WIDTH,
                .height = UI_CARD_AUDIO_METER_HEIGHT,
                .radius = 3,
                .grad_radius = 3,
                .min_px = 2,
            };
            native_ui_vu_meter_build(&meters[side], group, &spec);
        }
    } else {
        lv_obj_t *label = native_ui_preconnect_make_label(group, "", &ui->muted_style);
        lv_obj_set_pos(label, 0, -2);
        lv_obj_set_style_text_color(label, lv_color_hex(0x7ccd8e), 0);
        lv_obj_set_style_text_opa(label, LV_OPA_COVER, 0);
        if (label_out) {
            *label_out = label;
        }
    }
    return group;
}

lv_obj_t *native_ui_preconnect_make_button(NativePreconnectUi *ui, lv_obj_t *parent, int x, int y, int width,
                                           int height, const char *text, bool primary, lv_event_cb_t callback) {
    lv_obj_t *button = lv_btn_create(parent);
    lv_obj_remove_style_all(button);
    lv_obj_add_style(button, primary ? &ui->button_style : &ui->secondary_button_style, 0);
    lv_obj_add_style(button, &ui->button_focus_style, LV_STATE_FOCUSED);
    lv_obj_add_style(button, &ui->button_disabled_style, LV_STATE_DISABLED);
    lv_obj_set_pos(button, x, y);
    lv_obj_set_size(button, width, height);
    if (callback) {
        lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, ui);
    }
    lv_obj_add_event_cb(button, native_ui_preconnect_form_key_event, UI_FORM_KEY_EVENT, ui);
    lv_obj_t *label = lv_label_create(button);
    lv_label_set_text(label, text);
    lv_obj_center(label);
    return button;
}

lv_obj_t *native_ui_preconnect_make_field_label(NativePreconnectUi *ui, lv_obj_t *parent, int x, int y, int width,
                                                const char *text) {
    lv_obj_t *label = native_ui_preconnect_make_label(parent, text, &ui->eyebrow_style);
    lv_obj_set_pos(label, x, y);
    lv_obj_set_width(label, width);
    lv_obj_set_style_text_font(label, &lv_font_ibm_plex_mono_semibold_18, 0);
    return label;
}

lv_obj_t *native_ui_preconnect_make_input(NativePreconnectUi *ui, lv_obj_t *parent, int x, int y, int width,
                                          const char *text, const char *placeholder, size_t max_length,
                                          const char *accepted, bool password) {
    lv_obj_t *input = lv_textarea_create(parent);
    lv_obj_remove_style_all(input);
    lv_obj_add_style(input, &ui->input_style, 0);
    lv_obj_add_style(input, &ui->input_focus_style, LV_STATE_FOCUSED);
    lv_obj_add_style(input, &ui->input_cursor_style, LV_PART_CURSOR | LV_STATE_FOCUSED);
    lv_obj_set_pos(input, x, y);
    lv_obj_set_size(input, width, 58);
    lv_textarea_set_one_line(input, true);
    lv_textarea_set_max_length(input, (uint32_t)max_length);
    if (accepted) {
        lv_textarea_set_accepted_chars(input, accepted);
    }
    lv_textarea_set_password_mode(input, password);
    lv_textarea_set_placeholder_text(input, placeholder ? placeholder : "");
    lv_textarea_set_text(input, text ? text : "");
    lv_textarea_set_cursor_pos(input, LV_TEXTAREA_CURSOR_LAST);
    lv_obj_add_event_cb(input, native_ui_preconnect_input_changed, LV_EVENT_VALUE_CHANGED, ui);
    lv_obj_add_event_cb(input, native_ui_preconnect_form_key_event, UI_FORM_KEY_EVENT, ui);
    return input;
}

void native_ui_preconnect_widgets_ready(void) {
    clog(cLogLevelTrace, "pre-connect widgets ready");
}
