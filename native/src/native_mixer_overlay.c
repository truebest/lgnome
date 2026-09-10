/* Volume-mixer overlay state machine and the system-volume watcher (SDL
 * thread only). The fader model constants and both renderers live in
 * ui_mixer.h/.c; this file owns which overlay is up, the selection, the
 * gain/duck/mute/solo edits, key routing from both input paths, and the
 * LS2 volume poll that keeps the MASTER fader honest. */
#ifdef LGNOME_TARGET_WEBOS

#include "native_mixer_overlay.h"

#include <stdatomic.h>

#include "native_app.h"
#include "native_input_route.h"
#include "native_pointer.h"
#include "native_config_storage.h"
#include "native_presentation.h"
#include "native_session.h"

#include "clog.h"

clog_define(g_native_log_mixer_overlay, cLogLevelInfo, "native");

/* ---- Volume-mixer overlay (SDL thread only) ----
 * A semi-transparent panel over the live stream with one vertical slider per session
 * slot, in that slot's badge color. Opened by re-pressing the ACTIVE slot's
 * color button; volume changes hit the mixer source immediately. Up/down move the selected slider, left/right (or another slot's color
 * key) change the selection, the active slot's color key / OK / Back close it, and it
 * auto-hides after NATIVE_MIXER_OVERLAY_IDLE_HIDE_MS without a key. */

/* Any interaction: push the auto-hide deadline out. */
void native_mixer_overlay_touch(App *app) {
    app->mixer_overlay_hide_ticks = SDL_GetTicks() + NATIVE_MIXER_OVERLAY_IDLE_HIDE_MS;
}

void native_mixer_overlay_hide(App *app) {
    if (!app->mixer_overlay_visible) {
        return;
    }
    /* An outside click is closed on its release, not its press, and no close may
     * complete while ANY pointer button is physically down. Any simultaneous key,
     * auto-switch or teardown request must wait for those edges too; otherwise this
     * function would re-grab evdev under a held button and leak its release into the
     * session. The overlay button-up handler completes the pending close. */
    if (app->mixer_overlay_dismiss_button != 0 || app->mixer_overlay_pointer_buttons != 0) {
        app->mixer_overlay_dismiss_pending = true;
        return;
    }
    app->mixer_overlay_visible = false;
    native_ui_mixer_hide(native_preconnect_ui_mixer(app->preconnect_ui));
    app->mixer_overlay_dragging = false;
    app->mixer_overlay_dismiss_button = 0;
    app->mixer_overlay_dismiss_pending = false;
    app->mixer_overlay_pointer_buttons = 0;
    /* Hand the pointer back to the stream: re-grab and restore the session's cursor —
     * but only while we are actually staying on a live FOCUSED stream. A hide on the way
     * to a configurator or a failure path must not re-grab (those screens own the SDL
     * mouse), and neither must an auto-hide under a webOS system overlay: grabbing there
     * would steal input from the system menu — focus regain re-grabs instead. The
     * per-tick arming derive re-arms RDP input next tick. */
    if (app->streaming_visible && !app->window_unfocused &&
        atomic_load(&native_active_slot(app)->current_state) == (int)RDP_STATE_ACTIVE) {
        (void)native_start_streaming_input(app);
        native_cursor_reassert(&native_active_slot(app)->cursor);
        /* The compositor pointer sits wherever the fader interaction left it while
         * virtual_mouse_x/y still hold the RDP position (overlay motion is consumed):
         * warp back like native_resume_streaming_input does, or the visible cursor
         * jumps on the first post-close delta. */
        SDL_Window *focus_window = SDL_GetKeyboardFocus();
        if (focus_window) {
            SDL_WarpMouseInWindow(focus_window, atomic_load(&app->virtual_mouse_x),
                                  atomic_load(&app->virtual_mouse_y));
        }
    }
    /* Re-arm the punch latch so the next tick clears the panel away. */
    native_present_invalidate(app);
}

