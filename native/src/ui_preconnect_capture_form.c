#include "ui_preconnect_internal.h"

#include <stdio.h>
#include <string.h>

#include "clog.h"

clog_define(g_native_log_ui_capture_form, cLogLevelInfo, "ui.capture.form");

static const int16_t UI_AUDIO_INPUT_GAIN_OPTIONS[] = {-12, -6, 0, 6, 12, 18};

void ui_sanitize_option(char *text) {
    if (!text) {
        return;
    }
    for (; *text; text++) {
        if (*text == '\n' || *text == '\r') {
            *text = ' ';
        }
    }
}

void ui_enumerate_capture_devices(NativePreconnectUi *ui) {
    size_t camera_count = native_camera_enumerate(ui->camera_devices, UI_CAPTURE_DEVICE_OPTIONS);
    ui->camera_device_count = camera_count < UI_CAPTURE_DEVICE_OPTIONS ? camera_count : UI_CAPTURE_DEVICE_OPTIONS;
    bool saved_camera_found = ui->camera_device_id[0] == '\0';
    for (size_t i = 0; i < ui->camera_device_count; i++) {
        ui_sanitize_option(ui->camera_devices[i].name);
        if (strcmp(ui->camera_devices[i].id, ui->camera_device_id) == 0) {
            saved_camera_found = true;
        }
    }
    if (!saved_camera_found && UI_CAPTURE_DEVICE_OPTIONS > 0u) {
        size_t index = ui->camera_device_count < UI_CAPTURE_DEVICE_OPTIONS ? ui->camera_device_count++
                                                                           : UI_CAPTURE_DEVICE_OPTIONS - 1u;
        NativeCameraDeviceInfo *device = &ui->camera_devices[index];
        (void)snprintf(device->id, sizeof(device->id), "%s", ui->camera_device_id);
        (void)snprintf(device->name, sizeof(device->name), "Saved camera (offline)");
        device->path[0] = '\0';
    }

    size_t audio_count = native_audio_input_enumerate(ui->audio_input_devices, UI_CAPTURE_DEVICE_OPTIONS);
    ui->audio_input_device_count = audio_count < UI_CAPTURE_DEVICE_OPTIONS ? audio_count : UI_CAPTURE_DEVICE_OPTIONS;
    bool saved_audio_found = ui->audio_input_device_id[0] == '\0';
    for (size_t i = 0; i < ui->audio_input_device_count; i++) {
        ui_sanitize_option(ui->audio_input_devices[i].name);
        if (strcmp(ui->audio_input_devices[i].stable_id, ui->audio_input_device_id) == 0) {
            saved_audio_found = true;
        }
    }
    if (!saved_audio_found && UI_CAPTURE_DEVICE_OPTIONS > 0u) {
        size_t index = ui->audio_input_device_count < UI_CAPTURE_DEVICE_OPTIONS ? ui->audio_input_device_count++
                                                                                : UI_CAPTURE_DEVICE_OPTIONS - 1u;
        NativeAudioInputDeviceInfo *device = &ui->audio_input_devices[index];
        (void)snprintf(device->stable_id, sizeof(device->stable_id), "%s", ui->audio_input_device_id);
        (void)snprintf(device->name, sizeof(device->name), "Saved microphone (offline)");
    }

    clog(cLogLevelDebug, "capture enumeration returned %zu camera and %zu audio-input devices", camera_count,
         audio_count);
}

void ui_set_capture_options(NativePreconnectUi *ui) {
    char camera_options[1536] = "Auto";
    char audio_options[1536] = "Auto";
    for (size_t i = 0; i < ui->camera_device_count; i++) {
        size_t used = strlen(camera_options);
        (void)snprintf(camera_options + used, sizeof(camera_options) - used, "\n%s", ui->camera_devices[i].name);
    }
    for (size_t i = 0; i < ui->audio_input_device_count; i++) {
        size_t used = strlen(audio_options);
        (void)snprintf(audio_options + used, sizeof(audio_options) - used, "\n%s", ui->audio_input_devices[i].name);
    }
    lv_dropdown_set_options(ui->camera_input_dropdown, camera_options);
    lv_dropdown_set_options(ui->audio_input_dropdown, audio_options);
}

