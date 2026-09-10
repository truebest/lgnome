#include "ui_preconnect_internal.h"

#include <stdio.h>

#include "rdp_ffi.h"
#include "ui_profile_name.h"
#include "ui_slot_palette.h"

#include "clog.h"

clog_define(g_native_log_ui_hub, cLogLevelInfo, "ui.hub");

static const char *const UI_SLOT_NAMES[NATIVE_SETTINGS_MAX_SESSIONS] = {"Red", "Green", "Yellow", "Blue"};
static const char *const UI_SLOT_NAMES_UPPER[NATIVE_SETTINGS_MAX_SESSIONS] = {"RED", "GREEN", "YELLOW", "BLUE"};

/* HUB presentation, selection, request state, and spatial navigation. */
const char *ui_slot_display_name(const NativePreconnectUi *ui, int slot, char *fallback, size_t fallback_cap) {
    const NativeSessionConfig *values = &ui->slot_values[slot];
    if (values->name[0] && native_ui_profile_name_valid(values->name, sizeof(values->name))) {
        return values->name;
    }
    if (ui_slot_configured(values) && values->host[0]) {
        return values->host;
    }
    (void)snprintf(fallback, fallback_cap, "%s", UI_SLOT_NAMES_UPPER[slot]);
    return fallback;
}

static bool ui_audio_stream_text(const NativePreconnectUi *ui, int slot, char *text, size_t text_cap) {
    if (!ui || !text || text_cap == 0 || slot < 0 || slot >= NATIVE_SETTINGS_MAX_SESSIONS) {
        return false;
    }

    if (!ui->slot_audio_stream_open[slot] || ui->slot_audio_sample_rate[slot] == 0 ||
        ui->slot_audio_channels[slot] == 0) {
        (void)snprintf(text, text_cap, "Audio  \xc2\xb7  Waiting for stream");
        return true;
    }

    const char *codec = NULL;
    if (ui->slot_audio_codec[slot] == RDP_AUDIO_CODEC_OPUS) {
        codec = "Opus";
    } else if (ui->slot_audio_codec[slot] == RDP_AUDIO_CODEC_PCM_S16LE) {
        codec = "PCM S16LE";
    } else {
        return false;
    }

    char sample_rate[20];
    uint32_t rate = ui->slot_audio_sample_rate[slot];
    if (rate % 1000u == 0u) {
        (void)snprintf(sample_rate, sizeof(sample_rate), "%u kHz", (unsigned)(rate / 1000u));
    } else if (rate % 100u == 0u) {
        (void)snprintf(sample_rate, sizeof(sample_rate), "%u.%u kHz", (unsigned)(rate / 1000u),
                       (unsigned)((rate % 1000u) / 100u));
    } else if (rate % 10u == 0u) {
        (void)snprintf(sample_rate, sizeof(sample_rate), "%u.%02u kHz", (unsigned)(rate / 1000u),
                       (unsigned)((rate % 1000u) / 10u));
    } else {
        (void)snprintf(sample_rate, sizeof(sample_rate), "%u.%03u kHz", (unsigned)(rate / 1000u),
                       (unsigned)(rate % 1000u));
    }

    char channel_count[16];
    const char *channels = channel_count;
    if (ui->slot_audio_channels[slot] == 1) {
        channels = "Mono";
    } else if (ui->slot_audio_channels[slot] == 2) {
        channels = "Stereo";
    } else {
        (void)snprintf(channel_count, sizeof(channel_count), "%u ch",
                       (unsigned)ui->slot_audio_channels[slot]);
    }
    (void)snprintf(text, text_cap, "%s  \xc2\xb7  %s  \xc2\xb7  %s", codec, sample_rate, channels);
    return true;
}

static void ui_update_slot_buttons(NativePreconnectUi *ui) {
    for (int slot = 0; slot < NATIVE_SETTINGS_MAX_SESSIONS; slot++) {
        if (!ui->slot_buttons[slot]) {
            continue;
        }
        if (slot == ui->selected_slot) {
            lv_obj_add_state(ui->slot_buttons[slot], LV_STATE_CHECKED);
        } else {
            lv_obj_clear_state(ui->slot_buttons[slot], LV_STATE_CHECKED);
        }
    }
}