/* A streaming teardown cannot wait for the outside-click button-up: once the
 * preconnect loop owns SDL events that edge no longer reaches the overlay handler.
 * The caller must clear streaming_visible first so this path never re-grabs input. */
void native_mixer_overlay_force_hide(App *app) {
    if (!app) {
        return;
    }
    app->mixer_overlay_dismiss_button = 0;
    app->mixer_overlay_dismiss_pending = false;
    app->mixer_overlay_pointer_buttons = 0;
    native_mixer_overlay_hide(app);
}

void native_mixer_overlay_show(App *app) {
    pthread_mutex_lock(&app->video_lock);
    bool rgba_active = app->rgba != NULL;
    pthread_mutex_unlock(&app->video_lock);
    /* The panel renders through the LVGL preconnect display only. Without it the
     * overlay would hijack keys while drawing nothing, so use the badge instead. */
    bool mixer_available = false;
    mixer_available = native_preconnect_ui_mixer(app->preconnect_ui) != NULL;
    if (rgba_active || !mixer_available) {
        /* The RemoteFX RGBA path repaints the whole renderer every frame, so the panel
         * would be invisible while its keys still hijack input: keep the badge flash. */
        native_show_session_indicator(app, atomic_load(&app->active_index));
        return;
    }
    app->mixer_overlay_visible = true;
    app->mixer_overlay_selected = atomic_load(&app->active_index);
    /* The close-by-OK release lands after hide, when the overlay no longer consumes it,
     * so the held latch can be stale here: re-arm activation for this panel session. */
    app->mixer_overlay_ok_held = false;
    /* A still-armed badge would hold its drawn latch through our hide and leave a stale
     * panel on screen until it expires; the overlay supersedes it. */
    app->indicator_slot = -1;
    native_ui_mixer_show(native_preconnect_ui_mixer(app->preconnect_ui));
    /* Hand the pointer to the SYSTEM while the panel is up: drop the evdev grab so the
     * compositor drives a visible cursor again, disarm RDP input (the per-tick derive
     * keeps it off while the overlay is visible) and show the plain arrow without
     * touching the session's cached cursor shape. Faders become click/drag targets. */
    app->mixer_overlay_dragging = false;
    app->mixer_overlay_dismiss_button = 0;
    app->mixer_overlay_dismiss_pending = false;
    /* Seed the held mask with buttons already physically down at the handoff
     * (held_mouse tracks the downs forwarded to RDP; the stop below sends their
     * synthetic ups and clears it): their real release edges arrive through the
     * overlay's SDL handler and must keep blocking the close until then. */
    app->mixer_overlay_pointer_buttons = 0;
    if (app->held_mouse[NATIVE_INPUT_BUTTON_LEFT - 1]) {
        app->mixer_overlay_pointer_buttons |= native_overlay_button_bit(SDL_BUTTON_LEFT);
    }
    if (app->held_mouse[NATIVE_INPUT_BUTTON_RIGHT - 1]) {
        app->mixer_overlay_pointer_buttons |= native_overlay_button_bit(SDL_BUTTON_RIGHT);
    }
    if (app->held_mouse[NATIVE_INPUT_BUTTON_MIDDLE - 1]) {
        app->mixer_overlay_pointer_buttons |= native_overlay_button_bit(SDL_BUTTON_MIDDLE);
    }
    native_stop_streaming_input(app);
    native_input_set_active(&app->input, false);
    native_cursor_show_default();
    /* Re-sync the MASTER fader with the platform: the remote's VOL keys or headphone
     * buttons may have moved the system volume since the overlay was last up. */
    native_luna_volume_refresh(&app->luna_volume);
    native_mixer_overlay_touch(app);
}

void native_mixer_overlay_select(App *app, int slot) {
    if (slot >= 0 && slot < NATIVE_UI_MIXER_CHANNELS) {
        app->mixer_overlay_selected = slot;
    }
    native_mixer_overlay_touch(app); /* even an edge bump keeps the overlay alive */
}

