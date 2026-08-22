/* Shared camera/microphone drawer draft, save, and cancel behavior. */
#include "ui_preconnect_internal.h"

#include <stdio.h>

#include "clog.h"

clog_define(g_native_log_ui_capture, cLogLevelInfo, cLogFlags_Default, "ui.capture", NULL);

bool native_preconnect_ui_get_capture_values(NativePreconnectUi *ui,
                                             NativeSettings *settings) {
    if (!ui || !settings) {
        return false;
    }
    ui_recompute_capture_enabled(ui);
    /* The drawer's draft: this getter exists for the camera drawer's own Save, which is
     * the user confirming exactly what the drawer shows. Profile saves must not call it
     * — opening or cancelling the drawer adjusts the draft to what the selected device
     * can do, and those adjustments are not the user's choice. */
    settings->camera_enabled = ui->camera_enabled;
    (void)snprintf(settings->camera_device_id,
                   sizeof(settings->camera_device_id), "%s",
                   ui->camera_device_id);
    settings->camera_width = ui->camera_width;
    settings->camera_height = ui->camera_height;
    settings->camera_fps = ui->camera_fps;
    settings->audio_input_enabled = ui->audio_input_enabled;
    (void)snprintf(settings->audio_input_device_id,
                   sizeof(settings->audio_input_device_id), "%s",
                   ui->audio_input_device_id);
    settings->audio_input_gain_db = ui->audio_input_gain_db;
    return true;
}

bool native_preconnect_ui_take_capture_save(NativePreconnectUi *ui) {
    if (!ui || !ui->capture_save_requested) {
        return false;
    }
    ui->capture_save_requested = false;
    return true;
}

void native_preconnect_ui_finish_capture_save(NativePreconnectUi *ui,
                                              bool success,
                                              const char *status) {
    if (!ui) {
        return;
    }
    ui->capture_save_pending = false;
    if (success) {
        ui_commit_capture_settings(ui);
        ui_set_text(ui->capture_status_label,
                    status ? status : "Capture settings saved.");
        ui_show_capture_settings(ui, false);
    } else {
        ui_set_text(ui->capture_status_label,
                    status ? status
                           : "Could not save capture settings on this TV.");
        lv_obj_set_style_text_color(ui->capture_status_label,
                                    lv_color_hex(0xf0958f), 0);
        lv_obj_clear_state(ui->capture_save_btn, LV_STATE_DISABLED);
        lv_obj_clear_state(ui->capture_cancel_btn, LV_STATE_DISABLED);
        native_ui_preconnect_screen_refocus(ui);
    }
}

void ui_capture_settings_changed(lv_event_t *event) {
    NativePreconnectUi *ui =
        (NativePreconnectUi *)lv_event_get_user_data(event);
    if (!ui || ui->loading_form || ui->capture_save_pending) {
        return;
    }
    ui_store_capture_settings_form(ui);
    /* A new size or camera can change which sizes and rates exist, so rebuild
     * both lists before the user can pick a combination the device cannot open. */
    bool camera_changed = lv_event_get_target(event) == ui->camera_input_dropdown;
    bool size_changed = lv_event_get_target(event) == ui->camera_resolution_dropdown;
    if (camera_changed) {
        ui_refresh_camera_resolution_options(ui);
    }
    if (camera_changed || size_changed) {
        ui_refresh_camera_fps_options(ui);
        ui->loading_form = true;
        if (camera_changed) {
            ui_set_camera_resolution_options(ui);
        }
        ui_set_camera_fps_options(ui);
        ui->loading_form = false;
        /* Rebuilding may have preselected a different entry than the one stored, so
         * take the visible state as the new draft. */
        ui_store_capture_settings_form(ui);
    }
    ui_set_text(ui->capture_status_label, "Changes are not saved yet.");
    lv_obj_set_style_text_color(ui->capture_status_label,
                                lv_color_hex(0xaeb6bf), 0);
}

void ui_capture_scrim_clicked(lv_event_t *event) {
    if (lv_event_get_target(event) != lv_event_get_current_target(event)) {
        return;
    }
    ui_capture_cancel_clicked(event);
}

void ui_capture_settings_clicked(lv_event_t *event) {
    NativePreconnectUi *ui =
        (NativePreconnectUi *)lv_event_get_user_data(event);
    if (!ui || ui->hub_closing || ui->connecting || ui_any_action_pending(ui) || ui->setup_visible ||
        ui->onboarding_visible) {
        return;
    }
    ui_enumerate_capture_devices(ui);
    ui_set_capture_options(ui);
    ui_show_capture_settings(ui, true);
}

void ui_capture_save_clicked(lv_event_t *event) {
    NativePreconnectUi *ui =
        (NativePreconnectUi *)lv_event_get_user_data(event);
    if (!ui || !ui->capture_settings_visible || ui->capture_save_pending) {
        return;
    }
    ui_store_capture_settings_form(ui);
    ui->capture_save_requested = true;
    ui->capture_save_pending = true;
    clog(cLogLevelDebug, "queued capture settings save");
    ui_set_text(ui->capture_status_label, "Saving...");
    lv_obj_set_style_text_color(ui->capture_status_label,
                                lv_color_hex(0xaeb6bf), 0);
    lv_obj_add_state(ui->capture_save_btn, LV_STATE_DISABLED);
    lv_obj_add_state(ui->capture_cancel_btn, LV_STATE_DISABLED);
}

void ui_capture_cancel_clicked(lv_event_t *event) {
    NativePreconnectUi *ui =
        (NativePreconnectUi *)lv_event_get_user_data(event);
    if (!ui || ui->capture_save_pending) {
        return;
    }
    ui_restore_capture_settings(ui);
    ui_set_text(ui->capture_status_label, "");
    ui_show_capture_settings(ui, false);
}
