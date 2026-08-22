/* Input routing for the SDL thread: decoding webOS remote keys from SDL,
 * the system-key event filter, the grabbed-evdev mouse/keyboard drains (with
 * their HUB/mixer shortcuts), loop-tick pacing against the evdev wake fd, and
 * the streaming-input grab lifecycle. SDL builds only. */
#ifdef HELLOLG_TARGET_WEBOS

#include "native_input_route.h"

#include <errno.h>
#include <poll.h>
#include <stdatomic.h>
#include <stdlib.h>

#include "native_app.h"
#include "native_mixer_overlay.h"
#include "native_pointer.h"
#include "native_switch.h"

#include "clog.h"

clog_define(g_native_log_input_route, cLogLevelInfo, cLogFlags_Default, "native", NULL);

/* Color buttons on the TV remote select the session slot with the same name, in the
 * remote's own order: red, green, yellow, blue. The remote is deliberately NOT
 * evdev-grabbed, so these arrive as SDL scancodes (sysroot SDL_webOS.h; 486..489 are the
 * literal values for builds without that header). */
int native_sdl_webos_color_slot(const SDL_KeyboardEvent *event) {
    if (!event) {
        return -1;
    }
#if HELLOLG_HAVE_SDL_WEBOS_CURSOR
    if (event->keysym.scancode == SDL_WEBOS_SCANCODE_RED || event->keysym.scancode == 486) {
        return NATIVE_SESSION_SLOT_RED;
    }
    if (event->keysym.scancode == SDL_WEBOS_SCANCODE_GREEN || event->keysym.scancode == 487) {
        return NATIVE_SESSION_SLOT_GREEN;
    }
    if (event->keysym.scancode == SDL_WEBOS_SCANCODE_YELLOW || event->keysym.scancode == 488) {
        return NATIVE_SESSION_SLOT_YELLOW;
    }
    if (event->keysym.scancode == SDL_WEBOS_SCANCODE_BLUE || event->keysym.scancode == 489) {
        return NATIVE_SESSION_SLOT_BLUE;
    }
#else
    if (event->keysym.scancode == 486) {
        return NATIVE_SESSION_SLOT_RED;
    }
    if (event->keysym.scancode == 487) {
        return NATIVE_SESSION_SLOT_GREEN;
    }
    if (event->keysym.scancode == 488) {
        return NATIVE_SESSION_SLOT_YELLOW;
    }
    if (event->keysym.scancode == 489) {
        return NATIVE_SESSION_SLOT_BLUE;
    }
#endif
    return -1;
}

/* webOS reports Magic Remote/LSM pointer visibility as synthetic key scancodes. They are
 * not RDP keyboard input: use them to keep the cursor-plane diagnostic state honest and
 * to arm recovery when the platform hides a cursor the server still considers visible. */
int native_sdl_webos_cursor_visibility(const SDL_KeyboardEvent *event) {
    if (!event) {
        return -1;
    }
#if HELLOLG_HAVE_SDL_WEBOS_CURSOR
    if (event->keysym.scancode == SDL_WEBOS_SCANCODE_CURSOR_SHOW) {
        return 1;
    }
    if (event->keysym.scancode == SDL_WEBOS_SCANCODE_CURSOR_HIDE) {
        return 0;
    }
#else
    if (event->keysym.scancode == 484) {
        return 1;
    }
    if (event->keysym.scancode == 485) {
        return 0;
    }
#endif
    return -1;
}

bool native_sdl_confirm_key(const SDL_KeyboardEvent *event) {
    if (!event) {
        return false;
    }
    return event->keysym.sym == SDLK_RETURN || event->keysym.sym == SDLK_KP_ENTER ||
           event->keysym.sym == SDLK_RETURN2;
}

/* Volume-mixer overlay key hooks for the evdev drain (definitions live with the overlay
 * code, after the presenters they use). */