uint16_t ui_capture_selected_index(const char *device_id, const char *const *ids, size_t count) {
    if (!device_id || !device_id[0]) {
        return 0;
    }
    for (size_t i = 0; i < count; i++) {
        if (strcmp(ids[i], device_id) == 0) {
            return (uint16_t)(i + 1u);
        }
    }
    return 0;
}

uint16_t ui_camera_selected_index(const NativePreconnectUi *ui) {
    const char *ids[UI_CAPTURE_DEVICE_OPTIONS];
    for (size_t i = 0; i < ui->camera_device_count; i++) {
        ids[i] = ui->camera_devices[i].id;
    }
    return ui_capture_selected_index(ui->camera_device_id, ids, ui->camera_device_count);
}

uint16_t ui_audio_input_selected_index(const NativePreconnectUi *ui) {
    const char *ids[UI_CAPTURE_DEVICE_OPTIONS];
    for (size_t i = 0; i < ui->audio_input_device_count; i++) {
        ids[i] = ui->audio_input_devices[i].stable_id;
    }
    return ui_capture_selected_index(ui->audio_input_device_id, ids, ui->audio_input_device_count);
}

/* Offer the sizes the selected camera reports for native H.264, keeping only those
 * that also carry at least one usable frame rate: a size the rate query cannot
 * answer for is one the capture open would refuse. */
void ui_refresh_camera_resolution_options(NativePreconnectUi *ui) {
    ui->camera_current_resolution_available = native_camera_h264_mode_available(
        ui->camera_device_id, ui->camera_width, ui->camera_height, ui->camera_fps);
    ui->camera_resolution_option_count = 0u;
    uint16_t sizes[NATIVE_CAMERA_FRAME_SIZE_MAX][2];
    size_t size_count = 0u;
    if (native_camera_enumerate_frame_sizes(ui->camera_device_id, sizes, NATIVE_CAMERA_FRAME_SIZE_MAX, &size_count) !=
        NATIVE_CAMERA_RATES_LISTED) {
        size_count = 0u;
    }
    for (size_t i = 0; i < size_count && ui->camera_resolution_option_count < UI_CAMERA_RESOLUTION_OPTION_MAX; i++) {
        uint16_t rates[NATIVE_CAMERA_FRAME_RATE_MAX];
        size_t rate_count = 0u;
        NativeCameraRateQuery query = native_camera_enumerate_frame_rates(
            ui->camera_device_id, sizes[i][0], sizes[i][1], rates, NATIVE_CAMERA_FRAME_RATE_MAX, &rate_count);
        if (query != NATIVE_CAMERA_RATES_LISTED || rate_count == 0u) {
            continue;
        }
        ui->camera_resolution_options[ui->camera_resolution_option_count][0] = sizes[i][0];
        ui->camera_resolution_options[ui->camera_resolution_option_count][1] = sizes[i][1];
        ui->camera_resolution_option_count++;
    }
    if (ui->camera_resolution_option_count == 0u) {
        return;
    }

    /* A size kept only as the drawer's "(Current)" entry must still be one this
     * camera has at the saved rate; after a device change it may not be. A valid
     * off-list pair such as 960x540@12 must not be replaced, and merely opening or
     * cancelling the drawer must not rewrite the stored mode. The device list is
     * sorted largest first, so falling back to the tail keeps the replacement the
     * cheapest mode on the wire rather than silently jumping to 1080p. */
    if (!ui->camera_current_resolution_available) {
        size_t smallest = ui->camera_resolution_option_count - 1u;
        ui->camera_width = ui->camera_resolution_options[smallest][0];
        ui->camera_height = ui->camera_resolution_options[smallest][1];
    }
}

