#include "ui_preconnect_internal.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rdp_ffi.h"
#include "native_config.h"
#include "ui_host.h"

static const uint16_t UI_FPS_OPTIONS[] = {30, 60};

/* Desktop sizes a profile can request, largest first. A server that mirrors a real
 * monitor ignores the request; one that honours it builds the session at this size. */
static const UiDesktopSize UI_DESKTOP_OPTIONS[] = {
    {3840, 2160},
    {2560, 1440},
    {1920, 1080},
    {1280, 720},
};

size_t ui_desktop_option_count(void) {
    return sizeof(UI_DESKTOP_OPTIONS) / sizeof(UI_DESKTOP_OPTIONS[0]);
}

UiDesktopSize ui_desktop_option(size_t index) {
    if (index >= ui_desktop_option_count()) {
        index = 0;
    }
    return UI_DESKTOP_OPTIONS[index];
}

size_t ui_desktop_option_index(uint16_t width, uint16_t height) {
    for (size_t i = 0; i < ui_desktop_option_count(); i++) {
        if (UI_DESKTOP_OPTIONS[i].width == width && UI_DESKTOP_OPTIONS[i].height == height) {
            return i;
        }
    }
    /* Settings written by hand can hold any size; the form snaps to the nearest listed
     * one by area rather than silently offering a value it cannot represent. */
    size_t nearest = 0;
    uint32_t area = (uint32_t)width * (uint32_t)height;
    uint32_t best = UINT32_MAX;
    for (size_t i = 0; i < ui_desktop_option_count(); i++) {
        uint32_t candidate = (uint32_t)UI_DESKTOP_OPTIONS[i].width * (uint32_t)UI_DESKTOP_OPTIONS[i].height;
        uint32_t distance = candidate > area ? candidate - area : area - candidate;
        if (distance < best) {
            best = distance;
            nearest = i;
        }
    }
    return nearest;
}

void ui_set_selected_desktop(NativePreconnectUi *ui, size_t index) {
    if (!ui) {
        return;
    }
    UiDesktopSize size = ui_desktop_option(index);
    ui->selected_desktop_width = size.width;
    ui->selected_desktop_height = size.height;
    if (ui->desktop_dropdown) {
        lv_dropdown_set_selected(ui->desktop_dropdown, (uint16_t)index);
    }
}

void ui_set_desktop_options(NativePreconnectUi *ui) {
    if (!ui || !ui->desktop_dropdown) {
        return;
    }
    char options[128];
    size_t used = 0;
    for (size_t i = 0; i < ui_desktop_option_count(); i++) {
        int written = snprintf(options + used, sizeof(options) - used, i == 0 ? "%ux%u" : "\n%ux%u",
                               (unsigned)UI_DESKTOP_OPTIONS[i].width, (unsigned)UI_DESKTOP_OPTIONS[i].height);
        if (written <= 0 || (size_t)written >= sizeof(options) - used) {
            break;
        }
        used += (size_t)written;
    }
    lv_dropdown_set_options(ui->desktop_dropdown, options);
}

bool ui_parse_port(const char *text, uint16_t *port) {
    if (!text || !text[0]) {
        return false;
    }
    errno = 0;
    char *end = NULL;
    unsigned long value = strtoul(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value == 0 || value > UINT16_MAX) {
        return false;
    }
    *port = (uint16_t)value;
    return true;
}

bool ui_host_valid(const char *host) {
    char normalized[UI_HOST_MAX];
    return native_ui_host_normalize(host, normalized, sizeof(normalized));
}

size_t ui_fps_option_count(void) {
    return sizeof(UI_FPS_OPTIONS) / sizeof(UI_FPS_OPTIONS[0]);
}

bool ui_find_fps_option(uint16_t fps, size_t *index) {
    for (size_t i = 0; i < ui_fps_option_count(); i++) {
        if (UI_FPS_OPTIONS[i] == fps) {
            if (index) {
                *index = i;
            }
            return true;
        }
    }
    return false;
}

size_t ui_select_fps_index(NativePreconnectUi *ui, uint16_t fps) {
    size_t builtin_index = 0;
    /* Recomputed per load: a stale custom entry from the PREVIOUS slot would otherwise
     * stay in every slot's dropdown and could save that slot's fps into this one. The
     * callers always follow with ui_set_fps_options + ui_set_selected_fps, which keeps
     * the option list and index consistent with this flag. */
    ui->current_fps_option = false;
    if (ui_find_fps_option(fps, &builtin_index)) {
        return builtin_index;
    }
    ui->current_fps_option = true;
    ui->current_fps = fps;
    return 0;
}

void ui_set_selected_fps(NativePreconnectUi *ui, size_t index) {
    if (ui->current_fps_option && index == 0) {
        ui->selected_fps = ui->current_fps;
        if (ui->fps_dropdown) {
            lv_dropdown_set_selected(ui->fps_dropdown, 0);
        }
        return;
    }

    size_t builtin_index = ui->current_fps_option ? index - 1u : index;
    if (builtin_index >= ui_fps_option_count()) {
        builtin_index = 1u;
        index = ui->current_fps_option ? builtin_index + 1u : builtin_index;
    }
    ui->selected_fps = UI_FPS_OPTIONS[builtin_index];
    if (ui->fps_dropdown) {
        lv_dropdown_set_selected(ui->fps_dropdown, (uint16_t)index);
    }
}

