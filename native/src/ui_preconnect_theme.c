#include "ui_preconnect_internal.h"

#include "ui_fonts.h"

#include "clog.h"

clog_define(g_native_log_ui_theme, cLogLevelInfo, "ui.theme");

static void ui_init_styles(NativePreconnectUi *ui) {
    lv_style_init(&ui->root_style);
    lv_style_set_bg_opa(&ui->root_style, LV_OPA_TRANSP);
    lv_style_set_border_width(&ui->root_style, 0);
    lv_style_set_pad_all(&ui->root_style, 0);
    lv_style_set_radius(&ui->root_style, 0);

    lv_style_init(&ui->nav_style);
    lv_style_set_bg_color(&ui->nav_style, lv_color_white());
    lv_style_set_bg_opa(&ui->nav_style, LV_OPA_10);
    lv_style_set_border_width(&ui->nav_style, 1);
    lv_style_set_border_color(&ui->nav_style, lv_color_white());
    lv_style_set_border_opa(&ui->nav_style, LV_OPA_10);
    lv_style_set_radius(&ui->nav_style, LV_RADIUS_CIRCLE);
    lv_style_set_pad_hor(&ui->nav_style, 20);
    lv_style_set_pad_ver(&ui->nav_style, 10);

    lv_style_init(&ui->detail_style);
    lv_style_set_bg_color(&ui->detail_style, lv_color_hex(0x101319));
    lv_style_set_bg_opa(&ui->detail_style, LV_OPA_COVER);
    lv_style_set_border_width(&ui->detail_style, 1);
    lv_style_set_border_color(&ui->detail_style, lv_color_white());
    lv_style_set_border_opa(&ui->detail_style, LV_OPA_10);
    lv_style_set_radius(&ui->detail_style, 0);
    lv_style_set_pad_all(&ui->detail_style, 0);

    lv_style_init(&ui->title_style);
    lv_style_set_text_color(&ui->title_style, lv_color_hex(0xeef1f7));
    lv_style_set_text_font(&ui->title_style, &lv_font_ibm_plex_sans_semibold_24);
    lv_style_set_text_letter_space(&ui->title_style, -1);

    lv_style_init(&ui->hero_title_style);
    lv_style_set_text_color(&ui->hero_title_style, lv_color_hex(0xeef1f7));
    lv_style_set_text_font(&ui->hero_title_style, &lv_font_ibm_plex_sans_semibold_86);
    lv_style_set_text_letter_space(&ui->hero_title_style, -2);

    lv_style_init(&ui->label_style);
    lv_style_set_text_color(&ui->label_style, lv_color_hex(0xeef1f7));
    lv_style_set_text_font(&ui->label_style, &lv_font_ibm_plex_sans_regular_24);

    lv_style_init(&ui->eyebrow_style);
    lv_style_set_text_color(&ui->eyebrow_style, lv_color_hex(0xe9eef8));
    lv_style_set_text_opa(&ui->eyebrow_style, LV_OPA_60);
    lv_style_set_text_font(&ui->eyebrow_style, &lv_font_ibm_plex_mono_semibold_18);
    lv_style_set_text_letter_space(&ui->eyebrow_style, 2);

    lv_style_init(&ui->muted_style);
    lv_style_set_text_color(&ui->muted_style, lv_color_hex(0xe9eef8));
    lv_style_set_text_opa(&ui->muted_style, LV_OPA_60);
    lv_style_set_text_font(&ui->muted_style, &lv_font_ibm_plex_mono_regular_22);

    lv_style_init(&ui->input_style);
    lv_style_set_bg_color(&ui->input_style, lv_color_white());
    lv_style_set_bg_opa(&ui->input_style, LV_OPA_10);
    lv_style_set_border_width(&ui->input_style, 1);
    lv_style_set_border_color(&ui->input_style, lv_color_white());
    lv_style_set_border_opa(&ui->input_style, LV_OPA_20);
    lv_style_set_radius(&ui->input_style, 12);
    lv_style_set_pad_hor(&ui->input_style, 20);
    lv_style_set_pad_ver(&ui->input_style, 14);
    lv_style_set_text_color(&ui->input_style, lv_color_hex(0xeef1f7));
    lv_style_set_text_font(&ui->input_style, &lv_font_ibm_plex_mono_regular_22);

    lv_style_init(&ui->input_focus_style);
    lv_style_set_bg_color(&ui->input_focus_style, lv_color_white());
    lv_style_set_bg_opa(&ui->input_focus_style, LV_OPA_10);
    lv_style_set_outline_color(&ui->input_focus_style, lv_color_hex(0xeef3fd));
    lv_style_set_outline_opa(&ui->input_focus_style, LV_OPA_COVER);
    lv_style_set_outline_width(&ui->input_focus_style, 3);
    lv_style_set_outline_pad(&ui->input_focus_style, 2);

    lv_style_init(&ui->input_cursor_style);
    lv_style_set_bg_opa(&ui->input_cursor_style, LV_OPA_TRANSP);
    lv_style_set_border_side(&ui->input_cursor_style, LV_BORDER_SIDE_LEFT);
    lv_style_set_border_color(&ui->input_cursor_style, lv_color_hex(0xffffff));
    lv_style_set_border_width(&ui->input_cursor_style, 2);
    lv_style_set_pad_left(&ui->input_cursor_style, -1);
    lv_style_set_anim_time(&ui->input_cursor_style, 450);

    lv_style_init(&ui->dropdown_style);
    lv_style_set_bg_color(&ui->dropdown_style, lv_color_white());
    lv_style_set_bg_opa(&ui->dropdown_style, LV_OPA_10);
    lv_style_set_border_width(&ui->dropdown_style, 1);
    lv_style_set_border_color(&ui->dropdown_style, lv_color_white());
    lv_style_set_border_opa(&ui->dropdown_style, LV_OPA_20);
    lv_style_set_radius(&ui->dropdown_style, 12);
    lv_style_set_pad_hor(&ui->dropdown_style, 16);
    lv_style_set_pad_ver(&ui->dropdown_style, 12);
    lv_style_set_text_color(&ui->dropdown_style, lv_color_hex(0xeef1f7));
    lv_style_set_text_font(&ui->dropdown_style, &lv_font_ibm_plex_mono_regular_22);

    lv_style_init(&ui->dropdown_focus_style);
    lv_style_set_bg_color(&ui->dropdown_focus_style, lv_color_white());
    lv_style_set_bg_opa(&ui->dropdown_focus_style, LV_OPA_10);
    lv_style_set_outline_color(&ui->dropdown_focus_style, lv_color_hex(0xeef3fd));
    lv_style_set_outline_opa(&ui->dropdown_focus_style, LV_OPA_COVER);
    lv_style_set_outline_width(&ui->dropdown_focus_style, 3);
    lv_style_set_outline_pad(&ui->dropdown_focus_style, 2);

    lv_style_init(&ui->dropdown_list_style);
    lv_style_set_bg_color(&ui->dropdown_list_style, lv_color_hex(0x171b23));
    lv_style_set_bg_opa(&ui->dropdown_list_style, LV_OPA_COVER);
    lv_style_set_border_width(&ui->dropdown_list_style, 1);
    lv_style_set_border_color(&ui->dropdown_list_style, lv_color_white());
    lv_style_set_border_opa(&ui->dropdown_list_style, LV_OPA_20);
    lv_style_set_radius(&ui->dropdown_list_style, 12);
    lv_style_set_pad_all(&ui->dropdown_list_style, 8);
    lv_style_set_text_color(&ui->dropdown_list_style, lv_color_hex(0xeef1f7));
    lv_style_set_text_font(&ui->dropdown_list_style, &lv_font_ibm_plex_mono_regular_22);

    lv_style_init(&ui->dropdown_selected_style);
    lv_style_set_bg_color(&ui->dropdown_selected_style, lv_color_hex(0x3f8dea));
    lv_style_set_bg_opa(&ui->dropdown_selected_style, LV_OPA_COVER);
    lv_style_set_text_color(&ui->dropdown_selected_style, lv_color_hex(0xffffff));

    lv_style_init(&ui->button_style);
    lv_style_set_bg_color(&ui->button_style, lv_color_hex(0xf2f5fb));
    lv_style_set_bg_opa(&ui->button_style, LV_OPA_COVER);
    lv_style_set_border_width(&ui->button_style, 0);
    lv_style_set_radius(&ui->button_style, 16);
    lv_style_set_pad_hor(&ui->button_style, 36);
    lv_style_set_pad_ver(&ui->button_style, 16);
    lv_style_set_text_color(&ui->button_style, lv_color_hex(0x0b0d11));
    lv_style_set_text_font(&ui->button_style, &lv_font_ibm_plex_sans_semibold_24);

    lv_style_init(&ui->secondary_button_style);
    lv_style_set_bg_color(&ui->secondary_button_style, lv_color_white());
    lv_style_set_bg_opa(&ui->secondary_button_style, LV_OPA_10);
    lv_style_set_border_width(&ui->secondary_button_style, 1);
    lv_style_set_border_color(&ui->secondary_button_style, lv_color_white());
    lv_style_set_border_opa(&ui->secondary_button_style, LV_OPA_30);
    lv_style_set_radius(&ui->secondary_button_style, 16);
    lv_style_set_pad_hor(&ui->secondary_button_style, 30);
    lv_style_set_pad_ver(&ui->secondary_button_style, 16);
    lv_style_set_text_color(&ui->secondary_button_style, lv_color_hex(0xeef1f7));
    lv_style_set_text_font(&ui->secondary_button_style, &lv_font_ibm_plex_sans_semibold_24);

    lv_style_init(&ui->button_focus_style);
    lv_style_set_outline_color(&ui->button_focus_style, lv_color_hex(0xeef3fd));
    lv_style_set_outline_opa(&ui->button_focus_style, LV_OPA_COVER);
    lv_style_set_outline_width(&ui->button_focus_style, 4);
    lv_style_set_outline_pad(&ui->button_focus_style, 3);

    lv_style_init(&ui->button_disabled_style);
    lv_style_set_bg_opa(&ui->button_disabled_style, LV_OPA_30);
    lv_style_set_text_opa(&ui->button_disabled_style, LV_OPA_40);

    lv_style_init(&ui->card_style);
    lv_style_set_bg_color(&ui->card_style, lv_color_hex(0x11151c));
    lv_style_set_bg_grad_color(&ui->card_style, lv_color_hex(0x080a0e));
    lv_style_set_bg_grad_dir(&ui->card_style, LV_GRAD_DIR_VER);
    lv_style_set_bg_opa(&ui->card_style, LV_OPA_COVER);
    lv_style_set_border_width(&ui->card_style, 1);
    lv_style_set_border_color(&ui->card_style, lv_color_white());
    lv_style_set_border_opa(&ui->card_style, LV_OPA_10);
    lv_style_set_radius(&ui->card_style, 18);
    lv_style_set_shadow_width(&ui->card_style, 24);
    lv_style_set_shadow_ofs_y(&ui->card_style, 8);
    lv_style_set_shadow_color(&ui->card_style, lv_color_black());
    lv_style_set_shadow_opa(&ui->card_style, LV_OPA_40);

    lv_style_init(&ui->card_focus_style);
    lv_style_set_outline_color(&ui->card_focus_style, lv_color_hex(0xeef3fd));
    lv_style_set_outline_opa(&ui->card_focus_style, LV_OPA_COVER);
    lv_style_set_outline_width(&ui->card_focus_style, 4);
    lv_style_set_outline_pad(&ui->card_focus_style, 3);

    lv_style_init(&ui->status_style);
    lv_style_set_text_color(&ui->status_style, lv_color_hex(0xe9eef8));
    lv_style_set_text_opa(&ui->status_style, LV_OPA_60);
    lv_style_set_text_font(&ui->status_style, &lv_font_ibm_plex_sans_regular_20);
}

