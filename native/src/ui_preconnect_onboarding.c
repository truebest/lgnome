#include "ui_preconnect_internal.h"

#include "ui_fonts.h"

#include "clog.h"

clog_define(g_native_log_ui_onboarding, cLogLevelInfo, "ui.onboarding");

void native_ui_preconnect_build_onboarding(NativePreconnectUi *ui) {
    ui->onboarding_scrim = native_ui_preconnect_make_box(ui->root, 0, 0, UI_CANVAS_WIDTH, UI_CANVAS_HEIGHT);
    lv_obj_set_style_bg_color(ui->onboarding_scrim, lv_color_hex(0x030406), 0);
    lv_obj_set_style_bg_opa(ui->onboarding_scrim, LV_OPA_90, 0);
    native_ui_preconnect_make_brand_cube(ui->onboarding_scrim, 370, 134, UI_ONBOARDING_CUBE_SIZE, 11.0f,
                                         &ui->onboarding_cube);
    native_ui_preconnect_make_wordmark(ui, ui->onboarding_scrim, 424, 141, &lv_font_jetbrains_mono_semibold_28,
                                       LV_OPA_30);
    lv_obj_t *ob_title = native_ui_preconnect_make_label(ui->onboarding_scrim,
                                                         "Four computers. Four colour keys. One TV.", &ui->title_style);
    lv_obj_set_pos(ob_title, 370, 200);
    lv_obj_set_style_text_font(ob_title, &lv_font_ibm_plex_sans_semibold_40, 0);
    lv_obj_t *ob_subtitle = native_ui_preconnect_make_label(
        ui->onboarding_scrim, "lgnome turns this TV into a screen and KVM switch for your GNOME desktops.",
        &ui->muted_style);
    lv_obj_set_pos(ob_subtitle, 370, 258);
    lv_obj_set_width(ob_subtitle, 1180);
    lv_obj_set_style_text_font(ob_subtitle, &lv_font_ibm_plex_sans_regular_24, 0);
    static const char *const step_titles[4] = {"Prepare the computer", "Plug input into the TV",
                                               "Colour keys are computers", "Press the active colour again"};
    static const char *const step_text[4] = {
        "Enable GNOME Remote Desktop in Settings > Sharing and note the address, username and password.",
        "A USB keyboard and mouse control the remote desktop directly. The LG remote handles switching.",
        "Red, green, yellow and blue each belong to one computer. Press a colour to switch instantly.",
        "The active colour opens the audio mixer, with one channel per computer and a TV master."};
    for (int step = 0; step < 4; step++) {
        int col = step % 2;
        int row = step / 2;
        lv_obj_t *card = native_ui_preconnect_make_box(ui->onboarding_scrim, 370 + col * 601, 332 + row * 220, 579,
                                                       198);
        lv_obj_set_style_bg_color(card, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(card, LV_OPA_10, 0);
        lv_obj_set_style_border_width(card, 1, 0);
        lv_obj_set_style_border_color(card, lv_color_white(), 0);
        lv_obj_set_style_border_opa(card, LV_OPA_10, 0);
        lv_obj_set_style_radius(card, 18, 0);
        lv_obj_t *number = native_ui_preconnect_make_box(card, 30, 28, 38, 38);
        lv_obj_set_style_bg_color(number, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(number, LV_OPA_10, 0);
        lv_obj_set_style_radius(number, 11, 0);
        lv_obj_t *number_label = native_ui_preconnect_make_label(number, "", &ui->label_style);
        lv_label_set_text_fmt(number_label, "%d", step + 1);
        lv_obj_set_style_text_font(number_label, &lv_font_ibm_plex_mono_semibold_18, 0);
        lv_obj_center(number_label);
        lv_obj_t *heading = native_ui_preconnect_make_label(card, step_titles[step], &ui->title_style);
        lv_obj_set_pos(heading, 84, 25);
        lv_obj_t *body = native_ui_preconnect_make_label(card, step_text[step], &ui->status_style);
        lv_obj_set_pos(body, 30, 84);
        lv_obj_set_width(body, 519);
        lv_label_set_long_mode(body, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_font(body, &lv_font_ibm_plex_sans_regular_24, 0);
    }
    ui->onboarding_btn = native_ui_preconnect_make_button(ui, ui->onboarding_scrim, 370, 828, 252, 72, "Get started",
                                                          true, ui_onboarding_close_clicked);
    lv_obj_t *ob_hint = native_ui_preconnect_make_label(ui->onboarding_scrim, "Reopen anytime from the ? button.",
                                                        &ui->status_style);
    lv_obj_set_pos(ob_hint, 646, 846);
    lv_obj_set_style_text_font(ob_hint, &lv_font_ibm_plex_sans_regular_20, 0);
    lv_obj_add_flag(ui->onboarding_scrim, LV_OBJ_FLAG_HIDDEN);

    clog(cLogLevelTrace, "onboarding screen built");
}

void ui_help_clicked(lv_event_t *event) {
    NativePreconnectUi *ui = (NativePreconnectUi *)lv_event_get_user_data(event);
    if (ui && !ui->hub_closing && !ui->connect_save_pending && !ui->save_pending && !ui->delete_pending) {
        ui_show_onboarding(ui, true);
    }
}

void ui_onboarding_close_clicked(lv_event_t *event) {
    NativePreconnectUi *ui = (NativePreconnectUi *)lv_event_get_user_data(event);
    if (ui) {
        ui_show_onboarding(ui, false);
    }
}