/* Pre-connect screen: swallow session-navigation keys before the LVGL key queue eats
 * them. Onboarding temporarily owns the remote; after that, colours activate their
 * fixed profiles and CH +/- moves the hub selection. */
int native_filter_webos_system_keys(void *userdata, SDL_Event *event) {
    App *app = (App *)userdata;
    if (!event || (event->type != SDL_KEYDOWN && event->type != SDL_KEYUP)) {
        return 1;
    }
    int slot = native_sdl_webos_color_slot(&event->key);
    if (slot >= 0) {
        if (event->type == SDL_KEYDOWN && app) {
            if (!native_preconnect_ui_session_keys_enabled(app->preconnect_ui)) {
                return 0;
            }
            native_preconnect_ui_cancel_pending_navigation(app->preconnect_ui);
            native_request_session_switch(app, slot);
        }
        return 0;
    }
    if (event->key.keysym.scancode == 480 /* SDL_WEBOS_SCANCODE_CH_UP */ ||
        event->key.keysym.scancode == 481 /* SDL_WEBOS_SCANCODE_CH_DOWN */) {
        if (event->type == SDL_KEYDOWN && app && native_preconnect_ui_session_keys_enabled(app->preconnect_ui)) {
            (void)native_preconnect_ui_cycle_slot(app->preconnect_ui,
                                                  event->key.keysym.scancode == 480 ? 1 : -1);
        }
        return 0;
    }
    return 1;
}

#define NATIVE_EVDEV_MOUSE_BATCH 64u
#define NATIVE_EVDEV_KEYBOARD_BATCH 64u

static void native_flush_pending_evdev_motion(App *app, int *pending_dx, int *pending_dy, bool *moved) {
    if (!app || !pending_dx || !pending_dy || (*pending_dx == 0 && *pending_dy == 0)) {
        return;
    }
    native_set_virtual_mouse_position(app, atomic_load(&app->virtual_mouse_x) + *pending_dx,
                                      atomic_load(&app->virtual_mouse_y) + *pending_dy);
    native_input_pointer_move(&app->input, atomic_load(&app->virtual_mouse_x), atomic_load(&app->virtual_mouse_y));
    *pending_dx = 0;
    *pending_dy = 0;
    if (moved) {
        *moved = true;
    }
}

void native_cursor_reassert_for_activity(App *app) {
    if (!app) {
        return;
    }
    uint32_t now = SDL_GetTicks();
    bool after_idle = app->cursor_last_activity_ticks == 0 ||
                      now - app->cursor_last_activity_ticks >= NATIVE_CURSOR_ACTIVITY_IDLE_MS;
    bool heartbeat_due =
        app->cursor_last_reassert_ticks == 0 ||
        now - app->cursor_last_reassert_ticks >= NATIVE_CURSOR_ACTIVITY_HEARTBEAT_MS;
    app->cursor_last_activity_ticks = now;
    if (!app->cursor_reassert_pending && !after_idle && !heartbeat_due) {
        return;
    }

    bool explicitly_pending = app->cursor_reassert_pending;
    native_cursor_reassert(&native_active_slot(app)->cursor);
    app->cursor_reassert_pending = false;
    app->cursor_last_reassert_ticks = now;
    if (explicitly_pending) {
        clog(cLogLevelDebug, "reasserted server cursor on pointer activity after platform/focus hide");
    }
}

/* SDL thread: apply queued raw-evdev mouse events. Relative motion is integrated into the
 * logical pointer and sent as an absolute server position; buttons/wheel are sent at the
 * current position. Because the mouse is grabbed, the compositor no longer moves the OS
 * pointer, so we warp it to the logical position to make the server cursor (drawn on the
 * platform cursor plane) follow. */
