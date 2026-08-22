/* Profile draft, validation, persistence-request, connect, and delete behavior. */
#include "ui_preconnect_internal.h"

#include <stdio.h>
#include <string.h>

#include "rdp_ffi.h"
#include "ui_host.h"
#include "ui_mixer.h"
#include "ui_profile_name.h"
#include "ui_slot_palette.h"

#include "clog.h"

clog_define(g_native_log_ui_profile, cLogLevelInfo, cLogFlags_Default, "ui.profile", NULL);

static bool ui_queue_connect(NativePreconnectUi *ui, int slot, bool force_save);

bool ui_slot_configured(const NativeSessionConfig *values) {
    return values && ui_host_valid(values->host) && values->username[0] && values->password[0];
}

static void ui_store_form_to_slot(NativePreconnectUi *ui, int slot) {
    if (slot < 0 || slot >= NATIVE_SETTINGS_MAX_SESSIONS) {
        return;
    }
    NativeSessionConfig *values = &ui->slot_values[slot];
    const char *name = lv_textarea_get_text(ui->name_input);
    const char *host = lv_textarea_get_text(ui->host_input);
    const char *port_text = lv_textarea_get_text(ui->port_input);
    const char *username = lv_textarea_get_text(ui->username_input);
    const char *domain = lv_textarea_get_text(ui->domain_input);
    const char *password = lv_textarea_get_text(ui->password_input);
    if (native_ui_profile_name_valid(name, sizeof(values->name))) {
        (void)snprintf(values->name, sizeof(values->name), "%s", name);
    } else {
        values->name[0] = '\0';
    }
    if (!native_ui_host_normalize(host, values->host, sizeof(values->host))) {
        /* Preserve an invalid in-progress edit in the draft model. Validation keeps it
         * out of saved settings and connection requests. */
        (void)snprintf(values->host, sizeof(values->host), "%s", host ? host : "");
    }
    uint16_t parsed_port = 0;
    (void)snprintf(ui->slot_port_text[slot], UI_PORT_MAX, "%s", port_text ? port_text : "");
    ui->slot_port_valid[slot] = ui_parse_port(port_text, &parsed_port);
    if (ui->slot_port_valid[slot]) {
        values->port = parsed_port;
    }
    (void)snprintf(values->username, sizeof(values->username), "%s", username ? username : "");
    (void)snprintf(values->domain, sizeof(values->domain), "%s", domain ? domain : "");
    (void)snprintf(values->password, sizeof(values->password), "%s", password ? password : "");
    values->fps = ui->selected_fps;
    ui_store_profile_capture_toggles(ui, slot);
    native_ui_mixer_set_profiles(ui->mixer, ui->slot_values);
}

void ui_discard_form_changes(NativePreconnectUi *ui) {
    if (!ui || ui->selected_slot < 0 || ui->selected_slot >= NATIVE_SETTINGS_MAX_SESSIONS) {
        return;
    }
    int slot = ui->selected_slot;
    ui->slot_values[slot] = ui->committed_values[slot];
    (void)snprintf(ui->slot_port_text[slot], UI_PORT_MAX, "%u",
                   (unsigned)ui->slot_values[slot].port);
    ui->slot_port_valid[slot] = true;
    ui->loading_form = true;
    lv_dropdown_set_selected(ui->audio_codec_dropdown,
                             ui->committed_audio_codec == NATIVE_AUDIO_CODEC_PCM ? 1 : 0);
    if (ui->slot_values[slot].camera_redirect) {
        lv_obj_add_state(ui->profile_camera_checkbox, LV_STATE_CHECKED);
    } else {
        lv_obj_clear_state(ui->profile_camera_checkbox, LV_STATE_CHECKED);
    }
    if (ui->slot_values[slot].audio_input_redirect) {
        lv_obj_add_state(ui->profile_audio_input_checkbox, LV_STATE_CHECKED);
    } else {
        lv_obj_clear_state(ui->profile_audio_input_checkbox, LV_STATE_CHECKED);
    }
    ui_recompute_capture_enabled(ui);
    ui->loading_form = false;
    ui->delete_armed = false;
    if (ui->delete_label) {
        lv_label_set_text(ui->delete_label, "Delete profile");
    }
    native_ui_mixer_set_profiles(ui->mixer, ui->slot_values);
}

