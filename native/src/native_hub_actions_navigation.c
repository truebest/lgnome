/* HUB BACK/activate navigation requests. The caller orders this phase ahead of
 * every settings mutation and connection transaction. */
#ifdef LGNOME_TARGET_WEBOS

#include "native_hub_actions_internal.h"

#include <stdatomic.h>

#include "native_app.h"
#include "native_switch.h"

#include "clog.h"

clog_define(g_native_log_hub_navigation, cLogLevelInfo, "native");

bool native_hub_actions_drain_navigation(NativeHubActionsContext *context) {
    App *app = context->app;
    NativePreconnectUi *ui = context->ui;
    int activate_slot = -1;
    bool hub_close_requested = native_preconnect_ui_take_hub_close(ui);
    bool handled = hub_close_requested;
    if (hub_close_requested) {
        /* BACK wins over every navigation action queued in the same LVGL input
         * batch, including Connect/Retry on an offline card. */
        native_preconnect_ui_cancel_pending_navigation(ui);
        if (!app->hub_visible) {
            /* On the initial pre-connect screen BACK has no desktop to return to;
             * keep the HUB usable instead of leaving its close guard armed. */
            native_preconnect_ui_cancel_hub_close(ui);
        }
    }
    if (hub_close_requested && app->hub_visible) {
        int return_slot = app->hub_return_slot;
        bool return_slot_live = false;
        if (return_slot >= 0 && return_slot < NATIVE_SETTINGS_MAX_SESSIONS) {
            NativeSessionSlot *return_session = &app->sessions[return_slot];
            return_slot_live = native_slot_is_live_stream_target(return_session);
        }
        if (!return_slot_live) {
            return_slot = -1;
            for (int candidate = 0; candidate < NATIVE_SETTINGS_MAX_SESSIONS; candidate++) {
                NativeSessionSlot *candidate_slot = &app->sessions[candidate];
                if (native_slot_is_live_stream_target(candidate_slot) &&
                    atomic_load(&candidate_slot->current_state) == (int)RDP_STATE_ACTIVE) {
                    return_slot = candidate;
                    break;
                }
            }
            for (int candidate = 0;
                 return_slot < 0 && candidate < NATIVE_SETTINGS_MAX_SESSIONS; candidate++) {
                NativeSessionSlot *candidate_slot = &app->sessions[candidate];
                if (native_slot_is_live_stream_target(candidate_slot) &&
                    candidate_slot->snapshot_pending) {
                    return_slot = candidate;
                }
            }
        }
        if (return_slot >= 0) {
            clog(cLogLevelInfo, "remote BACK closes HUB and returns to the %s profile",
                 native_session_slot_name(return_slot));
            native_request_session_switch(app, return_slot);
            if (app->hub_visible) {
                /* The worker can leave ACTIVE between validation and the switch;
                 * release the guard when no stream accepted the return request. */
                native_preconnect_ui_cancel_hub_close(ui);
            }
        } else {
            clog(cLogLevelWarning, "remote BACK cannot close HUB: no live session remains");
            native_preconnect_ui_cancel_hub_close(ui);
        }
    }
    if (!hub_close_requested && native_preconnect_ui_take_activate(ui, &activate_slot)) {
        handled = true;
        native_request_session_switch(app, activate_slot);
    }
    return handled;
}

#endif