void native_drain_evdev_mouse(App *app, SDL_Window *window) {
    if (!native_evdev_input_mouse_active(&app->evdev_input)) {
        return;
    }
    bool moved = false;
    int pending_dx = 0;
    int pending_dy = 0;
    NativeMouseEv events[NATIVE_EVDEV_MOUSE_BATCH];
    for (;;) {
        size_t count = native_evdev_input_pop_mouse_batch(&app->evdev_input, events, NATIVE_EVDEV_MOUSE_BATCH);
        if (count == 0) {
            break;
        }
        for (size_t i = 0; i < count; i++) {
            NativeMouseEv *ev = &events[i];
            switch (ev->kind) {
            case NATIVE_MOUSE_EV_MOTION:
                pending_dx += ev->dx;
                pending_dy += ev->dy;
                break;
            case NATIVE_MOUSE_EV_BUTTON: {
                native_flush_pending_evdev_motion(app, &pending_dx, &pending_dy, &moved);
                if (ev->sdl_button >= SDL_BUTTON_X1) {
                    /* Extra mouse buttons never reach the server (native_button_from_sdl
                     * maps only left/right/middle), so they drive the UI instead: the
                     * side/back button opens HUB (the central remote button's contract:
                     * on release, so the grab drop cannot hand the press's second half
                     * to the compositor) and the extra/forward button toggles the
                     * volume mixer (the active color key's contract). Swallow both
                     * edges either way. */
                    bool opens_hub =
                        ev->sdl_button == SDL_BUTTON_X1 || ev->sdl_button == SDL_BUTTON_X2 + 2;
                    if (opens_hub) {
                        if (ev->down) {
                            app->hub_open_button_held = true;
                        } else {
                            bool opened_from_this_press = app->hub_open_button_held;
                            app->hub_open_button_held = false;
                            if (opened_from_this_press && app->streaming_visible &&
                                !app->mixer_overlay_visible) {
                                native_show_hub(app);
                                if (app->hub_visible) {
                                    /* HUB owns input now: the show dropped the evdev
                                     * grab, so the rest of this batch is stale. The
                                     * show self-guards (no preconnect UI, overlay up):
                                     * when it declined, keep draining normally. */
                                    return;
                                }
                            }
                        }
                        break;
                    }
                    if (ev->down) {
                        if (app->mixer_overlay_visible) {
                            native_mixer_overlay_hide(app);
                        } else if (app->streaming_visible) {
                            native_mixer_overlay_show(app);
                            if (app->mixer_overlay_visible) {
                                /* The show dropped the evdev grab and disarmed RDP
                                 * input: forwarding the rest of this batch would track
                                 * stray held-button state and fight the overlay's
                                 * cursor. Fold its button edges into the overlay's
                                 * held mask instead — they predate the handoff and
                                 * will never reach SDL. (The RGBA badge-only path
                                 * leaves the overlay hidden and keeps draining.) */
                                for (size_t j = i + 1; j < count; j++) {
                                    if (events[j].kind != NATIVE_MOUSE_EV_BUTTON) {
                                        continue;
                                    }
                                    uint8_t bit = native_overlay_button_bit(events[j].sdl_button);
                                    if (events[j].down) {
                                        app->mixer_overlay_pointer_buttons |= bit;
                                    } else {
                                        app->mixer_overlay_pointer_buttons &= (uint8_t)~bit;
                                    }
                                }
                                return;
                            }
                        }
                    }
                    break;
                }
                NativeInputButton button = native_button_from_sdl(ev->sdl_button);
                if (button != 0) {
                    native_input_pointer_button(&app->input, atomic_load(&app->virtual_mouse_x),
                                                atomic_load(&app->virtual_mouse_y), button, ev->down);
                    native_track_button(app, button, ev->down);
                }
                break;
            }
            case NATIVE_MOUSE_EV_WHEEL:
                native_flush_pending_evdev_motion(app, &pending_dx, &pending_dy, &moved);
                if (ev->wheel_y != 0) {
                    native_send_scaled_wheel(app, ev->wheel_y);
                }
                break;
            }
        }
    }
    native_flush_pending_evdev_motion(app, &pending_dx, &pending_dy, &moved);
    if (moved) {
        native_cursor_reassert_for_activity(app);
        if (window) {
            int wx = atomic_load(&app->virtual_mouse_x);
            int wy = atomic_load(&app->virtual_mouse_y);
            SDL_WarpMouseInWindow(window, wx, wy);
        }
    }
}