void ui_set_camera_resolution_options(NativePreconnectUi *ui) {
    if (ui->camera_resolution_option_count == 0u) {
        if (ui->camera_current_resolution_available) {
            char current[48];
            (void)snprintf(current, sizeof(current), "%ux%u (Current)", (unsigned)ui->camera_width,
                           (unsigned)ui->camera_height);
            ui->current_camera_resolution_option = true;
            lv_dropdown_set_options(ui->camera_resolution_dropdown, current);
            lv_obj_clear_state(ui->camera_resolution_dropdown, LV_STATE_DISABLED);
        } else {
            ui->current_camera_resolution_option = false;
            lv_dropdown_set_options(ui->camera_resolution_dropdown, "No native H.264 modes");
            lv_obj_add_state(ui->camera_resolution_dropdown, LV_STATE_DISABLED);
        }
        lv_dropdown_set_selected(ui->camera_resolution_dropdown, 0u);
        return;
    }
    lv_obj_clear_state(ui->camera_resolution_dropdown, LV_STATE_DISABLED);
    /* Widest entry is "1920x1080 (Current)\n"; size for every option plus that one. */
    char options[UI_CAMERA_RESOLUTION_OPTION_MAX * 12u + 24u] = "";
    size_t selected = 0u;
    ui->current_camera_resolution_option = true;
    for (size_t i = 0; i < ui->camera_resolution_option_count; i++) {
        if (ui->camera_resolution_options[i][0] == ui->camera_width &&
            ui->camera_resolution_options[i][1] == ui->camera_height) {
            ui->current_camera_resolution_option = false;
            selected = i;
            break;
        }
    }
    if (ui->current_camera_resolution_option) {
        (void)snprintf(options, sizeof(options), "%ux%u (Current)", (unsigned)ui->camera_width,
                       (unsigned)ui->camera_height);
    }
    for (size_t i = 0; i < ui->camera_resolution_option_count; i++) {
        size_t used = strlen(options);
        (void)snprintf(options + used, sizeof(options) - used, "%s%ux%u", used ? "\n" : "",
                       (unsigned)ui->camera_resolution_options[i][0], (unsigned)ui->camera_resolution_options[i][1]);
    }
    lv_dropdown_set_options(ui->camera_resolution_dropdown, options);
    lv_dropdown_set_selected(ui->camera_resolution_dropdown,
                             (uint16_t)(ui->current_camera_resolution_option ? 0u : selected));
}

/* Offers exactly the native-H.264 intervals the device reports for the selected
 * size. An unavailable/unqueryable device produces no choices. */
void ui_refresh_camera_fps_options(NativePreconnectUi *ui) {
    uint16_t device_rates[NATIVE_CAMERA_FRAME_RATE_MAX];
    size_t device_count = 0u;
    NativeCameraRateQuery query = native_camera_enumerate_frame_rates(ui->camera_device_id, ui->camera_width,
                                                                      ui->camera_height, device_rates,
                                                                      NATIVE_CAMERA_FRAME_RATE_MAX, &device_count);
    ui->camera_fps_option_count = 0u;
    ui->camera_current_fps_available =
        query == NATIVE_CAMERA_RATES_LISTED &&
        native_camera_frame_rate_list_contains(device_rates, device_count, ui->camera_fps);
    if (query != NATIVE_CAMERA_RATES_LISTED || device_count == 0u) {
        return;
    }
    for (size_t i = 0; i < device_count && ui->camera_fps_option_count < UI_CAMERA_FPS_OPTION_MAX; i++) {
        ui->camera_fps_options[ui->camera_fps_option_count++] = device_rates[i];
    }
    if (ui->camera_fps_option_count == 0u) {
        return;
    }
    /* A custom but device-supported rate stays as the drawer's "(Current)" entry;
     * only a rate this camera cannot deliver is replaced. The device list is sorted
     * largest first, so the smoothest rate is the head entry. */
    if (ui->camera_current_fps_available) {
        return;
    }
    ui->camera_fps = ui->camera_fps_options[0];
}