void ui_set_fps_options(NativePreconnectUi *ui) {
    char options[48];
    if (ui->current_fps_option) {
        (void)snprintf(options, sizeof(options), "%u FPS (Current)\n%u FPS\n%u FPS", (unsigned)ui->current_fps,
                       (unsigned)UI_FPS_OPTIONS[0], (unsigned)UI_FPS_OPTIONS[1]);
    } else {
        (void)snprintf(options, sizeof(options), "%u FPS\n%u FPS", (unsigned)UI_FPS_OPTIONS[0],
                       (unsigned)UI_FPS_OPTIONS[1]);
    }
    lv_dropdown_set_options(ui->fps_dropdown, options);
}

bool ui_form_valid(NativePreconnectUi *ui) {
    uint16_t port = 0;
    const char *host = lv_textarea_get_text(ui->host_input);
    const char *port_text = lv_textarea_get_text(ui->port_input);
    const char *username = lv_textarea_get_text(ui->username_input);
    const char *password = lv_textarea_get_text(ui->password_input);
    return ui_host_valid(host) && port_text && ui_parse_port(port_text, &port) && username && username[0] && password &&
           password[0];
}

void ui_set_hidden(lv_obj_t *obj, bool hidden) {
    if (hidden) {
        lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_clear_flag(obj, LV_OBJ_FLAG_HIDDEN);
    }
}

void ui_set_disabled(lv_obj_t *obj, bool disabled) {
    if (disabled) {
        lv_obj_add_state(obj, LV_STATE_DISABLED);
    } else {
        lv_obj_clear_state(obj, LV_STATE_DISABLED);
    }
}

/* One of the profile one-shot requests (save / save-and-connect / delete) is
 * armed or persisting; navigation and further requests must wait. */
bool ui_profile_action_pending(const NativePreconnectUi *ui) {
    return ui->save_pending || ui->connect_save_pending || ui->delete_pending;
}

/* Any one-shot request at all, including the capture-settings save. */
bool ui_any_action_pending(const NativePreconnectUi *ui) {
    return ui_profile_action_pending(ui) || ui->capture_save_pending;
}

/* A modal drawer owns the screen (setup form, capture settings, onboarding). */
bool ui_drawer_open(const NativePreconnectUi *ui) {
    return ui->setup_visible || ui->capture_settings_visible || ui->onboarding_visible;
}

void ui_update_connect_state(NativePreconnectUi *ui) {
    if (!ui || !ui->connect_btn || !ui->save_btn) {
        return;
    }
    bool action_pending = ui->connecting || ui_profile_action_pending(ui);
    bool slot_connecting = ui->selected_slot >= 0 && ui->selected_slot < NATIVE_SETTINGS_MAX_SESSIONS &&
                           native_ui_session_connecting(ui->slot_states[ui->selected_slot]);
    bool disabled = action_pending || slot_connecting || !ui_form_valid(ui);
    if (disabled) {
        lv_obj_add_state(ui->connect_btn, LV_STATE_DISABLED);
        lv_obj_add_state(ui->save_btn, LV_STATE_DISABLED);
    } else {
        lv_obj_clear_state(ui->connect_btn, LV_STATE_DISABLED);
        lv_obj_clear_state(ui->save_btn, LV_STATE_DISABLED);
    }
    if (ui->delete_btn) {
        ui_set_disabled(ui->delete_btn, action_pending || slot_connecting);
    }
    if (ui->cancel_btn) {
        ui_set_disabled(ui->cancel_btn, action_pending);
    }
}

void ui_set_text(lv_obj_t *label, const char *text) {
    lv_label_set_text(label, text && text[0] ? text : "");
}

/* Dropdown option order must match NATIVE_AUDIO_CODEC_AUTO (0) and NATIVE_AUDIO_CODEC_PCM (1). */
uint16_t ui_current_audio_codec(const NativePreconnectUi *ui) {
    if (!ui->audio_codec_dropdown) {
        return NATIVE_AUDIO_CODEC_AUTO;
    }
    return lv_dropdown_get_selected(ui->audio_codec_dropdown) == 1 ? NATIVE_AUDIO_CODEC_PCM : NATIVE_AUDIO_CODEC_AUTO;
}

void ui_recompute_capture_enabled(NativePreconnectUi *ui) {
    ui->camera_enabled = false;
    ui->audio_input_enabled = false;
    for (int i = 0; i < NATIVE_SETTINGS_MAX_SESSIONS; i++) {
        ui->camera_enabled |= ui->slot_values[i].camera_redirect;
        ui->audio_input_enabled |= ui->slot_values[i].audio_input_redirect;
    }
}

void ui_store_profile_capture_toggles(NativePreconnectUi *ui, int slot) {
    if (!ui || slot < 0 || slot >= NATIVE_SETTINGS_MAX_SESSIONS || !ui->profile_camera_checkbox ||
        !ui->profile_audio_input_checkbox) {
        return;
    }
    ui->slot_values[slot].camera_redirect = lv_obj_has_state(ui->profile_camera_checkbox, LV_STATE_CHECKED);
    ui->slot_values[slot].audio_input_redirect = lv_obj_has_state(ui->profile_audio_input_checkbox, LV_STATE_CHECKED);
    ui_recompute_capture_enabled(ui);
}

bool ui_profile_dirty(const NativePreconnectUi *ui, int slot) {
    const NativeSessionConfig *draft = &ui->slot_values[slot];
    const NativeSessionConfig *saved = &ui->committed_values[slot];
    return strcmp(draft->name, saved->name) != 0 || native_session_connection_config_changed(saved, draft) ||
           ui_current_audio_codec(ui) != ui->committed_audio_codec;
}
