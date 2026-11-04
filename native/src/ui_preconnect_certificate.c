#include "ui_preconnect_internal.h"

#include <stdio.h>
#include <string.h>

#include "ui_fonts.h"

static void certificate_clicked(lv_event_t *event) {
    NativePreconnectUi *ui = lv_event_get_user_data(event);
    if (!ui || !ui->certificate_visible) return;
    ui->certificate_accept = lv_event_get_target(event) == ui->certificate_continue;
    ui->certificate_answer_ready = true;
}

void native_ui_preconnect_certificate_focus(NativePreconnectUi *ui) {
    lv_group_remove_all_objs(ui->group);
    lv_group_add_obj(ui->group, ui->certificate_cancel);
    lv_group_add_obj(ui->group, ui->certificate_remember);
    lv_group_add_obj(ui->group, ui->certificate_continue);
    lv_group_focus_obj(ui->certificate_cancel);
}

void native_preconnect_ui_show_certificate(NativePreconnectUi *ui, const NativeSessionConfig *session,
                                            const char *sha256, const char *reason) {
    if (!ui || !session || ui->certificate_visible) return;
    if (!ui->certificate_scrim) {
        ui->certificate_scrim = native_ui_preconnect_make_box(ui->root, 0, 0, UI_CANVAS_WIDTH, UI_CANVAS_HEIGHT);
        lv_obj_set_style_bg_color(ui->certificate_scrim, lv_color_hex(0x07080b), 0);
        lv_obj_set_style_bg_opa(ui->certificate_scrim, LV_OPA_COVER, 0);
        lv_obj_t *title = native_ui_preconnect_make_label(ui->certificate_scrim,
            "The server's identity could not be verified", &ui->title_style);
        lv_obj_set_pos(title, 310, 170);
        lv_obj_set_style_text_font(title, &lv_font_ibm_plex_sans_semibold_40, 0);
        lv_obj_set_width(title, 1300);
        lv_obj_t *details = native_ui_preconnect_make_box(ui->certificate_scrim, 310, 270, 1300, 420);
        lv_obj_add_flag(details, LV_OBJ_FLAG_SCROLLABLE);
        ui->certificate_detail = native_ui_preconnect_make_label(details, "", &ui->label_style);
        lv_obj_set_width(ui->certificate_detail, 1260);
        lv_label_set_long_mode(ui->certificate_detail, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_font(ui->certificate_detail, &lv_font_ibm_plex_sans_regular_24, 0);
        ui->certificate_remember = lv_checkbox_create(ui->certificate_scrim);
        lv_checkbox_set_text(ui->certificate_remember, "Don't ask again for this server certificate");
        lv_obj_set_pos(ui->certificate_remember, 310, 720);
        lv_obj_set_style_text_font(ui->certificate_remember, &lv_font_ibm_plex_sans_regular_24, 0);
        lv_obj_set_style_text_color(ui->certificate_remember, lv_color_hex(0xeef1f7), 0);
        lv_obj_add_style(ui->certificate_remember, &ui->button_focus_style, LV_STATE_FOCUSED);
        lv_obj_add_event_cb(ui->certificate_remember, native_ui_preconnect_form_key_event, UI_FORM_KEY_EVENT, ui);
        ui->certificate_cancel = native_ui_preconnect_make_button(ui, ui->certificate_scrim,
            310, 820, 250, 72, "Cancel", false, certificate_clicked);
        ui->certificate_continue = native_ui_preconnect_make_button(ui, ui->certificate_scrim,
            590, 820, 250, 72, "Connect", true, certificate_clicked);
    }
    char endpoint[NATIVE_SETTINGS_STRING_MAX + 16];
    (void)snprintf(endpoint, sizeof(endpoint), "%s:%u", session->host, (unsigned)session->port);
    lv_label_set_text_fmt(ui->certificate_detail,
        "Server: %s\n\n%s\n\nSHA-256 fingerprint:\n%.32s\n%s\n\n"
        "Compare this fingerprint on the computer you want to connect to.\n"
        "Continue only if you trust this server.", endpoint, reason, sha256, sha256 + 32);
    lv_obj_clear_state(ui->certificate_remember, LV_STATE_CHECKED);
    ui->certificate_answer_ready = false;
    ui->certificate_visible = true;
    ui->hub_closing = false;
    ui->hub_close_requested = false;
    lv_obj_clear_flag(ui->certificate_scrim, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(ui->certificate_scrim);
    native_ui_preconnect_reset_input_state(ui);
    native_ui_preconnect_certificate_focus(ui);
}

void native_preconnect_ui_close_certificate(NativePreconnectUi *ui) {
    if (!ui || !ui->certificate_visible) return;
    ui->certificate_visible = false;
    ui->certificate_answer_ready = false;
    lv_obj_add_flag(ui->certificate_scrim, LV_OBJ_FLAG_HIDDEN);
    native_ui_preconnect_reset_input_state(ui);
    native_ui_preconnect_screen_refocus(ui);
}

bool native_preconnect_ui_take_certificate(NativePreconnectUi *ui, bool *accept, bool *remember) {
    if (!ui || !ui->certificate_visible || !ui->certificate_answer_ready) return false;
    *accept = ui->certificate_accept;
    *remember = *accept && lv_obj_has_state(ui->certificate_remember, LV_STATE_CHECKED);
    ui->certificate_answer_ready = false;
    return true;
}

void native_preconnect_ui_certificate_error(NativePreconnectUi *ui, const char *message) {
    if (ui && ui->certificate_visible) lv_label_set_text(ui->certificate_detail, message);
}

void native_preconnect_ui_set_certificate_trust(NativePreconnectUi *ui, int slot, const NativeCertificateTrust *trust) {
    if (!ui || slot < 0 || slot >= NATIVE_SETTINGS_MAX_SESSIONS) return;
    ui->slot_values[slot].certificate_trust = *trust;
    ui->committed_values[slot].certificate_trust = *trust;
}
