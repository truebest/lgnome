/* SDL-thread supervisor for terminal RDP worker events: update the HUB,
 * select a surviving foreground session when possible, and tear down failed
 * slots plus the shared media pipeline in the required order. */
#ifdef HELLOLG_TARGET_WEBOS

#include "native_loop_internal.h"

#include <stdatomic.h>
#include <stdio.h>

#include "cursor_sdl.h"
#include "input_evdev.h"
#include "native_app.h"
#include "native_input_route.h"
#include "native_mixer_overlay.h"
#include "native_rdp_state.h"
#include "native_session.h"
#include "native_settings.h"
#include "native_switch.h"

#include "clog.h"

clog_define(g_native_log_session_supervisor, cLogLevelInfo, cLogFlags_Default, "native", NULL);

void native_session_supervisor_tick(App *app, NativePreconnectUi *ui) {
    /* Drain per-slot terminal events. The active slot decides between an automatic
     * switch to the surviving session and a return to the pre-connect UI; a
     * background slot is torn down quietly (the mix simply loses its audio). */
    for (int i = 0; i < NATIVE_SETTINGS_MAX_SESSIONS; i++) {
        NativeSessionSlot *slot = &app->sessions[i];
        if (!atomic_load(&slot->session_failed)) {
            continue;
        }
        RdpState terminal_state = (RdpState)atomic_load(&slot->terminal_state);
        char status[128];
        if (terminal_state == RDP_STATE_STOPPED) {
            (void)snprintf(status, sizeof(status), "%s session stopped.", native_session_slot_name(i));
        } else {
            (void)snprintf(status, sizeof(status), "%s session failed: %s", native_session_slot_name(i),
                           rdp_state_name(terminal_state));
        }
        native_preconnect_ui_set_slot_state(ui, i, NATIVE_PRECONNECT_SESSION_ERROR, status);
        if (i == app->hub_connect_target) {
            /* A HUB-originated connect failed before it could own the screen. Keep
             * HUB open and returnable, but stop waiting for this slot to auto-enter. */
            app->hub_connect_target = -1;
        }

        if (i == atomic_load(&app->active_index)) {
            int survivor = -1;
            for (int j = 0; j < NATIVE_SETTINGS_MAX_SESSIONS; j++) {
                if (j != i && native_slot_is_live_stream_target(&app->sessions[j]) &&
                    atomic_load(&app->sessions[j].current_state) == (int)RDP_STATE_ACTIVE) {
                    survivor = j;
                }
            }
            if (survivor >= 0 && app->streaming_visible) {
                /* The session the user was WATCHING died: fall back to the survivor. */
                clog(cLogLevelWarning, "%s; auto-switching video to the %s session", status,
                     native_session_slot_name(survivor));
                native_complete_session_switch(app, survivor);
                native_stop_slot(app, i);
                native_preconnect_ui_set_status(ui, status, true);
            } else if (survivor >= 0) {
                /* A connect attempt made from this slot's configurator failed while
                 * another session runs backgrounded: stay on the form so the user can
                 * read the error and fix the values, instead of bouncing to the
                 * survivor's stream on every retry. */
                native_stop_slot(app, i);
                app->ui_last_state = -1;
                native_preconnect_ui_set_connecting(ui, i, false, status);
                native_preconnect_ui_set_status(ui, status, true);
            } else {
                /* Flush + release input while the failed session pointer is still
                 * wired, then tear the slot down (media follows after the pass once
                 * no slot needs it). */
                native_stop_streaming_input(app);
                native_preconnect_ui_set_keyboard_available(ui, native_evdev_input_probe_keyboard());
                native_preconnect_ui_set_mouse_available(ui, native_evdev_input_probe_mouse());
                native_stop_slot(app, i);
                native_cursor_reset(&slot->cursor);
                app->switch_deadline_ticks = 0;
                app->streaming_visible = false;
                native_mixer_overlay_force_hide(app);
                app->ui_last_state = -1;
                native_preconnect_ui_set_visible(ui, true);
                native_preconnect_ui_set_connecting(ui, i, false, status);
                native_preconnect_ui_set_status(ui, status, true);
            }
        } else {
            clog(cLogLevelWarning, "background %s", status);
            native_stop_slot(app, i);
        }
        if (!app->streaming_visible && !app->hub_visible) {
            int pending_return = app->pending_switch_target;
            bool pending_return_live = false;
            if (pending_return >= 0 && pending_return < NATIVE_SETTINGS_MAX_SESSIONS) {
                NativeSessionSlot *pending_slot = &app->sessions[pending_return];
                pending_return_live = native_slot_is_live_stream_target(pending_slot);
            }
            if (!pending_return_live) {
                /* A HUB return can be accepted just before its target dies. The UI
                 * is still on screen in that race, so make it interactive again
                 * instead of waiting for a hide transition that will never happen. */
                native_preconnect_ui_cancel_hub_close(ui);
            }
        }
        /* After each drained failure: when NO slot holds a session anymore, the
         * shared media pipeline must go too. Checked per drained slot (not inside
         * the branches) so an active+background double failure in one pass — where
         * the active slot drains first while the background one still counts as
         * connected — cannot leak the pipeline. */
        if (!native_any_slot_connected(app)) {
            native_stop_media(app);
        }
    }
}

#endif