void ui_load_slot_into_form(NativePreconnectUi *ui, int slot) {
    if (slot < 0 || slot >= NATIVE_SETTINGS_MAX_SESSIONS) {
        return;
    }
    const NativeSessionConfig *values = &ui->slot_values[slot];
    ui->loading_form = true;
    lv_textarea_set_text(ui->name_input, values->name);
    lv_textarea_set_cursor_pos(ui->name_input, LV_TEXTAREA_CURSOR_LAST);
    lv_textarea_set_text(ui->host_input, values->host);
    lv_textarea_set_cursor_pos(ui->host_input, LV_TEXTAREA_CURSOR_LAST);
    lv_textarea_set_text(ui->port_input, ui->slot_port_text[slot]);
    lv_textarea_set_cursor_pos(ui->port_input, LV_TEXTAREA_CURSOR_LAST);
    lv_textarea_set_text(ui->username_input, values->username);
    lv_textarea_set_cursor_pos(ui->username_input, LV_TEXTAREA_CURSOR_LAST);
    lv_textarea_set_text(ui->domain_input, values->domain);
    lv_textarea_set_cursor_pos(ui->domain_input, LV_TEXTAREA_CURSOR_LAST);
    lv_textarea_set_text(ui->password_input, values->password);
    lv_textarea_set_cursor_pos(ui->password_input, LV_TEXTAREA_CURSOR_LAST);
    size_t fps_index = ui_select_fps_index(ui, values->fps);
    ui_set_fps_options(ui);
    ui_set_selected_fps(ui, fps_index);
    if (values->camera_redirect) {
        lv_obj_add_state(ui->profile_camera_checkbox, LV_STATE_CHECKED);
    } else {
        lv_obj_clear_state(ui->profile_camera_checkbox, LV_STATE_CHECKED);
    }
    if (values->audio_input_redirect) {
        lv_obj_add_state(ui->profile_audio_input_checkbox, LV_STATE_CHECKED);
    } else {
        lv_obj_clear_state(ui->profile_audio_input_checkbox, LV_STATE_CHECKED);
    }
    ui->loading_form = false;
    char title[UI_NAME_MAX + 32u];
    char fallback[32];
    const char *name = ui_slot_display_name(ui, slot, fallback, sizeof(fallback));
    (void)snprintf(title, sizeof(title), "%s %s", ui_slot_configured(values) ? "Edit" : "Set up", name);
    lv_label_set_text(ui->form_title, title);
    for (int i = 0; i < NATIVE_SETTINGS_MAX_SESSIONS; i++) {
        lv_obj_set_style_outline_width(ui->color_choices[i], i == slot ? 4 : 0, 0);
        lv_obj_set_style_outline_color(ui->color_choices[i], lv_color_hex(0xeef3fd), 0);
        lv_obj_set_style_outline_opa(ui->color_choices[i], i == slot ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_set_style_outline_pad(ui->color_choices[i], 3, 0);
        lv_obj_set_style_bg_opa(ui->color_choices[i], i == slot ? LV_OPA_COVER : LV_OPA_30, 0);
    }
    if (ui->delete_btn) {
        ui_set_hidden(ui->delete_btn, !(ui_slot_configured(values)));
    }
    ui_update_connect_state(ui);
}

bool native_preconnect_ui_get_slot_values(NativePreconnectUi *ui, int slot, NativeSessionConfig *out) {
    if (!ui || !out || slot < 0 || slot >= NATIVE_SETTINGS_MAX_SESSIONS) {
        return false;
    }
    /* Make sure the on-screen form's latest edits are reflected for its own slot. */
    if (slot == ui->selected_slot) {
        ui_store_form_to_slot(ui, slot);
    }
    if (!ui->slot_port_valid[slot] || !ui_host_valid(ui->slot_values[slot].host)) {
        return false;
    }
    *out = ui->slot_values[slot];
    return true;
}

void native_preconnect_ui_open_setup(NativePreconnectUi *ui, int slot) {
    if (!ui || slot < 0 || slot >= NATIVE_SETTINGS_MAX_SESSIONS ||
        ui->connecting || ui->capture_settings_visible ||
        ui->capture_save_pending) {
        return;
    }
    native_preconnect_ui_select_slot(ui, slot);
    ui_load_slot_into_form(ui, slot);
    ui_set_text(ui->status_label, "");
    ui_show_setup(ui, true);
}

bool native_preconnect_ui_request_connect(NativePreconnectUi *ui, int slot) {
    if (!ui || slot < 0 || slot >= NATIVE_SETTINGS_MAX_SESSIONS ||
        ui->onboarding_visible || ui->capture_settings_visible ||
        ui->capture_save_pending) {
        return false;
    }
    native_preconnect_ui_select_slot(ui, slot);
    if (!ui_slot_configured(&ui->slot_values[slot]) || !ui->slot_port_valid[slot]) {
        native_preconnect_ui_open_setup(ui, slot);
        return false;
    }
    return ui_queue_connect(ui, slot, false);
}

bool native_preconnect_ui_take_connect(NativePreconnectUi *ui, int *slot, char *host, size_t host_cap,
                                       uint16_t *port, char *username, size_t username_cap, char *password,
                                       size_t password_cap, char *domain, size_t domain_cap, uint16_t *fps,
                                       uint16_t *audio_codec, bool *requires_save) {
    if (!ui || !ui->connect_requested) {
        return false;
    }
    ui->connect_requested = false;
    if (slot) {
        *slot = ui->requested_slot;
    }
    if (host && host_cap > 0) {
        size_t len = strlen(ui->requested_host);
        if (len >= host_cap) {
            len = host_cap - 1;
        }
        memcpy(host, ui->requested_host, len);
        host[len] = '\0';
    }
    if (port) {
        *port = ui->requested_port;
    }
    if (fps) {
        *fps = ui->requested_fps;
    }
    if (username && username_cap > 0) {
        size_t len = strlen(ui->requested_username);
        if (len >= username_cap) {
            len = username_cap - 1;
        }
        memcpy(username, ui->requested_username, len);
        username[len] = '\0';
    }
    if (password && password_cap > 0) {
        size_t len = strlen(ui->requested_password);
        if (len >= password_cap) {
            len = password_cap - 1;
        }
        memcpy(password, ui->requested_password, len);
        password[len] = '\0';
    }
    if (domain && domain_cap > 0) {
        size_t len = strlen(ui->requested_domain);
        if (len >= domain_cap) {
            len = domain_cap - 1;
        }
        memcpy(domain, ui->requested_domain, len);
        domain[len] = '\0';
    }
    if (audio_codec) {
        *audio_codec = ui->requested_audio_codec;
    }
    if (requires_save) {
        *requires_save = ui->requested_requires_save;
    }
    return true;
}

bool native_preconnect_ui_take_save(NativePreconnectUi *ui, int *slot, uint16_t *audio_codec) {
    if (!ui || !ui->save_requested) {
        return false;
    }
    ui->save_requested = false;
    if (slot) {
        *slot = ui->saved_slot;
    }
    if (audio_codec) {
        *audio_codec = ui_current_audio_codec(ui);
    }
    return true;
}

bool native_preconnect_ui_take_delete(NativePreconnectUi *ui, int *slot) {
    if (!ui || !ui->delete_requested) {
        return false;
    }
    ui->delete_requested = false;
    if (slot) {
        *slot = ui->deleted_slot;
    }
    return true;
}

void native_preconnect_ui_finish_save(NativePreconnectUi *ui, int slot, bool success, const char *status) {
    if (!ui || slot < 0 || slot >= NATIVE_SETTINGS_MAX_SESSIONS) {
        return;
    }
    ui->save_pending = false;
    if (success) {
        ui->committed_values[slot] = ui->slot_values[slot];
        ui->committed_audio_codec = ui_current_audio_codec(ui);
        ui->committed_camera_enabled = ui->camera_enabled;
        (void)snprintf(ui->committed_camera_device_id,
                       sizeof(ui->committed_camera_device_id), "%s",
                       ui->camera_device_id);
        ui->committed_audio_input_enabled = ui->audio_input_enabled;
        (void)snprintf(ui->committed_audio_input_device_id,
                       sizeof(ui->committed_audio_input_device_id), "%s",
                       ui->audio_input_device_id);
        if (ui->slot_states[slot] != NATIVE_PRECONNECT_SESSION_CONNECTED &&
            ui->slot_states[slot] != NATIVE_PRECONNECT_SESSION_CONNECTING) {
            ui->slot_states[slot] = NATIVE_PRECONNECT_SESSION_OFFLINE;
        }
        ui->slot_details[slot][0] = '\0';
        if (ui->selected_slot == slot) {
            ui_set_text(ui->status_label, status ? status : "Saved on this TV.");
            ui_show_setup(ui, false);
        }
    } else if (ui->selected_slot == slot) {
        ui_set_text(ui->status_label, status ? status : "Could not save settings on this TV.");
        lv_obj_set_style_text_color(ui->status_label, lv_color_hex(0xf0958f), 0);
        ui_show_setup(ui, true);
    }
    native_ui_mixer_set_profiles(ui->mixer, ui->slot_values);
    ui_update_hub(ui);
    ui_update_connect_state(ui);
}

void native_preconnect_ui_finish_connect_save(NativePreconnectUi *ui, int slot, bool success, bool persisted,
                                               const char *status) {
    if (!ui || slot < 0 || slot >= NATIVE_SETTINGS_MAX_SESSIONS) {
        return;
    }
    ui->connect_save_pending = false;
    if (success && persisted) {
        ui->committed_values[slot] = ui->slot_values[slot];
        ui->committed_audio_codec = ui_current_audio_codec(ui);
        ui->committed_camera_enabled = ui->camera_enabled;
        (void)snprintf(ui->committed_camera_device_id,
                       sizeof(ui->committed_camera_device_id), "%s",
                       ui->camera_device_id);
        ui->committed_audio_input_enabled = ui->audio_input_enabled;
        (void)snprintf(ui->committed_audio_input_device_id,
                       sizeof(ui->committed_audio_input_device_id), "%s",
                       ui->audio_input_device_id);
    } else if (!success) {
        ui->connecting = false;
        /* The request never replaced the card's prior runtime state. Restore its
         * complete presentation; the setup status below carries the new rejection. */
        ui->slot_states[slot] = ui->requested_previous_state;
        (void)snprintf(ui->slot_details[slot], UI_DETAIL_MAX, "%s", ui->requested_previous_detail);
        native_preconnect_ui_open_setup(ui, slot);
        ui_set_text(ui->status_label, status ? status : "Could not save settings on this TV.");
        lv_obj_set_style_text_color(ui->status_label, lv_color_hex(0xf0958f), 0);
    }
    ui_update_hub(ui);
    ui_update_connect_state(ui);
}

void native_preconnect_ui_finish_delete(NativePreconnectUi *ui, int slot, bool success, const char *status) {
    if (!ui || slot < 0 || slot >= NATIVE_SETTINGS_MAX_SESSIONS) {
        return;
    }
    ui->delete_pending = false;
    if (success) {
        NativeSessionConfig empty = {0};
        empty.port = 3389;
        empty.fps = 60;
        ui->slot_values[slot] = empty;
        ui->committed_values[slot] = empty;
        ui->committed_camera_enabled = false;
        ui->committed_audio_input_enabled = false;
        for (int i = 0; i < NATIVE_SETTINGS_MAX_SESSIONS; i++) {
            ui->committed_camera_enabled |=
                ui->committed_values[i].camera_redirect;
            ui->committed_audio_input_enabled |=
                ui->committed_values[i].audio_input_redirect;
        }
        ui->camera_enabled = ui->committed_camera_enabled;
        ui->audio_input_enabled = ui->committed_audio_input_enabled;
        (void)snprintf(ui->slot_port_text[slot], UI_PORT_MAX, "%u", (unsigned)empty.port);
        ui->slot_port_valid[slot] = true;
        ui->slot_states[slot] = NATIVE_PRECONNECT_SESSION_NOT_SET_UP;
        ui->slot_details[slot][0] = '\0';
        ui->delete_armed = false;
        native_ui_mixer_set_profiles(ui->mixer, ui->slot_values);
        if (ui->selected_slot == slot) {
            ui_load_slot_into_form(ui, slot);
            ui_set_text(ui->status_label, status ? status : "Profile deleted.");
            ui_show_setup(ui, false);
        }
    } else if (ui->selected_slot == slot) {
        ui_set_text(ui->status_label, status ? status : "Could not delete this profile.");
        lv_obj_set_style_text_color(ui->status_label, lv_color_hex(0xf0958f), 0);
        ui->delete_armed = false;
        lv_label_set_text(ui->delete_label, "Delete profile");
    }
    ui_update_hub(ui);
    ui_update_connect_state(ui);
}

bool native_preconnect_ui_read_current(NativePreconnectUi *ui, char *host, size_t host_cap, uint16_t *port,
                                       char *username, size_t username_cap, char *password, size_t password_cap,
                                       char *domain, size_t domain_cap, uint16_t *fps, uint16_t *audio_codec) {
    if (!ui) {
        return false;
    }

    uint16_t parsed_port = 0;
    const char *current_host = lv_textarea_get_text(ui->host_input);
    char normalized_host[UI_HOST_MAX];
    if (!native_ui_host_normalize(current_host, normalized_host, sizeof(normalized_host)) ||
        !ui_parse_port(lv_textarea_get_text(ui->port_input), &parsed_port)) {
        return false;
    }

    const char *current_username = lv_textarea_get_text(ui->username_input);
    const char *current_password = lv_textarea_get_text(ui->password_input);
    const char *current_domain = lv_textarea_get_text(ui->domain_input);
    if (!current_username || !current_password) {
        return false;
    }

    if (host && host_cap > 0) {
        size_t len = strlen(normalized_host);
        if (len >= host_cap) {
            return false;
        }
        memcpy(host, normalized_host, len + 1);
    }
    if (port) {
        *port = parsed_port;
    }
    if (username && username_cap > 0) {
        size_t len = strlen(current_username);
        if (len >= username_cap) {
            return false;
        }
        memcpy(username, current_username, len + 1);
    }
    if (password && password_cap > 0) {
        size_t len = strlen(current_password);
        if (len >= password_cap) {
            return false;
        }
        memcpy(password, current_password, len + 1);
    }
    if (domain && domain_cap > 0) {
        size_t len = strlen(current_domain ? current_domain : "");
        if (len >= domain_cap) {
            return false;
        }
        memcpy(domain, current_domain ? current_domain : "", len + 1);
    }
    if (fps) {
        *fps = ui->selected_fps;
    }
    if (audio_codec) {
        *audio_codec = ui_current_audio_codec(ui);
    }
    return true;
}

void native_ui_preconnect_input_changed(lv_event_t *event) {
    NativePreconnectUi *ui = (NativePreconnectUi *)lv_event_get_user_data(event);
    if (!ui || ui->loading_form) {
        return;
    }
    ui->delete_armed = false;
    if (ui->delete_label) {
        lv_label_set_text(ui->delete_label, "Delete profile");
    }
    ui_set_text(ui->status_label, "Changes are not saved yet.");
    lv_obj_set_style_text_color(ui->status_label, lv_color_hex(0xaeb6bf), 0);
    ui_update_connect_state(ui);
}

void ui_profile_name_insert(lv_event_t *event) {
    lv_obj_t *input = lv_event_get_target(event);
    const char *inserted = (const char *)lv_event_get_param(event);
    if (!input || !inserted) {
        return;
    }
    /* LVGL routes deletion through LV_EVENT_INSERT with this sentinel. */
    if ((unsigned char)inserted[0] == LV_KEY_DEL && inserted[1] == '\0') {
        return;
    }
    const char *current = lv_textarea_get_text(input);
    size_t current_bytes = current ? strlen(current) : 0u;
    size_t inserted_bytes = strlen(inserted);
    if (!native_ui_profile_name_valid(inserted, UI_NAME_MAX) ||
        inserted_bytes > UI_NAME_MAX - 1u ||
        current_bytes > UI_NAME_MAX - 1u - inserted_bytes) {
        /* NativeSessionConfig.name is byte-sized while LVGL's max_length counts
         * codepoints. Reject the character instead of truncating a later UTF-8 copy. */
        lv_textarea_set_insert_replace(input, "");
    }
}

void ui_fps_changed(lv_event_t *event) {
    NativePreconnectUi *ui = (NativePreconnectUi *)lv_event_get_user_data(event);
    if (!ui || ui->connecting || ui->loading_form) {
        return;
    }
    ui_set_selected_fps(ui, lv_dropdown_get_selected(ui->fps_dropdown));
    ui->delete_armed = false;
    lv_label_set_text(ui->delete_label, "Delete profile");
    ui_set_text(ui->status_label, "Changes are not saved yet.");
    lv_obj_set_style_text_color(ui->status_label, lv_color_hex(0xaeb6bf), 0);
}

void ui_profile_capture_changed(lv_event_t *event) {
    NativePreconnectUi *ui =
        (NativePreconnectUi *)lv_event_get_user_data(event);
    if (!ui || ui->connecting || ui->loading_form) {
        return;
    }
    ui_store_profile_capture_toggles(ui, ui->selected_slot);
    ui->delete_armed = false;
    lv_label_set_text(ui->delete_label, "Delete profile");
    ui_set_text(ui->status_label, "Profile capture choices are not saved yet.");
    lv_obj_set_style_text_color(ui->status_label, lv_color_hex(0xaeb6bf), 0);
}

static bool ui_queue_connect(NativePreconnectUi *ui, int slot, bool force_save) {
    if (!ui || slot < 0 || slot >= NATIVE_SETTINGS_MAX_SESSIONS || ui->hub_closing ||
        ui_any_action_pending(ui) || ui->capture_settings_visible) {
        return false;
    }
    NativeSessionConfig *values = &ui->slot_values[slot];
    if (!ui_slot_configured(values) || !ui->slot_port_valid[slot] || values->port == 0) {
        native_preconnect_ui_open_setup(ui, slot);
        native_preconnect_ui_set_status(ui, "Enter an address, port, username and password.", true);
        return false;
    }
    if (!native_ui_host_normalize(values->host, ui->requested_host,
                                  sizeof(ui->requested_host))) {
        native_preconnect_ui_set_status(ui, "Enter a valid address.", true);
        return false;
    }
    if (strcmp(values->host, ui->requested_host) != 0) {
        /* Profiles saved by the split-field UI regression can still contain [IPv6].
         * Migrate the draft as part of this request: it then differs from the committed
         * value, which makes main persist and use its normalized candidate. */
        (void)snprintf(values->host, sizeof(values->host), "%s", ui->requested_host);
    }
    size_t len = strlen(values->username);
    if (len >= sizeof(ui->requested_username)) {
        native_preconnect_ui_set_status(ui, "Username value is too long.", true);
        return false;
    }
    memcpy(ui->requested_username, values->username, len + 1);
    len = strlen(values->domain);
    if (len >= sizeof(ui->requested_domain)) {
        native_preconnect_ui_set_status(ui, "Domain value is too long.", true);
        return false;
    }
    memcpy(ui->requested_domain, values->domain, len + 1);
    len = strlen(values->password);
    if (len >= sizeof(ui->requested_password)) {
        native_preconnect_ui_set_status(ui, "Password value is too long.", true);
        return false;
    }
    memcpy(ui->requested_password, values->password, len + 1);
    ui->requested_port = values->port;
    ui->requested_fps = values->fps;
    ui->requested_audio_codec = ui_current_audio_codec(ui);
    ui->requested_slot = slot;
    ui->requested_previous_state = ui->slot_states[slot];
    (void)snprintf(ui->requested_previous_detail, UI_DETAIL_MAX, "%s", ui->slot_details[slot]);
    ui->requested_requires_save = force_save || ui_profile_dirty(ui, slot);
    ui->connect_requested = true;
    clog(cLogLevelDebug, "queued %s request for profile slot %d",
         ui->requested_requires_save ? "save-and-connect" : "connect", slot);
    ui->connect_save_pending = true;
    ui->slot_states[slot] = NATIVE_PRECONNECT_SESSION_CONNECTING;
    ui->slot_details[slot][0] = '\0';
    ui_update_hub(ui);
    ui_show_setup(ui, false);
    return true;
}

void ui_connect_clicked(lv_event_t *event) {
    NativePreconnectUi *ui = (NativePreconnectUi *)lv_event_get_user_data(event);
    if (!ui || ui->connecting || !ui_form_valid(ui)) {
        ui_update_connect_state(ui);
        return;
    }
    ui_store_form_to_slot(ui, ui->selected_slot);
    (void)ui_queue_connect(ui, ui->selected_slot, true);
}

void ui_save_clicked(lv_event_t *event) {
    NativePreconnectUi *ui = (NativePreconnectUi *)lv_event_get_user_data(event);
    if (!ui || ui->connecting || !ui_form_valid(ui)) {
        ui_update_connect_state(ui);
        return;
    }
    ui_store_form_to_slot(ui, ui->selected_slot);
    ui->saved_slot = ui->selected_slot;
    ui->save_requested = true;
    ui->save_pending = true;
    ui_set_text(ui->status_label, "Saving...");
    ui_update_connect_state(ui);
}

void ui_cancel_clicked(lv_event_t *event) {
    NativePreconnectUi *ui = (NativePreconnectUi *)lv_event_get_user_data(event);
    if (!ui || ui->connecting || ui_profile_action_pending(ui)) {
        return;
    }
    ui_discard_form_changes(ui);
    ui_load_slot_into_form(ui, ui->selected_slot);
    ui_set_text(ui->status_label, "");
    ui_update_hub(ui);
    ui_show_setup(ui, false);
}

void ui_delete_clicked(lv_event_t *event) {
    NativePreconnectUi *ui = (NativePreconnectUi *)lv_event_get_user_data(event);
    if (!ui || ui->connecting || ui_profile_action_pending(ui) ||
        !ui_slot_configured(&ui->slot_values[ui->selected_slot])) {
        return;
    }
    if (!ui->delete_armed) {
        ui->delete_armed = true;
        lv_label_set_text(ui->delete_label, "Confirm delete");
        ui_set_text(ui->status_label, "Press Confirm delete to remove this profile and its saved password.");
        lv_obj_set_style_text_color(ui->status_label, lv_color_hex(0xf0958f), 0);
        return;
    }
    ui->deleted_slot = ui->selected_slot;
    ui->delete_requested = true;
    ui->delete_pending = true;
    ui_set_text(ui->status_label, "Deleting...");
    ui_update_connect_state(ui);
}

void ui_setup_scrim_clicked(lv_event_t *event) {
    if (lv_event_get_target(event) != lv_event_get_current_target(event)) {
        return;
    }
    ui_cancel_clicked(event);
}

void native_preconnect_ui_set_connecting(NativePreconnectUi *ui, int slot, bool connecting, const char *status) {
    if (!ui || slot < 0 || slot >= NATIVE_SETTINGS_MAX_SESSIONS) {
        return;
    }
    ui->connecting = connecting;
    if (ui->host_input) {
        if (connecting) {
            lv_obj_add_state(ui->name_input, LV_STATE_DISABLED);
            lv_obj_add_state(ui->host_input, LV_STATE_DISABLED);
            lv_obj_add_state(ui->port_input, LV_STATE_DISABLED);
            lv_obj_add_state(ui->username_input, LV_STATE_DISABLED);
            lv_obj_add_state(ui->domain_input, LV_STATE_DISABLED);
            lv_obj_add_state(ui->password_input, LV_STATE_DISABLED);
            lv_obj_add_state(ui->fps_dropdown, LV_STATE_DISABLED);
            lv_obj_add_state(ui->audio_codec_dropdown, LV_STATE_DISABLED);
            lv_obj_add_state(ui->profile_camera_checkbox, LV_STATE_DISABLED);
            lv_obj_add_state(ui->profile_audio_input_checkbox, LV_STATE_DISABLED);
        } else {
            lv_obj_clear_state(ui->name_input, LV_STATE_DISABLED);
            lv_obj_clear_state(ui->host_input, LV_STATE_DISABLED);
            lv_obj_clear_state(ui->port_input, LV_STATE_DISABLED);
            lv_obj_clear_state(ui->username_input, LV_STATE_DISABLED);
            lv_obj_clear_state(ui->domain_input, LV_STATE_DISABLED);
            lv_obj_clear_state(ui->password_input, LV_STATE_DISABLED);
            lv_obj_clear_state(ui->fps_dropdown, LV_STATE_DISABLED);
            lv_obj_clear_state(ui->audio_codec_dropdown, LV_STATE_DISABLED);
            lv_obj_clear_state(ui->profile_camera_checkbox, LV_STATE_DISABLED);
            lv_obj_clear_state(ui->profile_audio_input_checkbox, LV_STATE_DISABLED);
        }
        if (connecting) {
            ui->slot_states[slot] = NATIVE_PRECONNECT_SESSION_CONNECTING;
            (void)snprintf(ui->slot_details[slot], UI_DETAIL_MAX, "%s", status ? status : "");
            ui_show_setup(ui, false);
        } else if (ui->slot_states[slot] == NATIVE_PRECONNECT_SESSION_CONNECTING) {
            ui->slot_states[slot] = ui_slot_configured(&ui->slot_values[slot]) ? NATIVE_PRECONNECT_SESSION_OFFLINE
                                                                               : NATIVE_PRECONNECT_SESSION_NOT_SET_UP;
        }
    }
    native_preconnect_ui_set_status(ui, status, false);
    ui_update_hub(ui);
    ui_update_connect_state(ui);
}

void native_preconnect_ui_set_status(NativePreconnectUi *ui, const char *status, bool error) {
    if (!ui || !ui->status_label) {
        return;
    }
    ui_set_text(ui->status_label, status);
    lv_obj_set_style_text_color(ui->status_label, error ? lv_color_hex(0xf0958f) : lv_color_hex(0xaeb6bf), 0);
}

void ui_edit_clicked(lv_event_t *event) {
    NativePreconnectUi *ui = (NativePreconnectUi *)lv_event_get_user_data(event);
    if (ui && !ui->hub_closing && !ui->connect_save_pending && !ui->save_pending &&
        !ui->delete_pending && !ui->capture_settings_visible &&
        !ui->capture_save_pending) {
        native_preconnect_ui_open_setup(ui, ui->selected_slot);
    }
}

void ui_hero_action_clicked(lv_event_t *event) {
    NativePreconnectUi *ui = (NativePreconnectUi *)lv_event_get_user_data(event);
    if (!ui || ui->hub_closing || ui->connecting || ui_any_action_pending(ui) ||
        ui->capture_settings_visible) {
        return;
    }
    NativePreconnectSessionState state = ui->slot_states[ui->selected_slot];
    if (state == NATIVE_PRECONNECT_SESSION_CONNECTED) {
        ui->activated_slot = ui->selected_slot;
        ui->activate_requested = true;
    } else if (state == NATIVE_PRECONNECT_SESSION_NOT_SET_UP) {
        native_preconnect_ui_open_setup(ui, ui->selected_slot);
    } else if (state != NATIVE_PRECONNECT_SESSION_CONNECTING) {
        (void)ui_queue_connect(ui, ui->selected_slot, false);
    }
}