/* MASTER fader edit: absolute system volume over the Luna bus. The optimistic cache in
 * luna_volume keeps the knob tracking key repeats; the worker coalesces the bus calls. */
void native_mixer_overlay_set_master_pct(App *app, int pct) {
    native_luna_volume_set(&app->luna_volume, pct);
    native_mixer_overlay_touch(app);
}

void native_mixer_overlay_set_db(App *app, int slot, int gain_db) {
    if (slot < 0 || slot >= NATIVE_SETTINGS_MAX_SESSIONS) {
        return;
    }
    if (gain_db < NATIVE_MIXER_FADER_MIN_DB) {
        gain_db = NATIVE_MIXER_FADER_MIN_DB;
    }
    if (gain_db > NATIVE_MIXER_FADER_MAX_DB) {
        gain_db = NATIVE_MIXER_FADER_MAX_DB;
    }
    if (gain_db != (int)app->mixer_gain_db[slot]) {
        app->mixer_gain_db[slot] = (int8_t)gain_db;
        native_audio_pipeline_set_source_gain(&app->audio_pipeline, slot,
                                              native_ui_mixer_gain_db_to_q15(gain_db));
    }
    native_mixer_overlay_touch(app);
}

/* Duck-button press on a channel: flips whether that BACKGROUND session's audio ducks
 * the currently displayed session (per-foreground trigger mask), persists the new mask,
 * and retargets the pipeline's controller. The active channel's own button is inert. */
void native_mixer_overlay_toggle_duck(App *app, int slot) {
    int fg = atomic_load(&app->active_index);
    if (slot >= 0 && slot < NATIVE_SETTINGS_MAX_SESSIONS && slot != fg) {
        app->duck_mask[fg] ^= (uint8_t)(1u << slot);
        if (app->settings) {
            app->settings->sessions[fg].duck_mask = app->duck_mask[fg];
            (void)native_config_save_persisted(app->settings);
        }
        native_duck_retarget(app);
        clog(cLogLevelInfo, "duck trigger %s: %s while %s is on screen",
             (app->duck_mask[fg] >> slot) & 1u ? "enabled" : "disabled",
             native_session_slot_name(slot), native_session_slot_name(fg));
    }
    native_mixer_overlay_touch(app);
}

/* M plate click: mutes/unmutes that channel in the mix (any session slot, the active
 * one included). Runtime-only — nothing is persisted. */
void native_mixer_overlay_toggle_mute(App *app, int slot) {
    if (slot >= 0 && slot < NATIVE_SETTINGS_MAX_SESSIONS) {
        app->mixer_mute_mask ^= (uint8_t)(1u << slot);
        bool muted = (app->mixer_mute_mask >> slot) & 1u;
        native_audio_pipeline_set_source_muted(&app->audio_pipeline, slot, muted);
        clog(cLogLevelInfo, "%s %s", native_session_slot_name(slot), muted ? "muted" : "unmuted");
    }
    native_mixer_overlay_touch(app);
}

/* S plate click: toggles that channel's solo. While any solo is lit, only soloed
 * channels reach the mix (mute still wins on a muted+soloed channel). */
void native_mixer_overlay_toggle_solo(App *app, int slot) {
    if (slot >= 0 && slot < NATIVE_SETTINGS_MAX_SESSIONS) {
        app->mixer_solo_mask ^= (uint8_t)(1u << slot);
        native_publish_effective_solo_mask(app);
        clog(cLogLevelInfo, "solo %s: mask 0x%x", native_session_slot_name(slot),
             (unsigned)app->mixer_solo_mask);
    }
    native_mixer_overlay_touch(app);
}

/* OK/ENTER while the overlay is up: pressing a background session's channel flips its
 * duck button; on the active channel or the MASTER it keeps the old close behavior. */
void native_mixer_overlay_activate(App *app) {
    int slot = app->mixer_overlay_selected;
    if (slot >= 0 && slot < NATIVE_SETTINGS_MAX_SESSIONS && slot != atomic_load(&app->active_index)) {
        native_mixer_overlay_toggle_duck(app, slot);
        return;
    }
    native_mixer_overlay_hide(app);
}

