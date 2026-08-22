/* Capture-device settings and profile save/delete transactions. Requests remain
 * ordered capture-save, profile-save, profile-delete exactly as emitted by the UI. */
#ifdef HELLOLG_TARGET_WEBOS

#include "native_hub_actions_internal.h"

#include <pthread.h>
#include <stdio.h>

#include "native_app.h"
#include "native_capture_glue.h"
#include "native_config.h"
#include "native_config_storage.h"
#include "native_session.h"

#include "clog.h"

clog_define(g_native_log_hub_profiles, cLogLevelInfo, cLogFlags_Default, "native", NULL);

bool native_hub_actions_drain_profiles(NativeHubActionsContext *context) {
    App *app = context->app;
    NativePreconnectUi *ui = context->ui;
    NativeSettings *settings = context->settings;
    bool handled = false;

    if (native_preconnect_ui_take_capture_save(ui)) {
        handled = true;
        NativeSettings candidate = *settings;
        bool saved =
            native_preconnect_ui_get_capture_values(ui, &candidate) &&
            native_config_save_persisted(&candidate);
        if (saved) {
            *settings = candidate;
            native_reconfigure_capture_redirect(app);
            clog(cLogLevelInfo,
                 "saved HUB camera/microphone device settings");
        } else {
            clog(cLogLevelError,
                 "failed to persist HUB camera/microphone device settings");
        }
        native_preconnect_ui_finish_capture_save(
            ui, saved,
            saved ? "Camera and microphone settings saved."
                  : "Could not save settings on this TV. Check storage and retry.");
    }

    int saved_slot = -1;
    uint16_t saved_audio_codec = settings->audio_codec;
    bool save_requested = native_preconnect_ui_take_save(ui, &saved_slot, &saved_audio_codec);
    if (save_requested) {
        handled = true;
    }
    if (save_requested && saved_slot >= 0 && saved_slot < NATIVE_SETTINGS_MAX_SESSIONS) {
        NativeSessionConfig edited;
        bool saved = false;
        char save_status[128] = "";
        if (native_preconnect_ui_get_slot_values(ui, saved_slot, &edited)) {
            edited.duck_mask = settings->sessions[saved_slot].duck_mask;
            NativeSettings candidate = *settings;
            candidate.sessions[saved_slot] = edited;
            candidate.audio_codec = saved_audio_codec;
            native_settings_recompute_capture_gates(&candidate);
            NativeSessionSlot *runtime = &app->sessions[saved_slot];
            bool connection_changed =
                native_session_connection_config_changed(&runtime->config, &edited);
            if (connection_changed && native_session_is_still_connecting(runtime)) {
                (void)snprintf(save_status, sizeof(save_status),
                               "The %s session is still connecting.",
                               native_session_slot_name(saved_slot));
            } else if (native_config_save_persisted(&candidate)) {
                bool endpoint_changed = native_session_endpoint_changed(&runtime->config, &edited);
                *settings = candidate;
                native_reconfigure_capture_redirect(app);
                if (app->audio_codec != saved_audio_codec) {
                    clog(cLogLevelInfo, "audio codec preference now %s (new connections only)",
                         saved_audio_codec == NATIVE_AUDIO_CODEC_PCM ? "pcm" : "auto");
                    app->audio_codec = saved_audio_codec;
                }
                if (runtime->rdp && connection_changed) {
                    /* Never leave the live old endpoint behind a newly saved
                     * identity. Save stops it; Save and connect replaces it. */
                    native_stop_slot(app, saved_slot);
                    native_preconnect_ui_set_slot_state(ui, saved_slot, NATIVE_PRECONNECT_SESSION_OFFLINE,
                                                        NULL);
                    if (!native_any_slot_connected(app)) {
                        native_stop_media(app);
                    }
                }
                if (!runtime->rdp) {
                    if (endpoint_changed) {
                        runtime->refresh_ineffective = false;
                    }
                    pthread_mutex_lock(&app->redaction_lock);
                    runtime->config = edited;
                    pthread_mutex_unlock(&app->redaction_lock);
                } else {
                    /* A display-name-only edit is safe on the existing endpoint. */
                    pthread_mutex_lock(&app->redaction_lock);
                    (void)snprintf(runtime->config.name, sizeof(runtime->config.name), "%s", edited.name);
                    pthread_mutex_unlock(&app->redaction_lock);
                }
                saved = true;
            } else {
                clog(cLogLevelError, "failed to persist the %s profile",
                     native_session_slot_name(saved_slot));
                (void)snprintf(save_status, sizeof(save_status),
                               "Could not save settings on this TV. Check storage and retry.");
            }
        }
        native_preconnect_ui_finish_save(ui, saved_slot, saved,
                                         saved ? "Saved on this TV."
                                               : (save_status[0] ? save_status
                                                                 : "Could not save this profile."));
    }

    int deleted_slot = -1;
    bool delete_requested = native_preconnect_ui_take_delete(ui, &deleted_slot);
    if (delete_requested) {
        handled = true;
    }
    if (delete_requested && deleted_slot >= 0 && deleted_slot < NATIVE_SETTINGS_MAX_SESSIONS) {
        NativeSessionConfig empty = {0};
        empty.port = 3389;
        empty.fps = 60;
        NativeSettings candidate = *settings;
        candidate.sessions[deleted_slot] = empty;
        candidate.camera_enabled = false;
        candidate.audio_input_enabled = false;
        for (int slot = 0; slot < NATIVE_SETTINGS_MAX_SESSIONS; slot++) {
            candidate.camera_enabled |=
                candidate.sessions[slot].camera_redirect;
            candidate.audio_input_enabled |=
                candidate.sessions[slot].audio_input_redirect;
        }
        NativeSessionSlot *runtime = &app->sessions[deleted_slot];
        bool deleted = false;
        char delete_status[128] = "";
        if (native_session_is_still_connecting(runtime)) {
            (void)snprintf(delete_status, sizeof(delete_status),
                           "The %s session is still connecting.",
                           native_session_slot_name(deleted_slot));
        } else if (native_config_save_persisted(&candidate)) {
            deleted = true;
            native_stop_slot(app, deleted_slot);
            *settings = candidate;
            runtime->refresh_ineffective = false;
            pthread_mutex_lock(&app->redaction_lock);
            runtime->config = empty;
            pthread_mutex_unlock(&app->redaction_lock);
            native_reconfigure_capture_redirect(app);
            app->duck_mask[deleted_slot] = 0;
            app->mixer_mute_mask &= (uint8_t)~(uint8_t)(1u << deleted_slot);
            app->mixer_solo_mask &= (uint8_t)~(uint8_t)(1u << deleted_slot);
            native_publish_effective_solo_mask(app);
            app->mixer_gain_db[deleted_slot] = 0;
            native_audio_pipeline_set_source_muted(&app->audio_pipeline, deleted_slot, false);
            native_audio_pipeline_set_source_gain(&app->audio_pipeline, deleted_slot,
                                                  native_ui_mixer_gain_db_to_q15(0));
            native_duck_retarget(app);
            native_preconnect_ui_set_slot_state(ui, deleted_slot,
                                                NATIVE_PRECONNECT_SESSION_NOT_SET_UP, NULL);
            if (!native_any_slot_connected(app)) {
                native_stop_media(app);
            }
        } else {
            clog(cLogLevelError, "failed to delete the %s profile",
                 native_session_slot_name(deleted_slot));
            (void)snprintf(delete_status, sizeof(delete_status),
                           "Could not delete this profile. Check storage and retry.");
        }
        native_preconnect_ui_finish_delete(ui, deleted_slot, deleted,
                                           deleted ? "Profile deleted."
                                                   : (delete_status[0] ? delete_status
                                                                       : "Could not delete this profile."));
    }

    return handled;
}

#endif
