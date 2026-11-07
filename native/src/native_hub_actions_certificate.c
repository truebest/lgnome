#ifdef LGNOME_TARGET_WEBOS

#include "native_hub_actions_internal.h"

#include <string.h>

#include "native_app.h"
#include "ui_preconnect.h"
#include "native_config_storage.h"
#include "native_switch.h"

bool native_hub_actions_certificate(NativeHubActionsContext *context) {
    App *app = context->app;
    NativePreconnectUi *ui = context->ui;
    if (app->certificate_prompt_active) {
        NativeSessionSlot *slot = &app->sessions[app->certificate_prompt_slot];
        pthread_mutex_lock(&app->video_lock);
        bool current = slot->certificate_pending && slot->rdp &&
            atomic_load(&slot->connect_epoch) == app->certificate_prompt_epoch &&
            strcmp(slot->certificate_sha256, app->certificate_prompt_sha256) == 0;
        pthread_mutex_unlock(&app->video_lock);
        if (!current) {
            native_preconnect_ui_close_certificate(ui);
            app->certificate_prompt_active = false;
        } else {
            bool accept, remember;
            if (native_preconnect_ui_take_certificate(ui, &accept, &remember)) {
                if (accept && remember) {
                    NativeSettings candidate = *context->settings;
                    NativeSessionConfig trusted = slot->config;
                    if (!native_session_trust_certificate(&trusted, app->certificate_prompt_sha256)) return true;
                    candidate.sessions[slot->index].certificate_trust = trusted.certificate_trust;
                    if (!native_config_save_persisted(&candidate)) {
                        native_preconnect_ui_certificate_error(ui,
                            "Could not save certificate trust. Retry, uncheck the box to connect once, or cancel.");
                        return true;
                    }
                    *context->settings = candidate;
                    native_preconnect_ui_set_certificate_trust(ui, slot->index, &trusted.certificate_trust);
                }
                if (accept) {
                    (void)native_session_trust_certificate(&slot->config, app->certificate_prompt_sha256);
                }
                pthread_mutex_lock(&app->video_lock);
                slot->certificate_pending = false;
                pthread_mutex_unlock(&app->video_lock);
                native_preconnect_ui_close_certificate(ui);
                app->certificate_prompt_active = false;
                (void)rdp_confirm_certificate(slot->rdp, app->certificate_prompt_sha256, accept);
            }
            return true;
        }
    }
    for (int i = 0; i < NATIVE_SETTINGS_MAX_SESSIONS; ++i) {
        NativeSessionSlot *slot = &app->sessions[i];
        char reason[sizeof(slot->certificate_reason)];
        pthread_mutex_lock(&app->video_lock);
        bool pending = slot->certificate_pending && slot->rdp;
        if (pending) {
            memcpy(app->certificate_prompt_sha256, slot->certificate_sha256, sizeof(slot->certificate_sha256));
            memcpy(reason, slot->certificate_reason, sizeof(reason));
        }
        pthread_mutex_unlock(&app->video_lock);
        if (!pending) continue;
        if (app->streaming_visible) native_show_hub(app);
        /* A mixer owns the screen until dismissed; do not put a hidden prompt behind it. */
        if (app->streaming_visible) return true;
        app->certificate_prompt_slot = i;
        app->certificate_prompt_epoch = atomic_load(&slot->connect_epoch);
        app->certificate_prompt_active = true;
        native_preconnect_ui_set_visible(ui, true);
        native_preconnect_ui_show_certificate(ui, &slot->config, app->certificate_prompt_sha256, reason);
        return true;
    }
    return false;
}

#endif