void ui_set_camera_fps_options(NativePreconnectUi *ui) {
    if (ui->camera_fps_option_count == 0u) {
        if (ui->camera_current_fps_available) {
            char current[48];
            (void)snprintf(current, sizeof(current), "%u FPS (Current)", (unsigned)ui->camera_fps);
            ui->current_camera_fps_option = true;
            lv_dropdown_set_options(ui->camera_fps_dropdown, current);
            lv_obj_clear_state(ui->camera_fps_dropdown, LV_STATE_DISABLED);
        } else {
            ui->current_camera_fps_option = false;
            lv_dropdown_set_options(ui->camera_fps_dropdown, "No native H.264 rates");
            lv_obj_add_state(ui->camera_fps_dropdown, LV_STATE_DISABLED);
        }
        lv_dropdown_set_selected(ui->camera_fps_dropdown, 0u);
        return;
    }
    lv_obj_clear_state(ui->camera_fps_dropdown, LV_STATE_DISABLED);
    /* Widest entry is "30 FPS (Current)\n"; size for every option plus that one. */
    char options[UI_CAMERA_FPS_OPTION_MAX * 8u + 20u] = "";
    size_t selected = 0u;
    ui->current_camera_fps_option = true;
    for (size_t i = 0; i < ui->camera_fps_option_count; i++) {
        if (ui->camera_fps_options[i] == ui->camera_fps) {
            ui->current_camera_fps_option = false;
            selected = i;
            break;
        }
    }
    if (ui->current_camera_fps_option) {
        (void)snprintf(options, sizeof(options), "%u FPS (Current)", (unsigned)ui->camera_fps);
    }
    for (size_t i = 0; i < ui->camera_fps_option_count; i++) {
        size_t used = strlen(options);
        (void)snprintf(options + used, sizeof(options) - used, "%s%u FPS", used ? "\n" : "",
                       (unsigned)ui->camera_fps_options[i]);
    }
    lv_dropdown_set_options(ui->camera_fps_dropdown, options);
    lv_dropdown_set_selected(ui->camera_fps_dropdown, (uint16_t)(ui->current_camera_fps_option ? 0u : selected));
}

void ui_set_audio_input_gain_options(NativePreconnectUi *ui) {
    char options[160] = "";
    size_t selected = 0u;
    ui->current_audio_input_gain_option = true;
    for (size_t i = 0; i < sizeof(UI_AUDIO_INPUT_GAIN_OPTIONS) / sizeof(UI_AUDIO_INPUT_GAIN_OPTIONS[0]); i++) {
        if (UI_AUDIO_INPUT_GAIN_OPTIONS[i] == ui->audio_input_gain_db) {
            ui->current_audio_input_gain_option = false;
            selected = i;
            break;
        }
    }
    if (ui->current_audio_input_gain_option) {
        (void)snprintf(options, sizeof(options), "%+d dB (Current)", (int)ui->audio_input_gain_db);
    }
    for (size_t i = 0; i < sizeof(UI_AUDIO_INPUT_GAIN_OPTIONS) / sizeof(UI_AUDIO_INPUT_GAIN_OPTIONS[0]); i++) {
        size_t used = strlen(options);
        (void)snprintf(options + used, sizeof(options) - used, "%s%+d dB", used ? "\n" : "",
                       (int)UI_AUDIO_INPUT_GAIN_OPTIONS[i]);
    }
    lv_dropdown_set_options(ui->audio_input_gain_dropdown, options);
    lv_dropdown_set_selected(ui->audio_input_gain_dropdown,
                             (uint16_t)(ui->current_audio_input_gain_option ? 0u : selected));
}

void ui_load_capture_settings_form(NativePreconnectUi *ui) {
    if (!ui || !ui->camera_input_dropdown) {
        return;
    }
    ui->loading_form = true;
    lv_dropdown_set_selected(ui->camera_input_dropdown, ui_camera_selected_index(ui));
    lv_dropdown_set_selected(ui->audio_input_dropdown, ui_audio_input_selected_index(ui));
    ui_refresh_camera_resolution_options(ui);
    ui_set_camera_resolution_options(ui);
    ui_refresh_camera_fps_options(ui);
    ui_set_camera_fps_options(ui);
    ui_set_audio_input_gain_options(ui);
    ui->loading_form = false;
}