void native_mixer_overlay_adjust(App *app, int delta_steps) {
    int slot = app->mixer_overlay_selected;
    if (slot == NATIVE_UI_MIXER_MASTER) {
        int pct = native_luna_volume_cached(&app->luna_volume);
        if (pct < 0) {
            /* Volume still unknown (probe in flight or bus unavailable): just re-ask. */
            native_luna_volume_refresh(&app->luna_volume);
            native_mixer_overlay_touch(app);
            return;
        }
        native_mixer_overlay_set_master_pct(app,
                                            pct + delta_steps / NATIVE_MIXER_OVERLAY_GAIN_STEP_DB *
                                                      NATIVE_UI_MIXER_MASTER_STEP_PCT);
        return;
    }
    native_mixer_overlay_set_db(app, slot, (int)app->mixer_gain_db[slot] + delta_steps);
}

/* While the overlay is open its navigation keys must not leak to the RDP session — both
 * edges are swallowed (a release reaching the server without its press confuses nobody,
 * but a swallowed press with a leaked release would). Everything else (typing on the USB
 * keyboard) passes through untouched. */
bool native_mixer_overlay_consumes_evdev_key(uint16_t code) {
    switch (code) {
    case 1:   /* KEY_ESC */
    case 28:  /* KEY_ENTER */
    case 96:  /* KEY_KPENTER */
    case 103: /* KEY_UP */
    case 105: /* KEY_LEFT */
    case 106: /* KEY_RIGHT */
    case 108: /* KEY_DOWN */
    case 158: /* KEY_BACK */
    case 352: /* KEY_OK */
        return true;
    default:
        return false;
    }
}

void native_mixer_overlay_evdev_key(App *app, uint16_t code, bool down) {
    if (!down) {
        if (code == 28 || code == 96 || code == 352) {
            app->mixer_overlay_ok_held = false; /* release edge re-arms activation */
        }
        return;
    }
    switch (code) {
    case 103: /* KEY_UP */
        native_mixer_overlay_adjust(app, NATIVE_MIXER_OVERLAY_GAIN_STEP_DB);
        break;
    case 108: /* KEY_DOWN */
        native_mixer_overlay_adjust(app, -NATIVE_MIXER_OVERLAY_GAIN_STEP_DB);
        break;
    case 105: /* KEY_LEFT */
        native_mixer_overlay_select(app, app->mixer_overlay_selected - 1);
        break;
    case 106: /* KEY_RIGHT */
        native_mixer_overlay_select(app, app->mixer_overlay_selected + 1);
        break;
    case 28:  /* KEY_ENTER */
    case 96:  /* KEY_KPENTER */
    case 352: /* KEY_OK */
        /* Edge-triggered, unlike the deliberately repeating fader arrows: evdev
         * autorepeat (value 2) folds into more down events. */
        if (!app->mixer_overlay_ok_held) {
            app->mixer_overlay_ok_held = true;
            native_mixer_overlay_activate(app);
        }
        break;
    default: /* esc/back */
        native_mixer_overlay_hide(app);
        break;
    }
}

/* Remote navigation when the remote is NOT evdev-grabbed (the SDL fallback path, same
 * split as the color keys): arrows/enter arrive as ordinary SDL keys, Back as a webOS
 * scancode. */