static void native_log_unmapped_evdev_key(uint16_t code, bool down) {
    clog_limited(cLogLevelDebug, 16, 1000, "unmapped evdev key %s code=%u",
                 down ? "down" : "up", (unsigned)code);
}

/* SDL thread: apply queued raw-evdev keyboard events. Each Linux keycode is translated to its
 * RDP set-1 scancode (+E0 flag) and injected as a scancode key event; there is no unicode/IME
 * path (the grabbed keyboard delivers physical scancodes only), so no TEXTINPUT suppression is
 * needed. Unlike the SDL path, typing does NOT hide the pointer: with the mouse grabbed we
 * own the cursor outright, so its visibility follows the RDP server's pointer state alone. A
 * local hide-on-keypress would only fight that (and is what made the cursor vanish mid-typing
 * before). */
/* The Magic Remote's virtual input nodes ("Smart Remote RCU Input", "LGE Network
 * Input") qualify as relative mice and therefore ARE evdev-grabbed during streaming, so
 * the remote's color keys arrive here as Linux key codes instead of reaching SDL through
 * the compositor. Map them to session-slot navigation, same as the SDL scancode path. */
static int native_evdev_color_slot(uint16_t code) {
    switch (code) {
    case 398: /* KEY_RED */
        return NATIVE_SESSION_SLOT_RED;
    case 399: /* KEY_GREEN */
        return NATIVE_SESSION_SLOT_GREEN;
    case 400: /* KEY_YELLOW */
        return NATIVE_SESSION_SLOT_YELLOW;
    case 401: /* KEY_BLUE */
        return NATIVE_SESSION_SLOT_BLUE;
    default:
        return -1;
    }
}

static bool native_evdev_confirm_key(uint16_t code) {
    return code == 28 /* KEY_ENTER */ || code == 96 /* KEY_KPENTER */ || code == 352 /* KEY_OK */;
}

void native_drain_evdev_keyboard(App *app) {
    if (!native_evdev_input_keyboard_active(&app->evdev_input)) {
        return;
    }
    NativeKeyboardEv events[NATIVE_EVDEV_KEYBOARD_BATCH];
    for (;;) {
        size_t count =
            native_evdev_input_pop_keyboard_batch(&app->evdev_input, events, NATIVE_EVDEV_KEYBOARD_BATCH);
        if (count == 0) {
            break;
        }
        for (size_t i = 0; i < count; i++) {
            if (events[i].from_remote) {
                /* Remote presses are sparse; keep this diagnostic because key codes
                 * vary between webOS firmware and remote models. */
                clog(cLogLevelDebug, "remote key code=%u %s", (unsigned)events[i].code,
                     events[i].down ? "down" : "up");
            }
            int color_slot = native_evdev_color_slot(events[i].code);
            if (color_slot >= 0) {
                if (events[i].down) {
                    /* May stop evdev input from under this drain (the configurator and
                     * mixer-overlay paths release the grab). That is safe by
                     * construction: the rest of THIS batch lives in the local array, a
                     * repeated stop early-returns on !lock_initialized, and the next
                     * pop_keyboard_batch returns 0 via its !started guard without
                     * touching the destroyed lock. */
                    native_request_session_switch(app, color_slot);
                }
                continue;
            }
            if (app->mixer_overlay_visible && native_mixer_overlay_consumes_evdev_key(events[i].code)) {
                /* Releases flow through too: the activation edge detector re-arms on
                 * the up edge (both edges stay swallowed either way). */
                native_mixer_overlay_evdev_key(app, events[i].code, events[i].down);
                continue;
            }
            if (events[i].from_remote && native_evdev_confirm_key(events[i].code)) {
                /* The central remote button owns HUB, but Enter from a physical USB
                 * keyboard (from_remote=false) remains an ordinary RDP key. Swallow
                 * both edges. Open on release so native_show_hub can drop EVIOCGRAB
                 * only after the compositor can no longer inherit the other half of
                 * this press and reinterpret it as BACK. Autorepeat merely keeps the
                 * held latch armed. */
                if (events[i].down) {
                    app->hub_open_key_held = true;
                } else {
                    bool opened_from_this_press = app->hub_open_key_held;
                    app->hub_open_key_held = false;
                    if (opened_from_this_press && app->streaming_visible) {
                        native_show_hub(app);
                    }
                }
                continue;
            }
            if (events[i].code == 402 /* KEY_CHANNELUP */ || events[i].code == 403 /* KEY_CHANNELDOWN */) {
                /* Channel-zapping between the connected slots; like the color keys these
                 * may ride either input path depending on the remote firmware. */
                if (events[i].down && app->streaming_visible) {
                    native_ring_switch(app, events[i].code == 402 ? 1 : -1);
                }
                continue;
            }
            uint8_t rdp_scancode = 0;
            bool extended = false;
            if (!native_input_linux_keycode_to_rdp(events[i].code, &rdp_scancode, &extended)) {
                native_log_unmapped_evdev_key(events[i].code, events[i].down);
                continue;
            }
            bool sent = native_input_key(&app->input, rdp_scancode, events[i].down, extended);
            if (sent) {
                native_track_key(app, rdp_scancode, extended, events[i].down);
            }
        }
    }
}

