/* Connect request validation, optional persistence, video-owner transition, and
 * asynchronous worker startup. This phase always consumes its one-shot request. */
#ifdef HELLOLG_TARGET_WEBOS

#include "native_hub_actions_internal.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>

#include "native_app.h"
#include "native_capture_glue.h"
#include "native_config.h"
#include "native_config_storage.h"
#include "native_pointer.h"
#include "native_presentation.h"
#include "native_rgba_owner.h"
#include "native_session.h"
#include "native_switch.h"

#include "clog.h"

clog_define(g_native_log_hub_connect, cLogLevelInfo, cLogFlags_Default, "native", NULL);

bool native_hub_actions_drain_connect(NativeHubActionsContext *context) {
    App *app = context->app;
    NativePreconnectUi *ui = context->ui;
    NativeSettings *settings = context->settings;
    bool handled = false;

    int requested_slot = NATIVE_SESSION_SLOT_GREEN;
    char requested_host[NATIVE_SETTINGS_STRING_MAX];
    char requested_username[NATIVE_SETTINGS_STRING_MAX];
    char requested_password[NATIVE_SETTINGS_STRING_MAX];
    char requested_domain[NATIVE_SETTINGS_STRING_MAX];
    uint16_t requested_port = 0;
    uint16_t requested_fps = 0;
    uint16_t requested_audio_codec = NATIVE_AUDIO_CODEC_AUTO;
    bool requested_requires_save = false;
    /* ALWAYS consume the one-shot request: gating the take on slot state would leave
     * a Connect clicked on a still-connecting slot's configurator latched in the UI,
     * to be replayed as a surprise reconnect whenever that session later fails. */
    if (native_preconnect_ui_take_connect(ui, &requested_slot, requested_host, sizeof(requested_host),
                                          &requested_port, requested_username, sizeof(requested_username),
                                          requested_password, sizeof(requested_password), requested_domain,
                                          sizeof(requested_domain), &requested_fps, &requested_audio_codec,
                                          &requested_requires_save)) {
        handled = true;
        if (requested_slot < 0 || requested_slot >= NATIVE_SETTINGS_MAX_SESSIONS) {
            requested_slot = NATIVE_SESSION_SLOT_GREEN;
        }
        NativeSessionSlot *requested_live = &app->sessions[requested_slot];
        NativeSettings candidate = *settings;
        NativeSessionConfig *session = &candidate.sessions[requested_slot];
        char validation_message[128] = "";
        bool ready_to_connect = true;
        if (native_session_is_still_connecting(requested_live)) {
            /* Mid-handshake: joining the worker here could block the SDL thread for
             * the whole connect timeout, so reject instead. */
            (void)snprintf(validation_message, sizeof(validation_message),
                           "The %s session is still connecting.", native_session_slot_name(requested_slot));
            ready_to_connect = false;
        } else if (!copy_config_string(session->host, sizeof(session->host), requested_host, "host")) {
            (void)snprintf(validation_message, sizeof(validation_message), "Host value is too long.");
            ready_to_connect = false;
        } else if (!copy_config_string(session->username, sizeof(session->username), requested_username,
                                       "username")) {
            (void)snprintf(validation_message, sizeof(validation_message), "Username value is too long.");
            ready_to_connect = false;
        } else if (!copy_config_string(session->password, sizeof(session->password), requested_password,
                                       "password")) {
            (void)snprintf(validation_message, sizeof(validation_message), "Password value is too long.");
            ready_to_connect = false;
        } else if (!copy_config_string(session->domain, sizeof(session->domain), requested_domain, "domain")) {
            (void)snprintf(validation_message, sizeof(validation_message), "Domain value is too long.");
            ready_to_connect = false;
        } else {
            NativeSessionConfig edited;
            if (native_preconnect_ui_get_slot_values(ui, requested_slot, &edited)) {
                (void)snprintf(session->name, sizeof(session->name), "%s", edited.name);
                session->camera_redirect = edited.camera_redirect;
                session->audio_input_redirect =
                    edited.audio_input_redirect;
            }
            session->port = requested_port;
            session->fps = requested_fps;
            candidate.audio_codec = requested_audio_codec;
            native_settings_recompute_capture_gates(&candidate);
            native_config_apply_initial_desktop_hint(&candidate);
            if (!native_session_config_validate_connect(session, requested_slot, validation_message,
                                                        sizeof(validation_message))) {
                ready_to_connect = false;
            }
        }

        if (ready_to_connect && requested_requires_save && !native_config_save_persisted(&candidate)) {
            clog(cLogLevelError, "failed to persist the %s profile before connect",
                 native_session_slot_name(requested_slot));
            (void)snprintf(validation_message, sizeof(validation_message),
                           "Could not save settings on this TV. Check storage and retry.");
            ready_to_connect = false;
        }
        native_preconnect_ui_finish_connect_save(ui, requested_slot, ready_to_connect,
                                                  ready_to_connect && requested_requires_save,
                                                  ready_to_connect ? NULL : validation_message);
        if (ready_to_connect) {
            /* A HUB-originated connect exits only after the asynchronous worker is
             * ACTIVE. Until then the old stream remains the BACK destination. */
            bool connect_from_hub = app->hub_visible;
            app->hub_connect_target = connect_from_hub ? requested_slot : -1;
            if (requested_requires_save) {
                *settings = candidate;
                native_reconfigure_capture_redirect(app);
            }
            session = &settings->sessions[requested_slot];
            if (app->audio_codec != requested_audio_codec) {
                clog(cLogLevelInfo, "audio codec preference now %s (new connections only)",
                     requested_audio_codec == NATIVE_AUDIO_CODEC_PCM ? "pcm" : "auto");
                app->audio_codec = requested_audio_codec;
            }
            native_config_log_effective(settings);
            native_preconnect_ui_set_connecting(ui, requested_slot, true, "Connecting...");
            int old_index = atomic_load(&app->active_index);
            /* Join before replacing config: workers may still consult their old
             * redaction strings while winding down. This is an explicit user
             * connection, not an internal recovery reconnect, so it starts a new
             * logical-session clock once the replacement reaches ACTIVE. */
            app->session_started_ms[requested_slot] = 0;
            app->session_runtime_active[requested_slot] = false;
            native_stop_slot(app, requested_slot);
            if (native_session_endpoint_changed(&app->sessions[requested_slot].config, session)) {
                app->sessions[requested_slot].refresh_ineffective = false;
            }
            pthread_mutex_lock(&app->redaction_lock);
            app->sessions[requested_slot].config = *session;
            pthread_mutex_unlock(&app->redaction_lock);

            /* Ask the outgoing server to stop graphics before ownership moves, so
             * no full-rate AUs/bitmap deltas can land in the demotion window. Do
             * not mark it suppressed yet: after the flip native_background_slot()
             * still needs to choose plain suppress vs hidden snapshot reconnect. */
            if (old_index != requested_slot && app->sessions[old_index].rdp &&
                !app->sessions[old_index].suppressed) {
                rdp_set_suppress_output(app->sessions[old_index].rdp, false);
            }
            app->pending_switch_target = -1;
            app->switch_deadline_ticks = 0;
            native_capture_pause_for_active_slot_change(app);
            pthread_mutex_lock(&app->video_lock);
            if (connect_from_hub) {
                native_capture_hub_return_rgba_locked(app);
            }
            if (app->hub_return_rgba) {
                /* native_slot_connect increments this epoch before the worker starts.
                 * Arm the readiness gate now so an immediate callback cannot make us
                 * drop the frozen background before ACTIVE. */
                native_arm_hub_return_replacement_locked(
                    app, requested_slot,
                    atomic_load(&app->sessions[requested_slot].connect_epoch) + 1u,
                    atomic_load(&app->sessions[requested_slot].video_ok_frames));
            }
            /* Any non-frozen canvas belongs to the outgoing/failed owner. The new
             * RemoteFX stream must receive a zeroed surface for its dirty rectangles. */
            native_close_rgba_locked(app, false);
            atomic_store(&app->active_index, requested_slot);
            atomic_store(&app->video_refresh_needed, false);
            native_present_invalidate(app);
            pthread_mutex_unlock(&app->video_lock);
            native_preconnect_ui_select_slot(ui, requested_slot);
            native_duck_retarget(app);
            if (old_index != requested_slot) {
                /* Demotion must precede this helper: snapshot backgrounding may
                 * reconnect old_index, which must not reset the shared decoder. */
                native_background_slot(app, old_index);
            }
            app->ui_last_state = -1;
            if (!native_slot_connect(app, requested_slot, false)) {
                const char *start_error = "Connection failed: start failed";
                if (app->hub_connect_target == requested_slot) {
                    app->hub_connect_target = -1;
                }
                native_preconnect_ui_set_connecting(ui, requested_slot, false, start_error);
                native_preconnect_ui_set_slot_state(ui, requested_slot, NATIVE_PRECONNECT_SESSION_ERROR,
                                                    start_error);
                native_preconnect_ui_open_setup(ui, requested_slot);
                native_preconnect_ui_set_status(ui, start_error, true);
                if (!native_any_slot_connected(app)) {
                    native_stop_media(app);
                }
            } else {
                native_update_pointer_window_size(app);
            }
        }
    }

    return handled;
}

#endif