void native_mixer_overlay_sdl_key(App *app, const SDL_KeyboardEvent *event) {
#if LGNOME_HAVE_SDL_WEBOS_CURSOR
    if (event->keysym.scancode == SDL_WEBOS_SCANCODE_BACK || event->keysym.scancode == 482) {
        native_mixer_overlay_hide(app);
        return;
    }
#else
    if (event->keysym.scancode == 482) {
        native_mixer_overlay_hide(app);
        return;
    }
#endif
    switch (event->keysym.sym) {
    case SDLK_UP:
        native_mixer_overlay_adjust(app, NATIVE_MIXER_OVERLAY_GAIN_STEP_DB);
        break;
    case SDLK_DOWN:
        native_mixer_overlay_adjust(app, -NATIVE_MIXER_OVERLAY_GAIN_STEP_DB);
        break;
    case SDLK_LEFT:
        native_mixer_overlay_select(app, app->mixer_overlay_selected - 1);
        break;
    case SDLK_RIGHT:
        native_mixer_overlay_select(app, app->mixer_overlay_selected + 1);
        break;
    case SDLK_RETURN:
    case SDLK_KP_ENTER:
        /* Edge-triggered like the evdev path: a held Enter repeats key-downs, and a
         * repeated activate would XOR the duck bit once per repeat. */
        if (event->repeat == 0) {
            native_mixer_overlay_activate(app);
        }
        break;
    case SDLK_ESCAPE:
        native_mixer_overlay_hide(app);
        break;
    default:
        break;
    }
}

/* Arms the auto-raise detector for a (re)entered streaming screen. The detector must
 * not baseline off the cached volume: nothing polls outside streaming, so the cache can
 * still hold a pre-streaming value — the first in-stream poll reply would then read as
 * an "external change" and pop the mixer unasked. Recording the reply sequence makes
 * the tick below wait for a reply that arrived AFTER this point. */
void native_system_volume_rebaseline(App *app) {
    app->system_volume_seen = -1;
    app->system_volume_baseline_seq = native_luna_volume_reply_seq(&app->luna_volume);
}

/* Per-tick system-volume watcher. The 200ms getVolume poll keeps the cached volume
 * live, so this is mostly comparison. A change made outside the overlay (the remote's
 * VOL keys, headphone buttons) RAISES the mixer on the MASTER channel; while the
 * volume keeps moving the auto-hide timer keeps resetting, and once it settles the
 * normal idle timeout hides the panel. The user's own fader edits flow through the same
 * detector (optimistic cache updates count as changes) and simply keep the visible
 * overlay alive — same effect as any other interaction. */
void native_system_volume_tick(App *app) {
    if (!app->streaming_visible) {
        return; /* the overlay is a streaming-screen surface */
    }
    uint32_t now = SDL_GetTicks();
    if (!app->mixer_overlay_dragging && SDL_TICKS_PASSED(now, app->system_volume_poll_ticks)) {
        /* Not while dragging: a poll reply from before the drag's newest set would
         * briefly yank the knob backwards. */
        app->system_volume_poll_ticks = now + NATIVE_SYSTEM_VOLUME_POLL_MS;
        native_luna_volume_refresh(&app->luna_volume);
    }
    int pct = native_luna_volume_cached(&app->luna_volume);
    if (pct < 0) {
        return;
    }
    if (app->system_volume_seen < 0) {
        if (native_luna_volume_reply_seq(&app->luna_volume) == app->system_volume_baseline_seq) {
            return; /* cache may predate this streaming entry; wait for a fresh reply */
        }
        app->system_volume_seen = pct; /* the first fresh reading is the baseline, not a change */
        return;
    }
    if (pct == app->system_volume_seen) {
        return;
    }
    clog(cLogLevelDebug, "system volume %d -> %d (overlay %s)", app->system_volume_seen, pct,
         app->mixer_overlay_visible ? "up" : "hidden");
    app->system_volume_seen = pct;
    if (app->mixer_overlay_visible) {
        native_mixer_overlay_touch(app); /* still moving: hold the panel open */
    } else {
        native_mixer_overlay_show(app);
        native_mixer_overlay_select(app, NATIVE_UI_MIXER_MASTER);
    }
}

/* Presents the live LVGL overlay; per-tick meter updates animate with playback. It is
 * cleared back to the punch frame on hide/expiry and skipped while the RemoteFX RGBA
 * path owns the renderer (native_mixer_overlay_show refuses to open there). */

#endif
