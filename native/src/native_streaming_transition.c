/* SDL-thread transition from HUB/setup into a live desktop: derive active-slot
 * input state, resume suppressed output when necessary, hide the UI, grab input,
 * and restore the server cursor before streaming becomes visible. */
#ifdef HELLOLG_TARGET_WEBOS

#include "native_loop_internal.h"

#include <stdatomic.h>
#include <stdio.h>

#include "cursor_sdl.h"
#include "input_evdev.h"
#include "input_sdl.h"
#include "native_app.h"
#include "native_config_storage.h"
#include "native_input_route.h"
#include "native_mixer_overlay.h"
#include "native_pointer.h"
#include "native_presentation.h"
#include "native_rdp_state.h"
#include "native_settings.h"
#include "native_switch.h"
#include "rdp_ffi.h"

#include "clog.h"

clog_define(g_native_log_streaming_transition, cLogLevelInfo, cLogFlags_Default, "native", NULL);

/* Derives input arming, coordinate mapping, and the NumLock sync from the
 * ACTIVE slot's state, on the SDL thread only: worker callbacks must not write
 * shared input state, and the mixer overlay owns the pointer while visible
 * (evdev grab released) so nothing leaks to the session behind it. */
static bool loop_arm_streaming_input(App *app) {
    NativeSessionSlot *active = native_active_slot(app);
    bool input_streaming = app->streaming_visible && active->rdp &&
                           atomic_load(&active->current_state) == (int)RDP_STATE_ACTIVE &&
                           !app->mixer_overlay_visible;
    native_input_set_active(&app->input, input_streaming);
    /* Input coordinate mapping follows the ACTIVE slot's desktop size, derived
     * here like the arming flag: workers only publish their own slot's atomics
     * (a worker writing app->input directly would race a session switch). */
    uint16_t desk_w = (uint16_t)atomic_load(&active->desktop_width);
    uint16_t desk_h = (uint16_t)atomic_load(&active->desktop_height);
    if (active->rdp && desk_w != 0 && desk_h != 0 &&
        (desk_w != (uint16_t)atomic_load(&app->input.desktop_width) ||
         desk_h != (uint16_t)atomic_load(&app->input.desktop_height))) {
        native_input_set_desktop_size(&app->input, desk_w, desk_h);
        native_request_pointer_window_size_update(app);
    }
    if (input_streaming && !app->input_locks_synced) {
        /* Fresh sessions start with the server's toggle keys in an unknown state;
         * force NumLock on so an attached keyboard's numpad types digits instead of
         * navigating. The TV has no lock-state source of its own to mirror (webOS SDL
         * does not track keyboard LEDs), and a NumLock key press still toggles the
         * server normally. */
        native_input_sync_locks(&app->input, false, true, false);
        app->input_locks_synced = true;
    } else if (!input_streaming) {
        app->input_locks_synced = false;
    }
    return input_streaming;
}

void native_streaming_transition_tick(App *app, NativePreconnectUi *ui, NativeSettings *settings,
                                      SDL_Window *window) {
    NativeSessionSlot *active = native_active_slot(app);
    int state = atomic_load(&active->current_state);
    bool input_streaming = loop_arm_streaming_input(app);
    if (active->rdp && state != app->ui_last_state && state != (int)RDP_STATE_ACTIVE) {
        char status[64];
        (void)snprintf(status, sizeof(status), "%s...", rdp_state_name((RdpState)state));
        native_preconnect_ui_set_status(ui, status, false);
        app->ui_last_state = state;
    }
    bool hub_connect_ready = app->hub_connect_target == atomic_load(&app->active_index);
    if (active->rdp && state == (int)RDP_STATE_ACTIVE && !app->streaming_visible &&
        app->pending_switch_target < 0 && (!app->hub_visible || hub_connect_ready)) {
        clog(cLogLevelDebug, "entering streaming screen for the %s session",
             native_session_slot_name(active->index));
        if (hub_connect_ready) {
            /* Only a successful ACTIVE transition commits the HUB exit. Until this
             * point BACK must still be able to resume the stream below the HUB. */
            app->hub_connect_target = -1;
            app->hub_visible = false;
        }
        if (active->suppressed) {
            /* The user left this slot mid-connect (queueing a suppress in its worker)
             * and returned before it went active, so no switch path ran to resume it:
             * without this the buffered suppress lands right after activation and the
             * on-screen session streams no video. Resume + force a keyframe exactly
             * like a switch-back, and arm the watchdog for servers where the refresh
             * request is a no-op (it reconnects for a fresh IDR). */
            active->suppressed = false;
            native_switch_watchdog_baseline(app, active);
            rdp_set_suppress_output(active->rdp, true);
            rdp_request_refresh(active->rdp);
            native_switch_watchdog_start(app, NATIVE_SWITCH_KEYFRAME_TIMEOUT_MS);
        }
        (void)native_config_save_persisted(settings);
        native_preconnect_ui_set_connecting(ui, atomic_load(&app->active_index), false, "");
        native_preconnect_ui_set_visible(ui, false);
        app->window_unfocused = false; /* enter streaming focused, whatever fired during preconnect */
        /* Grab input now that streaming has started; the preconnect UI needs the SDL mouse,
         * so we don't grab earlier. Note any degradation on the (now hidden) preconnect
         * status so it shows if the session returns here; the loud log covers diagnosis. */
        NativeInputStartResult input_result = native_start_streaming_input(app);
        native_preconnect_ui_set_mouse_available(ui, native_evdev_input_mouse_active(&app->evdev_input));
        switch (input_result) {
        case NATIVE_INPUT_START_OK:
            native_preconnect_ui_set_keyboard_available(ui, true);
            break;
        case NATIVE_INPUT_START_NO_KEYBOARD:
            native_preconnect_ui_set_keyboard_available(ui, false);
            break;
        case NATIVE_INPUT_START_UNAVAILABLE:
            native_preconnect_ui_set_input_unavailable(ui);
            break;
        }
        /* HUB and setup use the system arrow and can leave it at an unrelated UI
         * coordinate. Restore the server cursor and home the compositor pointer to
         * the last RDP position before input starts flowing again. */
        native_cursor_reassert(&active->cursor);
        app->cursor_reassert_pending = true;
        SDL_WarpMouseInWindow(window, atomic_load(&app->virtual_mouse_x), atomic_load(&app->virtual_mouse_y));
        app->streaming_visible = true;
        app->hub_visible = false;
        native_system_volume_rebaseline(app);
        native_show_session_indicator(app, atomic_load(&app->active_index));
    }
}

#endif