static void ui_reset_styles(NativePreconnectUi *ui) {
    lv_style_reset(&ui->root_style);
    lv_style_reset(&ui->nav_style);
    lv_style_reset(&ui->detail_style);
    lv_style_reset(&ui->title_style);
    lv_style_reset(&ui->hero_title_style);
    lv_style_reset(&ui->label_style);
    lv_style_reset(&ui->eyebrow_style);
    lv_style_reset(&ui->muted_style);
    lv_style_reset(&ui->input_style);
    lv_style_reset(&ui->input_focus_style);
    lv_style_reset(&ui->input_cursor_style);
    lv_style_reset(&ui->dropdown_style);
    lv_style_reset(&ui->dropdown_focus_style);
    lv_style_reset(&ui->dropdown_list_style);
    lv_style_reset(&ui->dropdown_selected_style);
    lv_style_reset(&ui->button_style);
    lv_style_reset(&ui->secondary_button_style);
    lv_style_reset(&ui->button_focus_style);
    lv_style_reset(&ui->button_disabled_style);
    lv_style_reset(&ui->card_style);
    lv_style_reset(&ui->card_focus_style);
    lv_style_reset(&ui->status_style);
}

void native_ui_preconnect_theme_init(NativePreconnectUi *ui) {
    ui_init_styles(ui);
    clog(cLogLevelTrace, "pre-connect theme initialized");
}

void native_ui_preconnect_theme_reset(NativePreconnectUi *ui) {
    ui_reset_styles(ui);
}
