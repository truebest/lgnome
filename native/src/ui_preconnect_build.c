#include "ui_preconnect_internal.h"

#include <stdio.h>

#include "rdp_ffi.h"
#include "ui_fonts.h"
#include "ui_host.h"
#include "ui_profile_name.h"
#include "ui_slot_palette.h"

#include "clog.h"

clog_define(g_native_log_ui_build, cLogLevelInfo, "ui.build");

static const char *const UI_BUILD_SLOT_NAMES[NATIVE_SETTINGS_MAX_SESSIONS] = {"Red", "Green", "Yellow", "Blue"};

static void ui_build(NativePreconnectUi *ui, const char *host, uint16_t port, const char *username,
                     const char *password, const char *domain, uint16_t fps, uint16_t desktop_width,
                     uint16_t desktop_height, uint16_t audio_codec) {
    lv_obj_t *screen = lv_scr_act();
    lv_obj_set_style_bg_opa(screen, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(screen, 0, 0);
    ui->root = lv_obj_create(screen);
    lv_obj_remove_style_all(ui->root);
    lv_obj_add_style(ui->root, &ui->root_style, 0);
    lv_obj_set_size(ui->root, LV_PCT(100), LV_PCT(100));
    lv_obj_clear_flag(ui->root, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *mask = native_ui_preconnect_make_box(ui->root, 0, 0, UI_CANVAS_WIDTH, UI_CANVAS_HEIGHT);
    lv_obj_clear_flag(mask, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_opa(mask, LV_OPA_TRANSP, 0);

    int h22 = UI_HUB_X_AT_PERCENT(22);
    int h42 = UI_HUB_X_AT_PERCENT(42);
    int h62 = UI_HUB_X_AT_PERCENT(62);
    int h82 = UI_HUB_X_AT_PERCENT(82);
    (void)native_ui_preconnect_make_alpha_gradient(mask, 0, 0, h22, UI_CANVAS_HEIGHT, LV_GRAD_DIR_HOR, (lv_opa_t)245,
                                                   (lv_opa_t)230, &ui->hub_horizontal_gradients[0]);
    (void)native_ui_preconnect_make_alpha_gradient(mask, h22, 0, h42 - h22, UI_CANVAS_HEIGHT, LV_GRAD_DIR_HOR,
                                                   (lv_opa_t)230, (lv_opa_t)158, &ui->hub_horizontal_gradients[1]);
    (void)native_ui_preconnect_make_alpha_gradient(mask, h42, 0, h62 - h42, UI_CANVAS_HEIGHT, LV_GRAD_DIR_HOR,
                                                   (lv_opa_t)158, (lv_opa_t)56, &ui->hub_horizontal_gradients[2]);
    (void)native_ui_preconnect_make_alpha_gradient(mask, h62, 0, h82 - h62, UI_CANVAS_HEIGHT, LV_GRAD_DIR_HOR,
                                                   (lv_opa_t)56, LV_OPA_TRANSP, &ui->hub_horizontal_gradients[3]);

    int v74 = UI_HUB_Y_FROM_BOTTOM_PERCENT(74);
    int v60 = UI_HUB_Y_FROM_BOTTOM_PERCENT(60);
    int v45 = UI_HUB_Y_FROM_BOTTOM_PERCENT(45);
    int v28 = UI_HUB_Y_FROM_BOTTOM_PERCENT(28);
    (void)native_ui_preconnect_make_alpha_gradient(mask, 0, v74, UI_CANVAS_WIDTH, v60 - v74, LV_GRAD_DIR_VER,
                                                   LV_OPA_TRANSP, (lv_opa_t)46, &ui->hub_vertical_gradients[0]);
    (void)native_ui_preconnect_make_alpha_gradient(mask, 0, v60, UI_CANVAS_WIDTH, v45 - v60, LV_GRAD_DIR_VER,
                                                   (lv_opa_t)46, (lv_opa_t)128, &ui->hub_vertical_gradients[1]);
    (void)native_ui_preconnect_make_alpha_gradient(mask, 0, v45, UI_CANVAS_WIDTH, v28 - v45, LV_GRAD_DIR_VER,
                                                   (lv_opa_t)128, (lv_opa_t)204, &ui->hub_vertical_gradients[2]);
    (void)native_ui_preconnect_make_alpha_gradient(mask, 0, v28, UI_CANVAS_WIDTH, UI_CANVAS_HEIGHT - v28,
                                                   LV_GRAD_DIR_VER, (lv_opa_t)204, (lv_opa_t)235,
                                                   &ui->hub_vertical_gradients[3]);

    native_ui_preconnect_make_brand_cube(ui->root, 96, 44, UI_BRAND_CUBE_SIZE, 10.0f, &ui->brand_cube);
    native_ui_preconnect_make_wordmark(ui, ui->root, 146, 48, &lv_font_jetbrains_mono_semibold_28, LV_OPA_40);

    ui->keyboard_pill = native_ui_preconnect_make_box(ui->root, 1622, 46, 64, 46);
    lv_obj_add_style(ui->keyboard_pill, &ui->nav_style, 0);
    lv_obj_set_style_pad_all(ui->keyboard_pill, 0, 0);
    lv_obj_t *keyboard_icon = native_ui_preconnect_make_label(ui->keyboard_pill, LV_SYMBOL_KEYBOARD,
                                                              &ui->eyebrow_style);
    lv_obj_set_style_text_font(keyboard_icon, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(keyboard_icon, lv_color_hex(0xeef1f7), 0);
    lv_obj_set_style_text_opa(keyboard_icon, LV_OPA_80, 0);
    lv_obj_set_style_text_letter_space(keyboard_icon, 0, 0);
    lv_obj_align(keyboard_icon, LV_ALIGN_LEFT_MID, 10, 0);
    ui->keyboard_dot = native_ui_preconnect_make_box(ui->keyboard_pill, 48, 18, 10, 10);
    lv_obj_set_style_bg_opa(ui->keyboard_dot, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(ui->keyboard_dot, LV_RADIUS_CIRCLE, 0);

    lv_obj_t *mouse_pill = native_ui_preconnect_make_box(ui->root, 1700, 46, 64, 46);
    lv_obj_add_style(mouse_pill, &ui->nav_style, 0);
    lv_obj_set_style_pad_all(mouse_pill, 0, 0);
    lv_obj_t *mouse_body = native_ui_preconnect_make_box(mouse_pill, 12, 8, 20, 30);
    lv_obj_set_style_bg_opa(mouse_body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(mouse_body, 2, 0);
    lv_obj_set_style_border_color(mouse_body, lv_color_hex(0xeef1f7), 0);
    lv_obj_set_style_border_opa(mouse_body, LV_OPA_80, 0);
    lv_obj_set_style_radius(mouse_body, 10, 0);
    lv_obj_t *mouse_wheel = native_ui_preconnect_make_box(mouse_body, 7, 5, 4, 8);
    lv_obj_set_style_bg_color(mouse_wheel, lv_color_hex(0xeef1f7), 0);
    lv_obj_set_style_bg_opa(mouse_wheel, LV_OPA_80, 0);
    lv_obj_set_style_radius(mouse_wheel, LV_RADIUS_CIRCLE, 0);
    ui->mouse_dot = native_ui_preconnect_make_box(mouse_pill, 48, 18, 10, 10);
    lv_obj_set_style_bg_color(ui->mouse_dot, lv_color_hex(0x43d47e), 0);
    lv_obj_set_style_bg_opa(ui->mouse_dot, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(ui->mouse_dot, LV_RADIUS_CIRCLE, 0);

    ui->capture_settings_btn = native_ui_preconnect_make_button(ui, ui->root, 1544, 46, 64, 46, "", false,
                                                                ui_capture_settings_clicked);
    lv_obj_remove_style_all(ui->capture_settings_btn);
    lv_obj_add_style(ui->capture_settings_btn, &ui->nav_style, 0);
    lv_obj_add_style(ui->capture_settings_btn, &ui->button_focus_style, LV_STATE_FOCUSED);
    lv_obj_add_style(ui->capture_settings_btn, &ui->button_disabled_style, LV_STATE_DISABLED);
    lv_obj_set_pos(ui->capture_settings_btn, 1544, 46);
    lv_obj_set_size(ui->capture_settings_btn, 64, 46);
    lv_obj_set_style_pad_all(ui->capture_settings_btn, 0, 0);
    lv_obj_set_style_radius(ui->capture_settings_btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_t *capture_settings_label = lv_obj_get_child(ui->capture_settings_btn, 0);
    lv_label_set_text(capture_settings_label, LV_SYMBOL_VIDEO);
    lv_obj_set_style_text_font(capture_settings_label, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(capture_settings_label, lv_color_hex(0xeef1f7), 0);
    lv_obj_set_style_text_opa(capture_settings_label, LV_OPA_80, 0);
    lv_obj_set_style_text_letter_space(capture_settings_label, 0, 0);
    lv_obj_align(capture_settings_label, LV_ALIGN_LEFT_MID, 10, 0);

    lv_obj_t *webcam_dot = native_ui_preconnect_make_box(ui->capture_settings_btn, 48, 18, 10, 10);
    lv_obj_clear_flag(webcam_dot, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(webcam_dot, lv_color_hex(0x43d47e), 0);
    lv_obj_set_style_bg_opa(webcam_dot, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(webcam_dot, LV_RADIUS_CIRCLE, 0);

    ui->help_btn = native_ui_preconnect_make_button(ui, ui->root, 1778, 46, 46, 46, "?", false, ui_help_clicked);
    lv_obj_set_style_pad_all(ui->help_btn, 0, 0);
    lv_obj_set_style_radius(ui->help_btn, LV_RADIUS_CIRCLE, 0);

    ui->keyboard_warning = native_ui_preconnect_make_box(ui->root, 96, 116, 940, 62);
    lv_obj_set_style_bg_color(ui->keyboard_warning, lv_color_hex(0xe8c15a), 0);
    lv_obj_set_style_bg_opa(ui->keyboard_warning, LV_OPA_20, 0);
    lv_obj_set_style_border_width(ui->keyboard_warning, 1, 0);
    lv_obj_set_style_border_color(ui->keyboard_warning, lv_color_hex(0xe8c15a), 0);
    lv_obj_set_style_border_opa(ui->keyboard_warning, LV_OPA_40, 0);
    lv_obj_set_style_radius(ui->keyboard_warning, 14, 0);
    ui->keyboard_warning_label = native_ui_preconnect_make_label(
        ui->keyboard_warning, "No USB keyboard detected - connect one to type on the remote desktop.",
        &ui->label_style);
    lv_obj_set_pos(ui->keyboard_warning_label, 24, 7);
    lv_obj_set_style_text_color(ui->keyboard_warning_label, lv_color_hex(0xf0e3bd), 0);
    lv_obj_set_style_text_font(ui->keyboard_warning_label, &lv_font_ibm_plex_sans_regular_24, 0);
    lv_obj_add_flag(ui->keyboard_warning, LV_OBJ_FLAG_HIDDEN);

    ui->hero_chip = native_ui_preconnect_make_box(ui->root, 96, 238, 26, 26);
    lv_obj_set_style_bg_opa(ui->hero_chip, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(ui->hero_chip, 8, 0);
    lv_obj_set_style_shadow_width(ui->hero_chip, 24, 0);
    lv_obj_set_style_shadow_opa(ui->hero_chip, LV_OPA_60, 0);
    ui->hero_slot_label = native_ui_preconnect_make_label(ui->root, "RED", &ui->eyebrow_style);
    lv_obj_set_pos(ui->hero_slot_label, 136, 237);
    lv_obj_set_style_text_font(ui->hero_slot_label, &lv_font_ibm_plex_mono_semibold_18, 0);
    lv_obj_set_style_text_opa(ui->hero_slot_label, LV_OPA_50, 0);
    ui->hero_name_label = native_ui_preconnect_make_label(ui->root, "RED", &ui->hero_title_style);
    lv_obj_set_pos(ui->hero_name_label, 96, 282);
    lv_obj_set_width(ui->hero_name_label, 900);
    lv_label_set_long_mode(ui->hero_name_label, LV_LABEL_LONG_DOT);
    ui->hero_state_dot = native_ui_preconnect_make_box(ui->root, 96, 403, 16, 16);
    lv_obj_set_style_bg_opa(ui->hero_state_dot, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(ui->hero_state_dot, LV_RADIUS_CIRCLE, 0);
    ui->hero_state_label = native_ui_preconnect_make_label(ui->root, "Not set up", &ui->title_style);
    lv_obj_set_pos(ui->hero_state_label, 126, 392);
    lv_obj_set_style_text_font(ui->hero_state_label, &lv_font_ibm_plex_mono_semibold_28, 0);
    lv_obj_set_style_text_letter_space(ui->hero_state_label, 0, 0);
    ui->hero_meta_label = native_ui_preconnect_make_label(ui->root, "Assign a computer to this colour key.",
                                                          &ui->muted_style);
    lv_obj_set_pos(ui->hero_meta_label, 96, 438);
    lv_obj_set_width(ui->hero_meta_label, 820);
    lv_label_set_long_mode(ui->hero_meta_label, LV_LABEL_LONG_WRAP);
    ui->hero_audio_group = native_ui_preconnect_make_audio_indicator(ui, ui->root, 96, 482, false, NULL,
                                                                     &ui->hero_audio_label);
    lv_obj_add_flag(ui->hero_audio_group, LV_OBJ_FLAG_HIDDEN);
    ui->hero_detail_panel = native_ui_preconnect_make_box(ui->root, 96, 484, 820, 74);
    lv_obj_set_style_bg_color(ui->hero_detail_panel, lv_color_hex(0xe35d55), 0);
    lv_obj_set_style_bg_opa(ui->hero_detail_panel, LV_OPA_20, 0);
    lv_obj_set_style_border_width(ui->hero_detail_panel, 1, 0);
    lv_obj_set_style_border_color(ui->hero_detail_panel, lv_color_hex(0xe35d55), 0);
    lv_obj_set_style_border_opa(ui->hero_detail_panel, LV_OPA_40, 0);
    lv_obj_set_style_radius(ui->hero_detail_panel, 14, 0);
    ui->hero_detail_label = native_ui_preconnect_make_label(ui->hero_detail_panel, "", &ui->label_style);
    lv_obj_set_pos(ui->hero_detail_label, 20, 10);
    lv_obj_set_width(ui->hero_detail_label, 780);
    lv_label_set_long_mode(ui->hero_detail_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(ui->hero_detail_label, &lv_font_ibm_plex_sans_regular_24, 0);
    lv_obj_add_flag(ui->hero_detail_panel, LV_OBJ_FLAG_HIDDEN);
    ui->hero_action_btn = native_ui_preconnect_make_button(ui, ui->root, 96, 590, 220, 72, "Set up", true,
                                                           ui_hero_action_clicked);
    ui->hero_action_label = lv_obj_get_child(ui->hero_action_btn, 0);
    ui->hero_edit_btn = native_ui_preconnect_make_button(ui, ui->root, 334, 590, 150, 72, "Edit", false,
                                                         ui_edit_clicked);
    lv_obj_set_style_text_font(ui->hero_action_label, &lv_font_ibm_plex_sans_semibold_24, 0);
    lv_obj_set_style_text_letter_space(ui->hero_action_label, 0, 0);
    lv_obj_t *hero_edit_label = lv_obj_get_child(ui->hero_edit_btn, 0);
    lv_obj_set_style_text_font(hero_edit_label, &lv_font_ibm_plex_sans_semibold_24, 0);
    lv_obj_set_style_text_letter_space(hero_edit_label, 0, 0);

    for (int slot = 0; slot < NATIVE_SETTINGS_MAX_SESSIONS; slot++) {
        int card_x = 96 + slot * (411 + UI_CARD_GAP);
        lv_obj_t *card = lv_btn_create(ui->root);
        lv_obj_remove_style_all(card);
        lv_obj_add_style(card, &ui->card_style, 0);
        lv_obj_add_style(card, &ui->card_focus_style, LV_STATE_FOCUSED);
        lv_obj_add_style(card, &ui->card_focus_style, LV_STATE_CHECKED);
        lv_obj_set_pos(card, card_x, 752);
        lv_obj_set_size(card, 411, UI_CARD_HEIGHT);
        lv_obj_set_style_bg_color(
            card, lv_color_mix(lv_color_hex(native_ui_slot_rgb(slot)), lv_color_hex(0x10131a), LV_OPA_20), 0);
        lv_obj_add_event_cb(card, ui_slot_button_clicked, LV_EVENT_CLICKED, ui);
        lv_obj_add_event_cb(card, ui_slot_button_focused, LV_EVENT_FOCUSED, ui);
        lv_obj_add_event_cb(card, ui_hub_key_event, UI_FORM_KEY_EVENT, ui);
        ui->slot_buttons[slot] = card;

        ui->card_tabs[slot] = native_ui_preconnect_make_box(card, 20, 18, 54, 8);
        lv_obj_clear_flag(ui->card_tabs[slot], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_color(ui->card_tabs[slot], lv_color_hex(native_ui_slot_rgb(slot)), 0);
        lv_obj_set_style_bg_opa(ui->card_tabs[slot], LV_OPA_COVER, 0);
        lv_obj_set_style_radius(ui->card_tabs[slot], 4, 0);

        ui->card_audio_groups[slot] = native_ui_preconnect_make_audio_indicator(ui, card, 207, 14, true,
                                                                                ui->card_audio_meters[slot], NULL);
        lv_obj_add_flag(ui->card_audio_groups[slot], LV_OBJ_FLAG_HIDDEN);

        ui->card_badges[slot] = native_ui_preconnect_make_box(card, 255, 14, 140, 38);
        lv_obj_clear_flag(ui->card_badges[slot], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_radius(ui->card_badges[slot], 9, 0);
        ui->card_badge_labels[slot] = native_ui_preconnect_make_label(ui->card_badges[slot], "NOT SET UP",
                                                                      &ui->eyebrow_style);
        lv_obj_set_style_text_font(ui->card_badge_labels[slot], &lv_font_ibm_plex_mono_semibold_18, 0);
        lv_obj_set_style_text_letter_space(ui->card_badge_labels[slot], 1, 0);
        lv_obj_center(ui->card_badge_labels[slot]);

        lv_obj_t *number = native_ui_preconnect_make_box(card, 20, 168, 30, 30);
        lv_obj_clear_flag(number, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_color(number, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(number, LV_OPA_10, 0);
        lv_obj_set_style_border_width(number, 1, 0);
        lv_obj_set_style_border_color(number, lv_color_white(), 0);
        lv_obj_set_style_border_opa(number, LV_OPA_20, 0);
        lv_obj_set_style_radius(number, 8, 0);
        lv_obj_t *number_label = native_ui_preconnect_make_label(number, "", &ui->label_style);
        lv_label_set_text_fmt(number_label, "%d", slot + 1);
        lv_obj_set_style_text_font(number_label, &lv_font_ibm_plex_mono_semibold_18, 0);
        lv_obj_set_style_text_opa(number_label, LV_OPA_70, 0);
        lv_obj_center(number_label);
        ui->card_name_labels[slot] = native_ui_preconnect_make_label(card, UI_BUILD_SLOT_NAMES[slot], &ui->title_style);
        lv_obj_set_pos(ui->card_name_labels[slot], 64, 166);
        lv_obj_set_style_text_font(ui->card_name_labels[slot], &lv_font_ibm_plex_sans_semibold_24, 0);
        lv_obj_set_style_text_letter_space(ui->card_name_labels[slot], 0, 0);
        lv_obj_set_width(ui->card_name_labels[slot], 320);
        lv_label_set_long_mode(ui->card_name_labels[slot], LV_LABEL_LONG_DOT);
    }

    lv_obj_t *footer = native_ui_preconnect_make_label(
        ui->root, "< >  Browse computers       OK  Connect / resume       COLOUR KEYS  Switch instantly",
        &ui->status_style);
    lv_obj_set_pos(footer, 96, 1005);
    lv_obj_set_width(footer, 1728);
    lv_obj_set_style_text_font(footer, &lv_font_ibm_plex_sans_regular_20, 0);

    ui->setup_scrim = native_ui_preconnect_make_box(ui->root, 0, 0, UI_CANVAS_WIDTH, UI_CANVAS_HEIGHT);
    lv_obj_set_style_bg_color(ui->setup_scrim, lv_color_hex(0x030406), 0);
    lv_obj_set_style_bg_opa(ui->setup_scrim, LV_OPA_70, 0);
    lv_obj_add_event_cb(ui->setup_scrim, ui_setup_scrim_clicked, LV_EVENT_CLICKED, ui);
    ui->setup_panel = native_ui_preconnect_make_box(ui->setup_scrim, UI_CANVAS_WIDTH - UI_SETUP_PANEL_WIDTH, 0,
                                                    UI_SETUP_PANEL_WIDTH, UI_CANVAS_HEIGHT);
    lv_obj_add_style(ui->setup_panel, &ui->detail_style, 0);

    ui->form_title = native_ui_preconnect_make_label(ui->setup_panel, "Set up computer", &ui->title_style);
    lv_obj_set_pos(ui->form_title, 48, 38);
    lv_obj_set_style_text_font(ui->form_title, &lv_font_ibm_plex_sans_semibold_40, 0);
    lv_obj_t *setup_help = native_ui_preconnect_make_label(
        ui->setup_panel, "Enable Remote Desktop in GNOME Settings > Sharing, then enter its credentials.",
        &ui->status_style);
    lv_obj_set_pos(setup_help, 48, 88);
    lv_obj_set_width(setup_help, 584);
    lv_label_set_long_mode(setup_help, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(setup_help, &lv_font_ibm_plex_sans_regular_24, 0);

    native_ui_preconnect_make_field_label(ui, ui->setup_panel, 48, 150, 584, "NAME");
    ui->name_input = native_ui_preconnect_make_input(ui, ui->setup_panel, 48, 174, 584,
                                                     ui->slot_values[ui->selected_slot].name, "e.g. Studio PC",
                                                     UI_NAME_MAX - 1, native_ui_profile_name_accepted_chars(), false);
    lv_obj_add_event_cb(ui->name_input, ui_profile_name_insert, LV_EVENT_INSERT, ui);
    native_ui_preconnect_make_field_label(ui, ui->setup_panel, 48, 246, 584, "REMOTE COLOUR KEY (FIXED)");
    for (int slot = 0; slot < NATIVE_SETTINGS_MAX_SESSIONS; slot++) {
        ui->color_choices[slot] = native_ui_preconnect_make_box(ui->setup_panel, 48 + slot * 68, 272, 52, 52);
        lv_obj_set_style_bg_color(ui->color_choices[slot], lv_color_hex(native_ui_slot_rgb(slot)), 0);
        lv_obj_set_style_radius(ui->color_choices[slot], 15, 0);
    }

    native_ui_preconnect_make_field_label(ui, ui->setup_panel, 48, 344, 394, "ADDRESS");
    native_ui_preconnect_make_field_label(ui, ui->setup_panel, 458, 344, 174, "PORT");
    ui->host_input = native_ui_preconnect_make_input(ui, ui->setup_panel, 48, 368, 394, host, "192.168.1.40",
                                                     UI_HOST_MAX - 1, native_ui_host_accepted_chars(), false);
    char port_text[UI_PORT_MAX];
    (void)snprintf(port_text, sizeof(port_text), "%u", (unsigned)port);
    ui->port_input = native_ui_preconnect_make_input(ui, ui->setup_panel, 458, 368, 174, port_text, "3389",
                                                     UI_PORT_MAX - 1, "0123456789", false);

    native_ui_preconnect_make_field_label(ui, ui->setup_panel, 48, 440, 282, "USERNAME");
    native_ui_preconnect_make_field_label(ui, ui->setup_panel, 346, 440, 286, "DOMAIN (OPTIONAL)");
    ui->username_input = native_ui_preconnect_make_input(ui, ui->setup_panel, 48, 464, 282, username, "username",
                                                         UI_USERNAME_MAX - 1, NULL, false);
    ui->domain_input = native_ui_preconnect_make_input(ui, ui->setup_panel, 346, 464, 286, domain, "optional",
                                                       UI_DOMAIN_MAX - 1, NULL, false);

    native_ui_preconnect_make_field_label(ui, ui->setup_panel, 48, 536, 584, "PASSWORD");
    ui->password_input = native_ui_preconnect_make_input(ui, ui->setup_panel, 48, 560, 584, password, "password",
                                                         UI_PASSWORD_TEXT_MAX, NULL, true);
    lv_obj_t *password_help = native_ui_preconnect_make_label(
        ui->setup_panel, "Stored privately on this TV when you save.", &ui->status_style);
    lv_obj_set_pos(password_help, 48, 624);

    native_ui_preconnect_make_field_label(ui, ui->setup_panel, 48, 668, 170, "FRAME RATE");
    native_ui_preconnect_make_field_label(ui, ui->setup_panel, 234, 668, 190, "DESKTOP SIZE");
    native_ui_preconnect_make_field_label(ui, ui->setup_panel, 440, 668, 192, "AUDIO QUALITY");
    ui->fps_dropdown = native_ui_preconnect_make_dropdown(ui, ui->setup_panel, 170);
    lv_obj_set_pos(ui->fps_dropdown, 48, 692);
    size_t selected_fps = ui_select_fps_index(ui, fps);
    ui_set_fps_options(ui);
    ui_set_selected_fps(ui, selected_fps);
    lv_obj_add_event_cb(ui->fps_dropdown, ui_fps_changed, LV_EVENT_VALUE_CHANGED, ui);
    lv_obj_add_event_cb(ui->fps_dropdown, native_ui_preconnect_form_key_event, UI_FORM_KEY_EVENT, ui);
    ui->desktop_dropdown = native_ui_preconnect_make_dropdown(ui, ui->setup_panel, 190);
    lv_obj_set_pos(ui->desktop_dropdown, 234, 692);
    ui_set_desktop_options(ui);
    ui_set_selected_desktop(ui, ui_desktop_option_index(desktop_width, desktop_height));
    lv_obj_add_event_cb(ui->desktop_dropdown, ui_desktop_changed, LV_EVENT_VALUE_CHANGED, ui);
    lv_obj_add_event_cb(ui->desktop_dropdown, native_ui_preconnect_form_key_event, UI_FORM_KEY_EVENT, ui);
    ui->audio_codec_dropdown = native_ui_preconnect_make_dropdown(ui, ui->setup_panel, 192);
    lv_obj_set_pos(ui->audio_codec_dropdown, 440, 692);
    lv_dropdown_set_options_static(ui->audio_codec_dropdown, "Auto (Opus)\nLossless PCM");
    lv_dropdown_set_selected(ui->audio_codec_dropdown, audio_codec == NATIVE_AUDIO_CODEC_PCM ? 1 : 0);
    lv_obj_add_event_cb(ui->audio_codec_dropdown, native_ui_preconnect_input_changed, LV_EVENT_VALUE_CHANGED, ui);
    lv_obj_add_event_cb(ui->audio_codec_dropdown, native_ui_preconnect_form_key_event, UI_FORM_KEY_EVENT, ui);

    ui->profile_camera_checkbox = lv_checkbox_create(ui->setup_panel);
    lv_checkbox_set_text(ui->profile_camera_checkbox, "Camera");
    lv_obj_set_pos(ui->profile_camera_checkbox, 48, 788);
    ui->profile_audio_input_checkbox = lv_checkbox_create(ui->setup_panel);
    lv_checkbox_set_text(ui->profile_audio_input_checkbox, "Microphone");
    lv_obj_set_pos(ui->profile_audio_input_checkbox, 346, 788);
    lv_obj_t *profile_checkboxes[] = {
        ui->profile_camera_checkbox,
        ui->profile_audio_input_checkbox,
    };
    for (size_t i = 0; i < sizeof(profile_checkboxes) / sizeof(profile_checkboxes[0]); i++) {
        lv_obj_t *checkbox = profile_checkboxes[i];
        lv_obj_set_size(checkbox, 250, 44);
        lv_obj_set_style_bg_opa(checkbox, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_text_color(checkbox, lv_color_hex(0xdbe2ef), LV_PART_MAIN);
        lv_obj_set_style_text_font(checkbox, &lv_font_ibm_plex_sans_regular_20, LV_PART_MAIN);
        lv_obj_set_style_pad_column(checkbox, 14, LV_PART_MAIN);
        lv_obj_set_style_bg_color(checkbox, lv_color_hex(0x343a46), LV_PART_INDICATOR);
        lv_obj_set_style_bg_opa(checkbox, LV_OPA_COVER, LV_PART_INDICATOR);
        lv_obj_set_style_border_width(checkbox, 2, LV_PART_INDICATOR);
        lv_obj_set_style_border_color(checkbox, lv_color_hex(0x697386), LV_PART_INDICATOR);
        lv_obj_set_style_border_opa(checkbox, LV_OPA_COVER, LV_PART_INDICATOR);
        lv_obj_set_style_radius(checkbox, 6, LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(checkbox, lv_color_hex(0x43d47e), LV_PART_INDICATOR | LV_STATE_CHECKED);
        lv_obj_set_style_border_color(checkbox, lv_color_hex(0x43d47e), LV_PART_INDICATOR | LV_STATE_CHECKED);
        lv_obj_set_style_text_color(checkbox, lv_color_hex(0x07100b), LV_PART_INDICATOR | LV_STATE_CHECKED);
        lv_obj_set_style_outline_color(checkbox, lv_color_hex(0xeef3fd), LV_STATE_FOCUSED);
        lv_obj_set_style_outline_opa(checkbox, LV_OPA_COVER, LV_STATE_FOCUSED);
        lv_obj_set_style_outline_width(checkbox, 3, LV_STATE_FOCUSED);
        lv_obj_set_style_outline_pad(checkbox, 4, LV_STATE_FOCUSED);
        lv_obj_add_event_cb(checkbox, ui_profile_capture_changed, LV_EVENT_VALUE_CHANGED, ui);
        lv_obj_add_event_cb(checkbox, native_ui_preconnect_form_key_event, UI_FORM_KEY_EVENT, ui);
    }
    ui->profile_capture_hint = native_ui_preconnect_make_label(
        ui->setup_panel, "Choose both devices from the camera icon on the HUB.", &ui->status_style);
    lv_obj_set_pos(ui->profile_capture_hint, 48, 846);
    lv_obj_set_width(ui->profile_capture_hint, 584);

    ui->status_label = native_ui_preconnect_make_label(ui->setup_panel, "", &ui->status_style);
    lv_obj_set_pos(ui->status_label, 276, 884);
    lv_obj_set_width(ui->status_label, 356);
    lv_label_set_long_mode(ui->status_label, LV_LABEL_LONG_WRAP);
    ui->delete_btn = native_ui_preconnect_make_button(ui, ui->setup_panel, 48, 874, 210, 52, "Delete profile", false,
                                                      ui_delete_clicked);
    ui->delete_label = lv_obj_get_child(ui->delete_btn, 0);
    lv_obj_set_style_text_color(ui->delete_label, lv_color_hex(0xf0958f), 0);
    lv_obj_add_flag(ui->delete_btn, LV_OBJ_FLAG_HIDDEN);
    ui->connect_btn = native_ui_preconnect_make_button(ui, ui->setup_panel, 48, 944, 270, 74, "Save and connect", true,
                                                       ui_connect_clicked);
    ui->save_btn = native_ui_preconnect_make_button(ui, ui->setup_panel, 332, 944, 128, 74, "Save", false,
                                                    ui_save_clicked);
    ui->cancel_btn = native_ui_preconnect_make_button(ui, ui->setup_panel, 474, 944, 158, 74, "Cancel", false,
                                                      ui_cancel_clicked);
    lv_obj_add_flag(ui->setup_scrim, LV_OBJ_FLAG_HIDDEN);

    ui->capture_scrim = native_ui_preconnect_make_box(ui->root, 0, 0, UI_CANVAS_WIDTH, UI_CANVAS_HEIGHT);
    lv_obj_set_style_bg_color(ui->capture_scrim, lv_color_hex(0x030406), 0);
    lv_obj_set_style_bg_opa(ui->capture_scrim, LV_OPA_70, 0);
    lv_obj_add_event_cb(ui->capture_scrim, ui_capture_scrim_clicked, LV_EVENT_CLICKED, ui);
    ui->capture_panel = native_ui_preconnect_make_box(ui->capture_scrim, UI_CANVAS_WIDTH - UI_SETUP_PANEL_WIDTH, 0,
                                                      UI_SETUP_PANEL_WIDTH, UI_CANVAS_HEIGHT);
    lv_obj_add_style(ui->capture_panel, &ui->detail_style, 0);

    lv_obj_t *capture_title = native_ui_preconnect_make_label(ui->capture_panel, "Camera & microphone",
                                                              &ui->title_style);
    lv_obj_set_pos(capture_title, 48, 38);
    lv_obj_set_style_text_font(capture_title, &lv_font_ibm_plex_sans_semibold_40, 0);
    lv_obj_t *capture_help = native_ui_preconnect_make_label(
        ui->capture_panel, "Shared devices for profiles that opt into capture.", &ui->status_style);
    lv_obj_set_pos(capture_help, 48, 92);
    lv_obj_set_width(capture_help, 584);
    lv_label_set_long_mode(capture_help, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(capture_help, &lv_font_ibm_plex_sans_regular_24, 0);

    native_ui_preconnect_make_field_label(ui, ui->capture_panel, 48, 174, 584, "CAMERA DEVICE");
    ui->camera_input_dropdown = native_ui_preconnect_make_dropdown(ui, ui->capture_panel, 584);
    lv_obj_set_pos(ui->camera_input_dropdown, 48, 198);

    native_ui_preconnect_make_field_label(ui, ui->capture_panel, 48, 286, 354, "CAMERA RESOLUTION");
    native_ui_preconnect_make_field_label(ui, ui->capture_panel, 418, 286, 214, "FRAME RATE");
    ui->camera_resolution_dropdown = native_ui_preconnect_make_dropdown(ui, ui->capture_panel, 354);
    lv_obj_set_pos(ui->camera_resolution_dropdown, 48, 310);
    ui->camera_fps_dropdown = native_ui_preconnect_make_dropdown(ui, ui->capture_panel, 214);
    lv_obj_set_pos(ui->camera_fps_dropdown, 418, 310);

    native_ui_preconnect_make_field_label(ui, ui->capture_panel, 48, 414, 584, "MICROPHONE DEVICE");
    ui->audio_input_dropdown = native_ui_preconnect_make_dropdown(ui, ui->capture_panel, 584);
    lv_obj_set_pos(ui->audio_input_dropdown, 48, 438);

    native_ui_preconnect_make_field_label(ui, ui->capture_panel, 48, 542, 584, "MICROPHONE GAIN");
    ui->audio_input_gain_dropdown = native_ui_preconnect_make_dropdown(ui, ui->capture_panel, 584);
    lv_obj_set_pos(ui->audio_input_gain_dropdown, 48, 566);

    ui_set_capture_options(ui);
    ui_load_capture_settings_form(ui);
    lv_obj_t *capture_controls[] = {
        ui->camera_input_dropdown, ui->camera_resolution_dropdown, ui->camera_fps_dropdown,
        ui->audio_input_dropdown,  ui->audio_input_gain_dropdown,
    };
    for (size_t i = 0; i < sizeof(capture_controls) / sizeof(capture_controls[0]); i++) {
        lv_obj_add_event_cb(capture_controls[i], ui_capture_settings_changed, LV_EVENT_VALUE_CHANGED, ui);
        lv_obj_add_event_cb(capture_controls[i], native_ui_preconnect_form_key_event, UI_FORM_KEY_EVENT, ui);
    }

    ui->capture_hint = native_ui_preconnect_make_label(
        ui->capture_panel,
        "Camera redirection requires hardware/native H.264. Before buying, verify Linux exposes "
        "V4L2_PIX_FMT_H264 for the wanted mode; current gnome-remote-desktop does not accept YUYV/MJPEG directly. "
        "Only the on-screen profile sends capture.",
        &ui->status_style);
    lv_obj_set_pos(ui->capture_hint, 48, 654);
    lv_obj_set_width(ui->capture_hint, 584);
    lv_label_set_long_mode(ui->capture_hint, LV_LABEL_LONG_WRAP);
    ui->capture_status_label = native_ui_preconnect_make_label(ui->capture_panel, "", &ui->status_style);
    lv_obj_set_pos(ui->capture_status_label, 48, 846);
    lv_obj_set_width(ui->capture_status_label, 584);
    lv_label_set_long_mode(ui->capture_status_label, LV_LABEL_LONG_WRAP);
    ui->capture_save_btn = native_ui_preconnect_make_button(ui, ui->capture_panel, 48, 944, 270, 74, "Save", true,
                                                            ui_capture_save_clicked);
    ui->capture_cancel_btn = native_ui_preconnect_make_button(ui, ui->capture_panel, 332, 944, 300, 74, "Cancel", false,
                                                              ui_capture_cancel_clicked);
    lv_obj_add_flag(ui->capture_scrim, LV_OBJ_FLAG_HIDDEN);

    native_ui_preconnect_build_onboarding(ui);
    ui->group = lv_group_create();
    ui_update_hub(ui);
    native_ui_preconnect_screen_init(ui, ui->onboarding_visible ? NATIVE_UI_SCREEN_ONBOARDING : NATIVE_UI_SCREEN_HUB);
    ui_update_connect_state(ui);
}

void native_ui_preconnect_build(NativePreconnectUi *ui, const char *host, uint16_t port, const char *username,
                                const char *password, const char *domain, uint16_t fps, uint16_t desktop_width,
                                uint16_t desktop_height, uint16_t audio_codec) {
    ui_build(ui, host, port, username, password, domain, fps, desktop_width, desktop_height, audio_codec);
    clog(cLogLevelTrace, "pre-connect screens built");
}