void native_wait_for_loop_tick(App *app, uint32_t delay_ms) {
    uint32_t timeout_ms = delay_ms == 0 ? 1u : delay_ms;
    int wake_fd = app ? native_evdev_input_wake_fd(&app->evdev_input) : -1;
    if (wake_fd >= 0) {
        /* Input can wake the loop before the full frame timeout so a keystroke/click is not
         * gated by the render sleep. But a 500-1000 Hz USB mouse would otherwise wake it on
         * every report and spin the whole loop (UI tick, present checks, SDL pump) at report
         * rate. So once input has arrived, process it after at most min_interval_ms (bounding
         * added latency to half a frame and the loop to ~2x/frame); while idle, wait the full
         * frame timeout as before. Bursty input coalesces in the reader's ring meanwhile. */
        const uint32_t min_interval_ms = timeout_ms > 2u ? timeout_ms / 2u : 0u;
        uint32_t start = SDL_GetTicks();
        bool woke = false;
        for (;;) {
            uint32_t elapsed = SDL_GetTicks() - start;
            uint32_t deadline_ms = woke ? min_interval_ms : timeout_ms;
            if (elapsed >= deadline_ms) {
                return; /* deadline reached: run the loop (processing any coalesced input) */
            }
            struct pollfd pfd;
            pfd.fd = wake_fd;
            pfd.events = POLLIN;
            pfd.revents = 0;
            int ret;
            do {
                ret = poll(&pfd, 1, (int)(deadline_ms - elapsed));
            } while (ret < 0 && errno == EINTR);
            if (ret <= 0) {
                return; /* deadline elapsed (or poll error): run the loop */
            }
            if (!(pfd.revents & POLLIN)) {
                return; /* unexpected condition on the wake fd (POLLERR/HUP): don't spin on it */
            }
            native_evdev_input_clear_wake(&app->evdev_input);
            /* First input this wait: switch to the min-interval deadline so it is processed
             * promptly; further wakes before then just coalesce. */
            woke = true;
        }
    }
    SDL_Delay((Uint32)timeout_ms);
}

/* Start the evdev input readers when streaming becomes active. The mouse degrades to the SDL
 * compositor-pointer path (Magic Remote) when no USB mouse is present, so a failed mouse grab
 * is informational; a failed keyboard grab means NO keyboard input (there is no SDL keyboard
 * fallback), so it is a loud warning. Returns NATIVE_INPUT_START_OK when the keyboard reader is
 * active, NATIVE_INPUT_START_NO_KEYBOARD when there is simply no keyboard to grab, and
 * NATIVE_INPUT_START_UNAVAILABLE when the reader could not start at all (so callers do not
 * misreport a resource failure as an absent keyboard). */