void ui_restore_capture_settings(NativePreconnectUi *ui) {
    if (!ui) {
        return;
    }
    (void)snprintf(ui->camera_device_id, sizeof(ui->camera_device_id), "%s", ui->committed_camera_device_id);
    ui->camera_width = ui->committed_camera_width;
    ui->camera_height = ui->committed_camera_height;
    ui->camera_fps = ui->committed_camera_fps;
    (void)snprintf(ui->audio_input_device_id, sizeof(ui->audio_input_device_id), "%s",
                   ui->committed_audio_input_device_id);
    ui->audio_input_gain_db = ui->committed_audio_input_gain_db;
    ui_load_capture_settings_form(ui);
}

void ui_commit_capture_settings(NativePreconnectUi *ui) {
    if (!ui) {
        return;
    }
    (void)snprintf(ui->committed_camera_device_id, sizeof(ui->committed_camera_device_id), "%s", ui->camera_device_id);
    ui->committed_camera_width = ui->camera_width;
    ui->committed_camera_height = ui->camera_height;
    ui->committed_camera_fps = ui->camera_fps;
    (void)snprintf(ui->committed_audio_input_device_id, sizeof(ui->committed_audio_input_device_id), "%s",
                   ui->audio_input_device_id);
    ui->committed_audio_input_gain_db = ui->audio_input_gain_db;
}

void ui_store_capture_settings_form(NativePreconnectUi *ui) {
    if (!ui || !ui->camera_input_dropdown) {
        return;
    }
    uint16_t camera = lv_dropdown_get_selected(ui->camera_input_dropdown);
    uint16_t audio = lv_dropdown_get_selected(ui->audio_input_dropdown);
    if (camera == 0u) {
        ui->camera_device_id[0] = '\0';
    } else if ((size_t)(camera - 1u) < ui->camera_device_count) {
        (void)snprintf(ui->camera_device_id, sizeof(ui->camera_device_id), "%s", ui->camera_devices[camera - 1u].id);
    }
    if (audio == 0u) {
        ui->audio_input_device_id[0] = '\0';
    } else if ((size_t)(audio - 1u) < ui->audio_input_device_count) {
        (void)snprintf(ui->audio_input_device_id, sizeof(ui->audio_input_device_id), "%s",
                       ui->audio_input_devices[audio - 1u].stable_id);
    }

    uint16_t selected = lv_dropdown_get_selected(ui->camera_resolution_dropdown);
    if (!ui->current_camera_resolution_option || selected > 0u) {
        size_t index = selected - (ui->current_camera_resolution_option ? 1u : 0u);
        if (index < ui->camera_resolution_option_count) {
            ui->camera_width = ui->camera_resolution_options[index][0];
            ui->camera_height = ui->camera_resolution_options[index][1];
        }
    }
    selected = lv_dropdown_get_selected(ui->camera_fps_dropdown);
    if (!ui->current_camera_fps_option || selected > 0u) {
        size_t index = selected - (ui->current_camera_fps_option ? 1u : 0u);
        if (index < ui->camera_fps_option_count) {
            ui->camera_fps = ui->camera_fps_options[index];
        }
    }
    selected = lv_dropdown_get_selected(ui->audio_input_gain_dropdown);
    if (!ui->current_audio_input_gain_option || selected > 0u) {
        size_t index = selected - (ui->current_audio_input_gain_option ? 1u : 0u);
        if (index < sizeof(UI_AUDIO_INPUT_GAIN_OPTIONS) / sizeof(UI_AUDIO_INPUT_GAIN_OPTIONS[0])) {
            ui->audio_input_gain_db = UI_AUDIO_INPUT_GAIN_OPTIONS[index];
        }
    }
}
