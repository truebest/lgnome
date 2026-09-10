#ifdef LGNOME_TARGET_WEBOS

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

clog_define(g_native_log_input_route, cLogLevelInfo, "native");

/* webOS SDL color scancodes; fallback values match SDL_webOS.h. */
int native_sdl_webos_color_slot(const SDL_KeyboardEvent *event) {
    if (!event) {
        return -1;
    }
#if LGNOME_HAVE_SDL_WEBOS_CURSOR
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

/* Synthetic webOS visibility scancodes are cursor notifications, not RDP keys. */
int native_sdl_webos_cursor_visibility(const SDL_KeyboardEvent *event) {
    if (!event) {
        return -1;
    }
#if LGNOME_HAVE_SDL_WEBOS_CURSOR
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

/* Grabbed evdev bypasses compositor motion; warp the platform cursor to our logical position. */
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
                    /* Side/back opens HUB on release; forward toggles the mixer. Consume both edges. */
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
                                    /* After HUB takes input, discard the rest of this stale batch. */
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
                                /* Do not overwrite post-ungrab physical state with this older batch. */
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

/* Grabbed Magic Remote nodes deliver navigation as Linux keycodes instead of SDL scancodes. */
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
            /* A remote shortcut can hand input to HUB and stop the reader.
             * Do not route the remainder of that old batch into the new UI. */
            if (!app->evdev_input.started) {
                return;
            }
            if (events[i].from_remote) {
                /* Remote presses are sparse; keep this diagnostic because key codes
                 * vary between webOS firmware and remote models. */
                clog(cLogLevelDebug, "remote key code=%u %s", (unsigned)events[i].code,
                     events[i].down ? "down" : "up");
            }
            int color_slot = native_evdev_color_slot(events[i].code);
            if (color_slot >= 0) {
                if (events[i].down) {
                    /* This callback may stop evdev; the current batch is local and later pops check reader state. */
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
                /* Open HUB on remote release before dropping the grab; USB Enter remains an RDP key. */
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

/* Typed drains stop at the first event belonging to the other device class. */
void native_drain_evdev_input(App *app, SDL_Window *window) {
    for (;;) {
        switch (native_evdev_input_next_kind(&app->evdev_input)) {
        case NATIVE_EVENT_MOUSE:
            native_drain_evdev_mouse(app, window);
            break;
        case NATIVE_EVENT_KEYBOARD:
            native_drain_evdev_keyboard(app);
            break;
        case NATIVE_EVENT_RESET:
            if (native_evdev_input_take_reset(&app->evdev_input)) {
                native_flush_held_inputs(app);
                app->wheel_accumulator = 0;
                app->hub_open_key_held = false;
                app->hub_open_button_held = false;
            }
            break;
        default:
            return;
        }
    }
}

void native_wait_for_loop_tick(App *app, uint32_t delay_ms) {
    uint32_t timeout_ms = delay_ms == 0 ? 1u : delay_ms;
    int wake_fd = app ? native_evdev_input_wake_fd(&app->evdev_input) : -1;
    if (wake_fd >= 0) {
        /* Coalesce input wakeups to avoid running the UI loop at the USB mouse report rate. */
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

/* Mouse has an SDL fallback; keyboard does not. Distinguish absent devices from reader failure. */
NativeInputStartResult native_start_streaming_input(App *app) {
    if (!app) {
        return NATIVE_INPUT_START_UNAVAILABLE;
    }
    if (!native_evdev_input_start(&app->evdev_input)) {
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

void native_stop_streaming_input(App *app) {
    if (!app) {
        return;
    }
    /* Flush releases for anything held BEFORE dropping the grab: once the grab is gone the
     * up-events go to the overlay, not RDP, and the server would keep the drag/key stuck. */
    native_flush_held_inputs(app);
    app->hub_open_key_held = false;
    app->hub_open_button_held = false;
    native_evdev_input_stop_with_buttons(&app->evdev_input,
                                        app->mixer_overlay_visible ? &app->mixer_overlay_pointer_buttons : NULL);
}

void native_resume_streaming_input(App *app, SDL_Window *window) {
    if (!app || !app->streaming_visible ||
        atomic_load(&native_active_slot(app)->current_state) != (int)RDP_STATE_ACTIVE) {
        return;
    }
    if (app->mixer_overlay_visible) {
        /* The mixer owns SDL input until its hide path re-grabs. */
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