NativeInputStartResult native_start_streaming_input(App *app) {
    if (!app) {
        return NATIVE_INPUT_START_UNAVAILABLE;
    }
    if (!native_evdev_input_start(&app->evdev_input)) {
        /* The reader failed to come up; the grab path cannot tell us whether a keyboard exists.
         * Probe /dev/input directly so an eventfd/thread failure with a keyboard attached is
         * reported as a resource failure, not "no keyboard detected". */
        if (native_evdev_input_probe_keyboard()) {
            clog(cLogLevelWarning,
                 "input capture failed to start even though a USB keyboard is attached; this session has no "
                 "mouse or keyboard input");
            return NATIVE_INPUT_START_UNAVAILABLE;
        }
        clog(cLogLevelWarning,
             "no USB mouse/keyboard grabbed; using SDL mouse fallback and no keyboard input");
        return NATIVE_INPUT_START_NO_KEYBOARD;
    }
    if (!native_evdev_input_mouse_active(&app->evdev_input)) {
        clog(cLogLevelInfo,
             "no USB mouse to grab; using the compositor pointer (Magic Remote) via SDL");
    }
    if (!native_evdev_input_keyboard_active(&app->evdev_input)) {
        clog(cLogLevelWarning,
             "no USB keyboard grabbed and there is no SDL keyboard fallback; this session has no keyboard "
             "input until a USB keyboard is attached (it is picked up live)");
        return NATIVE_INPUT_START_NO_KEYBOARD;
    }
    return NATIVE_INPUT_START_OK;
}

/* Release the evdev grabs (mouse + keyboard). Safe when nothing is grabbed (idempotent). Used
 * on focus loss / backgrounding: EVIOCGRAB is a GLOBAL capture, so a running-but-unfocused app
 * must not keep holding it or the webOS home UI / TV overlay menus get no mouse or keyboard. */
void native_stop_streaming_input(App *app) {
    if (!app) {
        return;
    }
    /* Flush releases for anything held BEFORE dropping the grab: once the grab is gone the
     * up-events go to the overlay, not RDP, and the server would keep the drag/key stuck. */
    native_flush_held_inputs(app);
    app->hub_open_key_held = false;
    app->hub_open_button_held = false;
    native_evdev_input_stop(&app->evdev_input);
}

/* Re-acquire input after regaining focus / returning to foreground, but only on the streaming
 * screen (the preconnect UI needs the SDL mouse, so we must not grab there). Besides re-grabbing,
 * re-assert the server cursor and re-home the OS pointer: a webOS overlay leaves the platform
 * pointer hidden behind our back, so without this the RDP cursor stays invisible until a big
 * mouse sweep. */
void native_resume_streaming_input(App *app, SDL_Window *window) {
    if (!app || !app->streaming_visible ||
        atomic_load(&native_active_slot(app)->current_state) != (int)RDP_STATE_ACTIVE) {
        return;
    }
    if (app->mixer_overlay_visible) {
        /* The overlay deliberately released the grab so SDL delivers fader clicks;
         * re-grabbing on focus regain would steal the pointer back from it. Its hide
         * path re-grabs (focus-gated) once it closes. */
        return;
    }
    (void)native_start_streaming_input(app);
    native_cursor_reassert(&native_active_slot(app)->cursor);
    app->cursor_last_reassert_ticks = SDL_GetTicks();
    /* Also re-assert on the next real mouse movement — see cursor_reassert_pending. */
    app->cursor_reassert_pending = true;
    if (window) {
        SDL_WarpMouseInWindow(window, atomic_load(&app->virtual_mouse_x), atomic_load(&app->virtual_mouse_y));
    }
}

#endif