void ui_update_hub(NativePreconnectUi *ui) {
    if (!ui || !ui->hero_name_label) {
        return;
    }
    int selected = ui->selected_slot;
    const NativeSessionConfig *values = &ui->slot_values[selected];
    NativePreconnectSessionState state = ui->slot_states[selected];
    NativeSessionStatus presentation = native_session_status(state, ui->slot_reasons[selected]);
    char fallback[32];
    const char *display_name = ui_slot_display_name(ui, selected, fallback, sizeof(fallback));
    lv_label_set_text(ui->hero_slot_label, UI_SLOT_NAMES_UPPER[selected]);
    lv_label_set_text(ui->hero_name_label, display_name);
    lv_label_set_text(ui->hero_state_label, presentation.title);
    lv_obj_set_style_bg_color(ui->hero_chip, lv_color_hex(native_ui_slot_rgb(selected)), 0);
    lv_obj_set_style_shadow_color(ui->hero_chip, lv_color_hex(native_ui_slot_rgb(selected)), 0);
    uint32_t state_color = presentation.color;
    lv_obj_set_style_bg_color(ui->hero_state_dot, lv_color_hex(state_color), 0);
    lv_obj_set_style_shadow_color(ui->hero_state_dot, lv_color_hex(state_color), 0);
    lv_obj_set_style_shadow_width(ui->hero_state_dot, 18, 0);
    lv_obj_set_style_shadow_opa(ui->hero_state_dot,
                                state == NATIVE_PRECONNECT_SESSION_CONNECTED ? LV_OPA_60 : LV_OPA_TRANSP, 0);

    char meta[NATIVE_SETTINGS_STRING_MAX + 96u];
    if (state == NATIVE_PRECONNECT_SESSION_NOT_SET_UP) {
        (void)snprintf(meta, sizeof(meta), "Assign a computer to the %s key to switch to it instantly.",
                       UI_SLOT_NAMES[selected]);
    } else if (state == NATIVE_PRECONNECT_SESSION_CONNECTED && ui->slot_desktop_width[selected] != 0 &&
               ui->slot_desktop_height[selected] != 0) {
        (void)snprintf(meta, sizeof(meta), "%u\xc3\x97%u  \xc2\xb7  %u fps  \xc2\xb7  Session %u min",
                       (unsigned)ui->slot_desktop_width[selected], (unsigned)ui->slot_desktop_height[selected],
                       (unsigned)values->fps, (unsigned)ui->slot_session_minutes[selected]);
    } else {
        (void)snprintf(meta, sizeof(meta), "%s:%u - %u fps", values->host, (unsigned)values->port,
                       (unsigned)values->fps);
    }
    lv_label_set_text(ui->hero_meta_label, meta);
    char audio_meta[80];
    if (state == NATIVE_PRECONNECT_SESSION_CONNECTED &&
        ui_audio_stream_text(ui, selected, audio_meta, sizeof(audio_meta))) {
        lv_label_set_text(ui->hero_audio_label, audio_meta);
        lv_obj_clear_flag(ui->hero_audio_group, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(ui->hero_audio_group, LV_OBJ_FLAG_HIDDEN);
    }
    bool show_detail = ui->slot_details[selected][0] != '\0' && native_ui_session_terminal(state);
    if (show_detail) {
        lv_label_set_text(ui->hero_detail_label, ui->slot_details[selected]);
        lv_obj_set_style_bg_color(ui->hero_detail_panel, lv_color_hex(presentation.color), 0);
        lv_obj_set_style_border_color(ui->hero_detail_panel, lv_color_hex(presentation.color), 0);
        lv_obj_clear_flag(ui->hero_detail_panel, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(ui->hero_detail_panel, LV_OBJ_FLAG_HIDDEN);
    }

    lv_label_set_text(ui->hero_action_label, presentation.action);
    ui_set_disabled(ui->hero_action_btn, native_ui_session_connecting(state));
    ui_set_hidden(ui->hero_edit_btn, state == NATIVE_PRECONNECT_SESSION_NOT_SET_UP || native_ui_session_connecting(state));

    for (int slot = 0; slot < NATIVE_SETTINGS_MAX_SESSIONS; slot++) {
        char card_fallback[32];
        const char *card_name = ui_slot_display_name(ui, slot, card_fallback, sizeof(card_fallback));
        NativePreconnectSessionState card_state = ui->slot_states[slot];
        NativeSessionStatus card_status = native_session_status(card_state, ui->slot_reasons[slot]);
        lv_label_set_text(ui->card_name_labels[slot], card_name);
        lv_label_set_text(ui->card_badge_labels[slot], card_status.badge);
        uint32_t badge_color = card_status.color;
        lv_obj_set_style_bg_color(ui->card_badges[slot], lv_color_hex(badge_color), 0);
        lv_obj_set_style_bg_opa(ui->card_badges[slot], card_state == NATIVE_PRECONNECT_SESSION_NOT_SET_UP
                                                                  ? LV_OPA_TRANSP
                                                                  : LV_OPA_20,
                                0);
        lv_obj_set_style_border_width(ui->card_badges[slot], 1, 0);
        lv_obj_set_style_border_color(ui->card_badges[slot], lv_color_hex(badge_color), 0);
        lv_obj_set_style_border_opa(ui->card_badges[slot], LV_OPA_40, 0);
        lv_obj_set_style_text_color(ui->card_badge_labels[slot], lv_color_hex(badge_color), 0);
        lv_obj_set_style_text_opa(ui->card_badge_labels[slot], LV_OPA_COVER, 0);
        lv_obj_set_style_bg_opa(ui->slot_buttons[slot], card_state == NATIVE_PRECONNECT_SESSION_NOT_SET_UP
                                                              ? LV_OPA_70
                                                              : LV_OPA_COVER,
                                0);
        lv_obj_set_style_bg_opa(ui->card_tabs[slot],
                                card_state == NATIVE_PRECONNECT_SESSION_NOT_SET_UP ? (lv_opa_t)115
                                                                                 : LV_OPA_COVER,
                                0);
        ui_set_hidden(ui->card_audio_groups[slot], !(card_state == NATIVE_PRECONNECT_SESSION_CONNECTED));
    }
    ui_update_slot_buttons(ui);
    if (!ui->setup_visible && !ui->capture_settings_visible &&
        !ui->onboarding_visible && ui->group) {
        /* Session state can change asynchronously while a HUB control owns focus. Do
         * not leave the keypad stranded on an action that just became disabled or an
         * Edit button that was hidden when the selected slot started connecting. */
        lv_obj_t *focused = lv_group_get_focused(ui->group);
        if ((focused == ui->hero_action_btn && lv_obj_has_state(ui->hero_action_btn, LV_STATE_DISABLED)) ||
            (focused == ui->hero_edit_btn && lv_obj_has_flag(ui->hero_edit_btn, LV_OBJ_FLAG_HIDDEN))) {
            lv_group_focus_obj(ui->slot_buttons[selected]);
        }
    }
}

int native_preconnect_ui_selected_slot(const NativePreconnectUi *ui) {
    return ui ? ui->selected_slot : -1;
}

bool native_preconnect_ui_session_keys_enabled(const NativePreconnectUi *ui) {
    return ui && !ui->connecting && !ui->onboarding_visible && !ui->capture_settings_visible &&
           !ui_any_action_pending(ui);
}

void native_preconnect_ui_select_slot(NativePreconnectUi *ui, int slot) {
    if (!ui || slot < 0 || slot >= NATIVE_SETTINGS_MAX_SESSIONS) {
        return;
    }
    bool discarded_form = ui->setup_visible && !ui_profile_action_pending(ui);
    if (discarded_form) {
        /* Slot and colour-key navigation leaves the setup drawer without accepting it.
         * Restore both the model and widgets so a later get_slot_values() cannot
         * resurrect and persist the abandoned form. Save and Save-and-connect snapshot
         * the form before their pending flag is set and retain those explicit edits. */
        ui_discard_form_changes(ui);
        ui_set_text(ui->status_label, "");
    }
    if (slot == ui->selected_slot) {
        if (discarded_form) {
            ui_load_slot_into_form(ui, slot);
        }
        ui_update_hub(ui);
        return;
    }
    ui->selected_slot = slot;
    ui_load_slot_into_form(ui, slot);
    ui_update_hub(ui);
    if (!ui->setup_visible && !ui->capture_settings_visible &&
        !ui->onboarding_visible && ui->group) {
        lv_group_focus_obj(ui->slot_buttons[slot]);
    }
}

void native_preconnect_ui_show_hub(NativePreconnectUi *ui, int active_slot) {
    if (!ui) {
        return;
    }
    clog(cLogLevelDebug, "showing HUB with active slot %d", active_slot);
    if (active_slot >= 0 && active_slot < NATIVE_SETTINGS_MAX_SESSIONS) {
        native_preconnect_ui_select_slot(ui, active_slot);
    }
    /* A previous HUB close happens on BACK-down, before its physical release reaches
     * the app. Do not carry LVGL's PRESSED/long-press state into this new visit. */
    native_ui_preconnect_reset_input_state(ui);
    ui->activate_requested = false;
    ui->hub_close_requested = false;
    ui->hub_closing = false;
    ui_show_onboarding(ui, false);
    ui_show_setup(ui, false);
    if (ui->capture_settings_visible && !ui->capture_save_pending) {
        ui_restore_capture_settings(ui);
    }
    ui_show_capture_settings(ui, false);
}

bool native_preconnect_ui_cycle_slot(NativePreconnectUi *ui, int direction) {
    if (!ui || direction == 0 || ui->connecting || ui_any_action_pending(ui) || ui_drawer_open(ui)) {
        return false;
    }
    int step_direction = direction > 0 ? 1 : -1;
    for (int step = 1; step <= NATIVE_SETTINGS_MAX_SESSIONS; step++) {
        int candidate = (ui->selected_slot + step_direction * step) % NATIVE_SETTINGS_MAX_SESSIONS;
        if (candidate < 0) {
            candidate += NATIVE_SETTINGS_MAX_SESSIONS;
        }
        if (ui_slot_configured(&ui->slot_values[candidate])) {
            native_preconnect_ui_select_slot(ui, candidate);
            return true;
        }
    }
    return false;
}

void native_preconnect_ui_cancel_pending_navigation(NativePreconnectUi *ui) {
    if (!ui) {
        return;
    }
    ui->activate_requested = false;
    if (ui->connect_requested) {
        ui->connect_requested = false;
        ui->connect_save_pending = false;
        ui->connecting = false;
        if (ui->requested_slot >= 0 && ui->requested_slot < NATIVE_SETTINGS_MAX_SESSIONS) {
            ui->slot_states[ui->requested_slot] = ui->requested_previous_state;
            ui->slot_reasons[ui->requested_slot] = ui->requested_previous_reason;
            (void)snprintf(ui->slot_details[ui->requested_slot], UI_DETAIL_MAX, "%s",
                           ui->requested_previous_detail);
        }
        ui_update_hub(ui);
        ui_update_connect_state(ui);
    }
}

void ui_slot_button_focused(lv_event_t *event) {
    NativePreconnectUi *ui = (NativePreconnectUi *)lv_event_get_user_data(event);
    if (!ui || ui->hub_closing || ui->rebuilding_group || ui_drawer_open(ui) ||
        ui_any_action_pending(ui)) {
        return;
    }
    lv_indev_t *active_indev = lv_indev_get_act();
    if (!active_indev || lv_indev_get_type(active_indev) != LV_INDEV_TYPE_KEYPAD) {
        return;
    }
    lv_obj_t *target = lv_event_get_current_target(event);
    for (int slot = 0; slot < NATIVE_SETTINGS_MAX_SESSIONS; slot++) {
        if (ui->slot_buttons[slot] == target && ui->selected_slot != slot) {
            ui->selected_slot = slot;
            ui_load_slot_into_form(ui, slot);
            ui_update_hub(ui);
            return;
        }
    }
}

void ui_slot_button_clicked(lv_event_t *event) {
    NativePreconnectUi *ui = (NativePreconnectUi *)lv_event_get_user_data(event);
    /* The capture drawer blocks card clicks, but the setup drawer's scrim already
     * swallows them, so only the capture flag is checked here. */
    if (!ui || ui->hub_closing || ui_any_action_pending(ui) || ui->capture_settings_visible) {
        return;
    }
    lv_obj_t *target = lv_event_get_current_target(event);
    for (int slot = 0; slot < NATIVE_SETTINGS_MAX_SESSIONS; slot++) {
        if (ui->slot_buttons[slot] == target) {
            if (ui->selected_slot == slot) {
                ui_hero_action_clicked(event);
            } else {
                native_preconnect_ui_select_slot(ui, slot);
            }
            break;
        }
    }
    ui_update_slot_buttons(ui);
}

bool native_preconnect_ui_take_activate(NativePreconnectUi *ui, int *slot) {
    if (!ui || !ui->activate_requested) {
        return false;
    }
    ui->activate_requested = false;
    if (slot) {
        *slot = ui->activated_slot;
    }
    return true;
}

bool native_preconnect_ui_take_hub_close(NativePreconnectUi *ui) {
    if (!ui || !ui->hub_close_requested) {
        return false;
    }
    ui->hub_close_requested = false;
    return true;
}

void native_preconnect_ui_cancel_hub_close(NativePreconnectUi *ui) {
    if (!ui) {
        return;
    }
    ui->hub_close_requested = false;
    ui->hub_closing = false;
}

/* Spatial remote navigation for the HUB. LEFT/RIGHT always means computer selection;
 * UP/DOWN move between the selected card and its action row, then Edit and Help. The
 * explicit table avoids relying on LVGL's default keypad behavior (buttons consume
 * arrow keys without moving group focus). */
bool ui_hub_navigate(NativePreconnectUi *ui, lv_obj_t *target, uint32_t key) {
    if (!ui || !target || ui->connecting || ui_any_action_pending(ui) || ui_drawer_open(ui)) {
        return false;
    }

    int card_slot = -1;
    for (int slot = 0; slot < NATIVE_SETTINGS_MAX_SESSIONS; slot++) {
        if (target == ui->slot_buttons[slot]) {
            card_slot = slot;
            break;
        }
    }
    bool hub_control = card_slot >= 0 || target == ui->hero_action_btn || target == ui->hero_edit_btn ||
                       target == ui->capture_settings_btn || target == ui->help_btn;
    if (!hub_control) {
        return false;
    }

    if (key == LV_KEY_LEFT || key == LV_KEY_RIGHT) {
        int direction = key == LV_KEY_RIGHT ? 1 : -1;
        int next = (ui->selected_slot + direction + NATIVE_SETTINGS_MAX_SESSIONS) %
                   NATIVE_SETTINGS_MAX_SESSIONS;
        native_preconnect_ui_select_slot(ui, next);
        return true;
    }
    if (card_slot >= 0 && (key == LV_KEY_UP || key == LV_KEY_DOWN)) {
        lv_group_focus_obj(
            lv_obj_has_state(ui->hero_action_btn, LV_STATE_DISABLED)
                ? ui->capture_settings_btn
                : ui->hero_action_btn);
        return true;
    }

    bool edit_visible = !lv_obj_has_flag(ui->hero_edit_btn, LV_OBJ_FLAG_HIDDEN);
    if (target == ui->hero_action_btn) {
        if (key == LV_KEY_UP) {
            lv_group_focus_obj(ui->slot_buttons[ui->selected_slot]);
            return true;
        }
        if (key == LV_KEY_DOWN) {
            lv_group_focus_obj(edit_visible ? ui->hero_edit_btn
                                            : ui->capture_settings_btn);
            return true;
        }
    } else if (target == ui->hero_edit_btn) {
        if (key == LV_KEY_UP) {
            lv_group_focus_obj(ui->hero_action_btn);
            return true;
        }
        if (key == LV_KEY_DOWN) {
            lv_group_focus_obj(ui->capture_settings_btn);
            return true;
        }
    } else if (target == ui->capture_settings_btn) {
        if (key == LV_KEY_UP) {
            lv_obj_t *up_target =
                edit_visible ? ui->hero_edit_btn : ui->hero_action_btn;
            if (lv_obj_has_state(up_target, LV_STATE_DISABLED)) {
                up_target = ui->slot_buttons[ui->selected_slot];
            }
            lv_group_focus_obj(up_target);
            return true;
        }
        if (key == LV_KEY_DOWN) {
            lv_group_focus_obj(ui->help_btn);
            return true;
        }
    } else if (target == ui->help_btn) {
        if (key == LV_KEY_UP) {
            lv_group_focus_obj(ui->capture_settings_btn);
            return true;
        }
        if (key == LV_KEY_DOWN) {
            lv_group_focus_obj(ui->slot_buttons[ui->selected_slot]);
            return true;
        }
    }
    return false;
}

void ui_hub_key_event(lv_event_t *event) {
    NativePreconnectUi *ui = (NativePreconnectUi *)lv_event_get_user_data(event);
    if (!ui) {
        return;
    }
    uint32_t key = lv_event_get_key(event);
    if (ui->hub_closing || ui->connecting || ui_any_action_pending(ui) || ui_drawer_open(ui)) {
        return;
    } else if (ui_hub_navigate(ui, lv_event_get_target(event), key)) {
        lv_event_stop_processing(event);
    }
}
