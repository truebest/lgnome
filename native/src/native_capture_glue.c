/* Fans the capture (camera/microphone) settings out to the redirect workers
 * and every session slot. The applied-state recorder is needed by every build;
 * the settings fan-out only exists with SDL (the interactive builds). */
#include "native_capture_glue.h"

#include <stdio.h>
#include <string.h>

#include "native_app.h"

#include "clog.h"

clog_define(g_native_log_capture_glue, cLogLevelInfo, cLogFlags_Default, "native", NULL);

/* Records the applied device/format config (deep-copying the id strings into
 * App storage) so the next settings save can tell a device change from a
 * per-profile toggle. */
void native_capture_record_applied(App *app, const NativeCaptureRedirectConfig *config) {
    app->capture_applied = *config;
    (void)snprintf(app->capture_applied_camera_id, sizeof(app->capture_applied_camera_id), "%s",
                   config->camera_device_id);
    (void)snprintf(app->capture_applied_audio_id, sizeof(app->capture_applied_audio_id), "%s",
                   config->audio_input_device_id);
    app->capture_applied.camera_device_id = app->capture_applied_camera_id;
    app->capture_applied.audio_input_device_id = app->capture_applied_audio_id;
    app->capture_applied_valid = true;
}

void native_capture_pause_for_active_slot_change(App *app) {
    if (app) {
        native_capture_redirect_set_foreground_slot(&app->capture_redirect, -1);
    }
}

void native_capture_follow_active_slot(App *app) {
    if (app) {
        native_capture_redirect_set_foreground_slot(
            &app->capture_redirect, atomic_load(&app->active_index));
    }
}

#if defined(HELLOLG_TARGET_WEBOS) || defined(HELLOLG_CAPTURE_REDIRECT_TESTING)
void native_reconfigure_capture_redirect(App *app) {
    if (!app || !app->settings || app->camera_preview) {
        return;
    }
    NativeSettings *settings = app->settings;
    NativeCaptureRedirectConfig config = {
        .camera_enabled = settings->camera_enabled,
        .camera_device_id = settings->camera_device_id,
        .camera_width = settings->camera_width,
        .camera_height = settings->camera_height,
        .camera_fps = settings->camera_fps,
        .audio_input_enabled = settings->audio_input_enabled,
        .audio_input_device_id = settings->audio_input_device_id,
        .audio_input_gain_db = settings->audio_input_gain_db,
    };
    /* Only the shared devices and their format need a worker restart. Doing it for a
     * per-profile toggle would pull the camera out from under a profile that is
     * streaming right now, so those changes ride on set_slot alone. */
    bool devices_changed =
        !app->capture_applied_valid ||
        app->capture_applied.camera_enabled != config.camera_enabled ||
        app->capture_applied.camera_width != config.camera_width ||
        app->capture_applied.camera_height != config.camera_height ||
        app->capture_applied.camera_fps != config.camera_fps ||
        app->capture_applied.audio_input_enabled != config.audio_input_enabled ||
        app->capture_applied.audio_input_gain_db != config.audio_input_gain_db ||
        strcmp(app->capture_applied_camera_id, config.camera_device_id) != 0 ||
        strcmp(app->capture_applied_audio_id, config.audio_input_device_id) != 0;

    if (devices_changed) {
        bool configured = app->capture_redirect.impl
                              ? native_capture_redirect_reconfigure(
                                    &app->capture_redirect, &config)
                              : native_capture_redirect_init(
                                    &app->capture_redirect, &config);
        if (configured) {
            native_capture_record_applied(app, &config);
        } else if (!app->capture_redirect.impl) {
            clog(cLogLevelWarning,
                 "failed to apply camera/microphone settings; capture remains disabled");
            return;
        } else {
            /* Partial worker-start failure: surviving workers keep running, so
             * the per-slot permissions below MUST still be published or a save
             * that also revoked a profile's camera/microphone would leave the
             * old grant live. Applied state stays unrecorded so the next save
             * retries the device reconfigure. */
            clog(cLogLevelWarning,
                 "camera/microphone settings only partially applied; retrying on the next save");
        }
    }
    for (int i = 0; i < NATIVE_SETTINGS_MAX_SESSIONS; i++) {
        NativeSessionSlot *slot = &app->sessions[i];
        native_capture_redirect_set_slot(
            &app->capture_redirect, i, slot->rdp,
            settings->camera_enabled &&
                settings->sessions[i].camera_redirect,
            settings->audio_input_enabled &&
                settings->sessions[i].audio_input_redirect);
    }
    /* A failed startup allocation/mutex init leaves no manager for the earlier
     * native_duck_retarget call. A later successful settings-save init starts paused,
     * so restore the current owner after all session handles have been published. */
    native_capture_follow_active_slot(app);
}

#endif
