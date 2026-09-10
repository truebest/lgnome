/* HUB/configurator and remote-control navigation for session switching.
 * SDL thread only; stream recovery and handoff live in sibling modules. */
#ifdef LGNOME_TARGET_WEBOS

#include "native_switch_internal.h"

#include <stdatomic.h>

#include "native_app.h"
#include "native_capture_glue.h"
#include "native_input_route.h"
#include "native_mixer_overlay.h"
#include "native_presentation.h"
#include "native_rgba_owner.h"
#include "native_session.h"

#include "clog.h"

clog_define(g_native_log_switch_navigation, cLogLevelInfo, "native");

/* Reveal the four-card HUB without stopping the active RDP session. Translucent LVGL
 * chrome is composited over the still-running video plane, while input is released so
 * the remote can navigate locally. A subsequent card action resumes/switches through
 * the normal session path. */
void native_show_hub(App *app) {
    if (!app || !app->streaming_visible || app->mixer_overlay_visible || !app->preconnect_ui) {
        return;
    }
    int active = atomic_load(&app->active_index);
    clog(cLogLevelInfo, "central remote button opens HUB on the %s profile",
         native_session_slot_name(active));

    /* A HUB request supersedes a deferred colour-key switch. Its target can finish
     * snapshot backgrounding normally; it no longer owns navigation. */
    app->pending_switch_target = -1;
    native_stop_streaming_input(app);
    native_input_set_active(&app->input, false);
    native_preconnect_ui_set_keyboard_available(app->preconnect_ui, native_evdev_input_probe_keyboard());
    native_preconnect_ui_set_mouse_available(app->preconnect_ui, native_evdev_input_probe_mouse());
    native_cursor_show_default();
    app->indicator_slot = -1;
    app->indicator_drawn = false;
    app->switch_splash_drawn = false;
    pthread_mutex_lock(&app->video_lock);
    /* Re-arm the one-shot punch for the eventual return so the final HUB chrome frame
     * is cleared before raw streaming input resumes. */
    native_present_invalidate(app);
    pthread_mutex_unlock(&app->video_lock);
    app->streaming_visible = false;
    app->hub_visible = true;
    app->hub_return_slot = active;
    app->hub_connect_target = -1;
    app->ui_last_state = -1;
    native_preconnect_ui_show_hub(app->preconnect_ui, active);
    native_preconnect_ui_set_visible(app->preconnect_ui, true);
}

/* Navigates the screen to `target`'s configurator (pre-connect form) while any current
 * session keeps running in the background: its graphics are suppressed server-side and
 * its audio keeps feeding the mix. */
void native_show_slot_configurator(App *app, int target) {
    /* Navigating to a form invalidates a deferred switch; the prepared slot finishes
     * backgrounding on its own. */
    app->pending_switch_target = -1;
    app->hub_connect_target = -1;
    int old_index = atomic_load(&app->active_index);
    if (old_index != target) {
        clog(cLogLevelInfo, "showing the %s session configurator", native_session_slot_name(target));
        /* Releases owed to the old server must go out while input is still wired to it;
         * the evdev grab is dropped because the configurator needs the SDL mouse. */
        native_stop_streaming_input(app);
        native_preconnect_ui_set_keyboard_available(app->preconnect_ui, native_evdev_input_probe_keyboard());
        native_preconnect_ui_set_mouse_available(app->preconnect_ui, native_evdev_input_probe_mouse());
        native_input_set_active(&app->input, false);
        native_capture_pause_for_active_slot_change(app);
        pthread_mutex_lock(&app->video_lock);
        /* Capture before the ownership flip: an in-flight old bitmap callback either
         * finishes under this lock and is frozen with the frame, or observes the new
         * active slot on its under-lock recheck and drops the update. */
        native_capture_hub_return_rgba_locked(app);
        native_close_rgba_locked(app, false);
        atomic_store(&app->active_index, target);
        /* The video track stays open behind the (opaque) configurator: closing it would
         * reload the shared pipeline and cut the mixed audio for nothing. Returning to a
         * stream swaps the decoder on that stream's next keyframe (on_video_au). */
        native_present_invalidate(app);
        pthread_mutex_unlock(&app->video_lock);
        native_duck_retarget(app);
        /* Demote the old slot BEFORE backgrounding it: snapshot backgrounding reconnects
         * the slot, and native_slot_connect must see it as inactive so the shared
         * decoder/input state stays with the incoming screen. */
        native_background_slot(app, old_index);
        native_input_set_session(&app->input, app->sessions[target].rdp);
        /* The configurator needs the plain SDL arrow, but the old session keeps running
         * backgrounded: leave its cached cursor shape/hidden state alone so switching
         * back can reassert it (the server resends nothing while suppressed). */
        native_cursor_show_default();
        app->switch_deadline_ticks = 0;
    }
    app->streaming_visible = false;
    app->ui_last_state = -1;
    if (app->preconnect_ui) {
        /* Any in-flight connect banner belongs to the slot we are leaving; its session
         * continues in the background and reports through per-slot status lines. */
        native_preconnect_ui_set_connecting(app->preconnect_ui, old_index, false, "");
        native_preconnect_ui_open_setup(app->preconnect_ui, target);
        native_preconnect_ui_set_visible(app->preconnect_ui, true);
    }
}

/* Color-button press: every button is "go to that computer" — resume a live stream,
 * connect a configured offline profile, or open setup for an empty slot. */
void native_request_session_switch(App *app, int target) {
    if (!app || target < 0 || target >= NATIVE_SETTINGS_MAX_SESSIONS) {
        return;
    }
    if (app->mixer_overlay_visible) {
        /* Mixer mode: a color key selects that slot's slider; the ACTIVE slot's key (the
         * one that opened the overlay) toggles it back off. Switching resumes once the
         * overlay is gone. */
        if (target == atomic_load(&app->active_index)) {
            native_mixer_overlay_hide(app);
        } else {
            native_mixer_overlay_select(app, target);
        }
        return;
    }
    NativeSessionSlot *slot = &app->sessions[target];
    if (target == atomic_load(&app->active_index) && app->pending_switch_target >= 0) {
        /* The active slot's key while a deferred switch is in flight = change of mind:
         * stay here. The prepared slot finishes backgrounding on its own (the tick
         * suppresses it once its IDR lands), ready for a later switch. */
        clog(cLogLevelInfo, "deferred switch to %s canceled",
             native_session_slot_name(app->pending_switch_target));
        app->pending_switch_target = -1;
        native_show_session_indicator(app, target);
        return;
    }
    /* A slot mid-hidden-reconnect (snapshot_pending) is still a live stream target: the
     * user watched it moments ago; treat the press as a switch rather than dropping to
     * its configurator. */
    if (native_slot_is_live_stream_target(slot)) {
        /* A live resume/switch answers the HUB visit. Do this only after identifying a
         * stream-capable target: choosing a still-CONNECTING background card must leave
         * HUB open until that connection can actually be entered. */
        app->hub_visible = false;
        app->hub_connect_target = -1;
        if (target == atomic_load(&app->active_index) && app->streaming_visible) {
            /* Re-pressing the active slot's button is no switch: it opens the mixer. */
            native_mixer_overlay_show(app);
            return;
        }
        if (target == app->pending_switch_target) {
            return; /* already preparing this switch; the tick completes it */
        }
        /* Switch NOW only when the target can take the screen without a black reload
         * window: it is streaming live (never suppressed), or its cached IDR is ready
         * and the stream has been quiet past the suppress tail. Anything else defers:
         * the CURRENT stream keeps the screen while the target gets ready. */
        bool ready_now;
        if (atomic_load(&slot->video_via_bitmap)) {
            /* Bitmap streams have no AUs to snapshot; resume simply restarts bitmap
             * updates, so the old immediate switch IS the right path for them. */
            ready_now = true;
        } else if (slot->snapshot_pending) {
            ready_now = false;
        } else if (!slot->suppressed) {
            ready_now = true;
        } else {
            ready_now = atomic_load(&slot->snapshot_idr_ready) &&
                        native_monotonic_ms() - atomic_load(&slot->snapshot_last_au_ms) >=
                            NATIVE_SNAPSHOT_QUIET_MS;
        }
        if (ready_now) {
            app->pending_switch_target = -1;
            native_complete_session_switch(app, target);
            return;
        }
        clog(cLogLevelInfo, "deferring the switch to %s until its stream is ready",
             native_session_slot_name(target));
        app->pending_switch_target = target;
        app->pending_switch_deadline_ticks = SDL_GetTicks() + NATIVE_PENDING_SWITCH_TIMEOUT_MS;
        slot->snapshot_retry_used = false;
        /* Acknowledge the press right away: the badge shows the DESTINATION while the
         * current stream keeps playing. */
        native_show_session_indicator(app, target);
        if (slot->suppressed && atomic_load(&slot->snapshot_idr_ready)) {
            /* The cache is ready and merely inside the quiet window (a fast flip-back):
             * preparing would throw it away for a needless reconnect. Just wait — the
             * tick completes the switch within NATIVE_SNAPSHOT_QUIET_MS. */
            return;
        }
        native_prepare_pending_switch(app, target);
        return;
    }
    if (slot->rdp) {
        /* A worker that has not reached ACTIVE is already connecting. Do not queue a
         * duplicate attempt; its live card state will catch up on the next loop tick. */
        return;
    }
    native_show_slot_configurator(app, target);
    (void)native_preconnect_ui_request_connect(app->preconnect_ui, target);
}

/* Remote D-pad ring switching: left/right cycles to the neighbouring stream-capable slot
 * (red -> green -> yellow -> blue -> red). Only slots holding a session that can take
 * the screen participate — empty slots stay reachable through their color buttons — and
 * with nothing else connected the press just re-flashes the badge. */
void native_ring_switch(App *app, int direction) {
    int active = atomic_load(&app->active_index);
    for (int step = 1; step < NATIVE_SETTINGS_MAX_SESSIONS; step++) {
        int candidate = (active + direction * step) % NATIVE_SETTINGS_MAX_SESSIONS;
        if (candidate < 0) {
            candidate += NATIVE_SETTINGS_MAX_SESSIONS;
        }
        NativeSessionSlot *slot = &app->sessions[candidate];
        if (native_slot_is_live_stream_target(slot)) {
            native_request_session_switch(app, candidate);
            return;
        }
    }
    native_show_session_indicator(app, active); /* alone on the ring: acknowledge the press */
}

#endif
