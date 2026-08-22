/* The SDL event switch for the streaming screen: app lifecycle, focus and
 * window events, mixer-overlay pointer protocol, the compositor-pointer
 * fallback, remote color keys, and the preconnect-screen selective pump.
 * SDL builds only. */
#ifdef HELLOLG_TARGET_WEBOS

#include "native_sdl_events.h"

#include <stdatomic.h>

#include "native_app.h"
#include "native_input_route.h"
#include "native_mixer_overlay.h"
#include "native_pointer.h"
#include "native_presentation.h"
#include "native_switch.h"

#include "clog.h"

clog_define(g_native_log_sdl_events, cLogLevelInfo, cLogFlags_Default, "native", NULL);

void native_handle_sdl_event(App *app, SDL_Window *window, SDL_Renderer *renderer, const SDL_Event *event) {
    switch (event->type) {
    case SDL_QUIT:
        clog(cLogLevelNotice, "SDL_QUIT requests shutdown");
        atomic_store(&app->running, false);
        break;
    case SDL_APP_TERMINATING:
        clog(cLogLevelNotice, "SDL_APP_TERMINATING requests shutdown");
        atomic_store(&app->running, false);
        break;
    case SDL_APP_WILLENTERBACKGROUND:
        clog(cLogLevelInfo, "SDL_APP_WILLENTERBACKGROUND; keeping native session running");
        app->app_backgrounded = true;
        native_stop_streaming_input(app);
        break;
    case SDL_APP_DIDENTERBACKGROUND:
        clog(cLogLevelInfo, "SDL_APP_DIDENTERBACKGROUND; keeping native session running");
        app->app_backgrounded = true;
        native_stop_streaming_input(app);
        break;
    case SDL_APP_WILLENTERFOREGROUND:
        clog(cLogLevelDebug, "SDL_APP_WILLENTERFOREGROUND");
        app->app_backgrounded = false;
        break;
    case SDL_APP_DIDENTERFOREGROUND:
        clog(cLogLevelInfo, "SDL_APP_DIDENTERFOREGROUND");
        app->app_backgrounded = false;
        app->window_unfocused = false;
        native_resume_streaming_input(app, window);
        break;
    case SDL_WINDOWEVENT:
        if (event->window.event == SDL_WINDOWEVENT_CLOSE) {
            clog(cLogLevelNotice, "SDL window close requests shutdown");
            atomic_store(&app->running, false);
        } else if (event->window.event == SDL_WINDOWEVENT_SIZE_CHANGED || event->window.event == SDL_WINDOWEVENT_RESIZED) {
            clog(cLogLevelDebug, "SDL window size event %u: %dx%d", (unsigned)event->window.event,
                 event->window.data1, event->window.data2);
            (void)native_update_render_size(app, renderer);
            native_update_pointer_window_size(app);
        } else if (event->window.event == SDL_WINDOWEVENT_FOCUS_LOST || event->window.event == SDL_WINDOWEVENT_HIDDEN ||
                   event->window.event == SDL_WINDOWEVENT_MINIMIZED) {
            /* A remote-invoked webOS overlay (TV menu, notifications) steals input focus WITHOUT
             * fully backgrounding the app, so SDL_APP_WILLENTERBACKGROUND never fires. Release
             * the global evdev grab here too, otherwise the mouse/keyboard stay locked to us and
             * are unusable in the overlay. Re-grab happens on FOCUS_GAINED below. */
            clog(cLogLevelInfo, "window lost focus (event %u); releasing input grab",
                 (unsigned)event->window.event);
            app->window_unfocused = true;
            if (app->mixer_overlay_visible) {
                /* Focus loss can swallow the matching SDL button-up(s): the physical
                 * button state is unknowable from here on, so the held mask must not
                 * keep blocking closes after focus returns. Finishing an in-flight
                 * close is safe because input remains ungrabbed until focus returns. */
                app->mixer_overlay_pointer_buttons = 0;
                if (app->mixer_overlay_dismiss_button != 0 || app->mixer_overlay_dismiss_pending) {
                    app->mixer_overlay_dismiss_button = 0;
                    app->mixer_overlay_dismiss_pending = false;
                    native_mixer_overlay_hide(app);
                }
            }
            native_stop_streaming_input(app);
        } else if (event->window.event == SDL_WINDOWEVENT_FOCUS_GAINED || event->window.event == SDL_WINDOWEVENT_RESTORED ||
                   event->window.event == SDL_WINDOWEVENT_SHOWN) {
            clog(cLogLevelInfo, "window gained focus (event %u); re-grabbing input",
                 (unsigned)event->window.event);
            app->window_unfocused = false;
            native_resume_streaming_input(app, window);
        } else if (event->window.event == SDL_WINDOWEVENT_EXPOSED) {
            clog(cLogLevelTrace, "SDL window lifecycle event %u", (unsigned)event->window.event);
        }
        break;
    case SDL_MOUSEMOTION:
        if (app->mixer_overlay_visible) {
            /* The pointer belongs to the overlay: the evdev grab is released while it is
             * up, so the system cursor is live and these are compositor coordinates. */
            if (app->mixer_overlay_dragging) {
                int win_w = 0;
                int win_h = 0;
                SDL_GetWindowSize(window, &win_w, &win_h);
                if (app->mixer_overlay_selected == NATIVE_UI_MIXER_MASTER) {
                    native_mixer_overlay_set_master_pct(app, native_ui_mixer_fader_pct_at(win_h, event->motion.y));
                } else {
                    native_mixer_overlay_set_db(app, app->mixer_overlay_selected,
                                                native_ui_mixer_fader_db_at(win_h, event->motion.y));
                }
            }
            break;
        }
        if (app->window_unfocused) {
            break; /* an overlay/menu has focus: the released mouse must not move the RDP cursor */
        }
        if (native_evdev_input_mouse_active(&app->evdev_input)) {
            break; /* a grabbed USB mouse is read via evdev */
        }
        /* Fallback when no USB mouse was grabbed: SDL delivers the compositor's own pointer
         * (Magic Remote, trackpad remote) in absolute window coordinates. */
        native_cursor_reassert_for_activity(app);
        native_set_virtual_mouse_position(app, event->motion.x, event->motion.y);
        native_input_pointer_move(&app->input, event->motion.x, event->motion.y);
        break;
    case SDL_MOUSEBUTTONDOWN:
    case SDL_MOUSEBUTTONUP: {
        if (app->mixer_overlay_visible) {
            if (event->type == SDL_MOUSEBUTTONUP) {
                app->mixer_overlay_pointer_buttons &=
                    (uint8_t)~native_overlay_button_bit(event->button.button);
                /* Only LEFT can start a fader drag, so only its release ends one. */
                if (event->button.button == SDL_BUTTON_LEFT) {
                    app->mixer_overlay_dragging = false;
                }
                if (app->mixer_overlay_dismiss_button == event->button.button) {
                    app->mixer_overlay_dismiss_button = 0;
                    app->mixer_overlay_dismiss_pending = true;
                }
                /* Complete a pending close only on the release that leaves no pointer
                 * button down, so chords stay leak-free in any press/release order. */
                if (app->mixer_overlay_dismiss_pending &&
                    app->mixer_overlay_pointer_buttons == 0) {
                    app->mixer_overlay_dismiss_pending = false;
                    native_mixer_overlay_hide(app);
                    break;
                }
                native_mixer_overlay_touch(app);
                break;
            }
            app->mixer_overlay_pointer_buttons |=
                native_overlay_button_bit(event->button.button);
            if (event->button.button >= SDL_BUTTON_X1) {
                /* An extra button closes the overlay wherever the pointer sits. Reuse
                 * the outside-click contract: close on this button's release, so the
                 * re-grab cannot swallow the pending UP edge. Never start the close
                 * mid-drag: hiding would re-grab with LEFT still physically held and
                 * leak its release into the session as a stray pointer-up. */
                if (app->mixer_overlay_dismiss_button == 0 && !app->mixer_overlay_dragging) {
                    app->mixer_overlay_dismiss_button = event->button.button;
                }
                native_mixer_overlay_touch(app);
                break;
            }
            int win_w = 0;
            int win_h = 0;
            SDL_GetWindowSize(window, &win_w, &win_h);
            int hit_slot = -1;
            NativeUiMixerHit zone = NATIVE_UI_MIXER_HIT_BODY;
            if (!native_ui_mixer_hit_test(win_w, win_h, event->button.x, event->button.y, &hit_slot, &zone)) {
                /* Keep the overlay alive until this same button is released. Closing
                 * on DOWN would immediately restore the grab and could leak the UP edge
                 * into the RDP session as a stray pointer release. */
                if (app->mixer_overlay_dismiss_button == 0) {
                    app->mixer_overlay_dismiss_button = event->button.button;
                }
                app->mixer_overlay_dragging = false;
                native_mixer_overlay_touch(app);
                break;
            }
            if (event->button.button != SDL_BUTTON_LEFT) {
                native_mixer_overlay_touch(app);
                break;
            }
            if (hit_slot >= 0) {
                native_mixer_overlay_select(app, hit_slot);
                if (zone == NATIVE_UI_MIXER_HIT_MUTE && hit_slot < NATIVE_SETTINGS_MAX_SESSIONS) {
                    native_mixer_overlay_toggle_mute(app, hit_slot);
                } else if (zone == NATIVE_UI_MIXER_HIT_SOLO && hit_slot < NATIVE_SETTINGS_MAX_SESSIONS) {
                    native_mixer_overlay_toggle_solo(app, hit_slot);
                } else if (zone == NATIVE_UI_MIXER_HIT_DUCK && hit_slot < NATIVE_SETTINGS_MAX_SESSIONS) {
                    /* A click on the switch toggles it (inert for the active channel);
                     * no drag state — the switch has no fader travel. */
                    native_mixer_overlay_toggle_duck(app, hit_slot);
                } else if (zone == NATIVE_UI_MIXER_HIT_FADER) {
                    if (hit_slot == NATIVE_UI_MIXER_MASTER) {
                        native_mixer_overlay_set_master_pct(app,
                                                            native_ui_mixer_fader_pct_at(win_h, event->button.y));
                    } else {
                        native_mixer_overlay_set_db(app, hit_slot,
                                                    native_ui_mixer_fader_db_at(win_h, event->button.y));
                    }
                    app->mixer_overlay_dragging = true;
                }
            } else {
                native_mixer_overlay_touch(app);
            }
            break;
        }
        if (app->window_unfocused) {
            break;
        }
        if (native_evdev_input_mouse_active(&app->evdev_input)) {
            break;
        }
        if (event->button.button >= SDL_BUTTON_X1) {
            /* Compositor-pointer fallback for the same extra-button UI shortcuts as the
             * evdev drain; the overlay-visible branch above owns the closing half. */
            if (event->button.button == SDL_BUTTON_X1) {
                if (event->type == SDL_MOUSEBUTTONDOWN) {
                    if (app->streaming_visible) {
                        app->hub_open_button_held = true;
                    }
                } else {
                    bool opened_from_this_press = app->hub_open_button_held;
                    app->hub_open_button_held = false;
                    if (opened_from_this_press && app->streaming_visible) {
                        native_show_hub(app);
                    }
                }
            } else if (event->type == SDL_MOUSEBUTTONDOWN && app->streaming_visible) {
                native_mixer_overlay_show(app);
            }
            break;
        }
        NativeInputButton button = native_button_from_sdl(event->button.button);
        if (button != 0) {
            native_set_virtual_mouse_position(app, event->button.x, event->button.y);
            native_input_pointer_button(&app->input, atomic_load(&app->virtual_mouse_x),
                                        atomic_load(&app->virtual_mouse_y), button,
                                        event->type == SDL_MOUSEBUTTONDOWN);
            native_track_button(app, button, event->type == SDL_MOUSEBUTTONDOWN);
        }
        break;
    }
    case SDL_MOUSEWHEEL: {
        if (app->mixer_overlay_visible) {
            int overlay_wheel_y = event->wheel.y;
#if SDL_VERSION_ATLEAST(2, 0, 4)
            if (event->wheel.direction == SDL_MOUSEWHEEL_FLIPPED) {
                overlay_wheel_y = -overlay_wheel_y;
            }
#endif
            if (overlay_wheel_y != 0) {
                native_mixer_overlay_adjust(app, overlay_wheel_y * NATIVE_MIXER_OVERLAY_GAIN_STEP_DB);
            }
            break;
        }
        if (app->window_unfocused) {
            break;
        }
        if (native_evdev_input_mouse_active(&app->evdev_input)) {
            break;
        }
        int wheel_y = event->wheel.y;
#if SDL_VERSION_ATLEAST(2, 0, 4)
        if (event->wheel.direction == SDL_MOUSEWHEEL_FLIPPED) {
            wheel_y = -wheel_y;
        }
#endif
        native_send_scaled_wheel(app, wheel_y);
        break;
    }
    case SDL_KEYDOWN:
    case SDL_KEYUP:
        /* The USB keyboard is read from grabbed /dev/input (input_evdev); there is no SDL
         * keyboard path. SDL keys here belong to the ungrabbed webOS remote: its colour
         * keys navigate sessions, central OK opens HUB, and the channel rocker rings
         * connected slots. App exit is system-driven (webOS EXIT/home -> SDL_QUIT /
         * SDL_APP_TERMINATING above).
         *
         * The mouse, by contrast, keeps an SDL fallback above: a grabbed USB mouse is read via
         * evdev, but with no USB mouse SDL still delivers the compositor pointer (Magic Remote). */
        if (event->type == SDL_KEYDOWN) {
            /* Only the ungrabbed remote (and system) reaches SDL here — the USB keyboard
             * is evdev-grabbed — so these are sparse; logging them maps what a given
             * remote firmware actually sends. */
            int platform_cursor_visible = native_sdl_webos_cursor_visibility(&event->key);
            if (platform_cursor_visible >= 0) {
                NativeCursor *cursor = &native_active_slot(app)->cursor;
                native_cursor_note_platform_visibility(cursor, platform_cursor_visible != 0);
                bool stream_owns_cursor = app->streaming_visible && !app->window_unfocused &&
                                          !app->mixer_overlay_visible;
                if (stream_owns_cursor) {
                    if (platform_cursor_visible == 0) {
                        app->cursor_reassert_pending = true;
                    } else {
                        /* If the server wants HIDDEN, undo an out-of-band platform show;
                         * otherwise restore the cached shape, not merely a generic arrow. */
                        native_cursor_reassert(cursor);
                        app->cursor_last_reassert_ticks = SDL_GetTicks();
                    }
                }
                break;
            }
            clog(cLogLevelTrace, "remote sdl key scancode=%d", (int)event->key.keysym.scancode);
            int slot = native_sdl_webos_color_slot(&event->key);
            if (slot >= 0) {
                native_request_session_switch(app, slot);
            } else if (app->mixer_overlay_visible) {
                native_mixer_overlay_sdl_key(app, &event->key);
            } else if (app->streaming_visible && event->key.repeat == 0 &&
                       native_sdl_confirm_key(&event->key)) {
                /* SDL is the fallback for remote nodes the evdev grab does not own.
                 * A single central-button down opens HUB; its release is ignored. */
                native_show_hub(app);
            } else if (event->key.keysym.scancode == 480 /* SDL_WEBOS_SCANCODE_CH_UP */ ||
                       event->key.keysym.scancode == 481 /* SDL_WEBOS_SCANCODE_CH_DOWN */) {
                /* The channel rocker rings through the connected slots — channel-zapping
                 * semantics. It has no meaning inside an RDP session, so nothing is
                 * taken away from the remote desktop. */
                native_ring_switch(app, event->key.keysym.scancode == 480 ? 1 : -1);
            }
        }
        break;
    default:
        break;
    }

    (void)window;
}

void native_pump_preconnect_system_events(App *app, SDL_Window *window, SDL_Renderer *renderer, NativePreconnectUi *ui,
                                                 int *window_width, int *window_height) {
    SDL_Event event;
    SDL_PumpEvents();

    while (SDL_PeepEvents(&event, 1, SDL_GETEVENT, SDL_QUIT, SDL_QUIT) > 0) {
        native_handle_sdl_event(app, window, renderer, &event);
    }

    while (SDL_PeepEvents(&event, 1, SDL_GETEVENT, SDL_APP_TERMINATING, SDL_APP_DIDENTERFOREGROUND) > 0) {
        native_handle_sdl_event(app, window, renderer, &event);
    }

    SDL_FilterEvents(native_filter_webos_system_keys, app);

    while (SDL_PeepEvents(&event, 1, SDL_GETEVENT, SDL_WINDOWEVENT, SDL_WINDOWEVENT) > 0) {
        native_handle_sdl_event(app, window, renderer, &event);
        if (event.window.event == SDL_WINDOWEVENT_SIZE_CHANGED || event.window.event == SDL_WINDOWEVENT_RESIZED) {
            *window_width = event.window.data1;
            *window_height = event.window.data2;
            native_preconnect_ui_resize(ui, event.window.data1, event.window.data2);
        }
    }

    while (SDL_PeepEvents(&event, 1, SDL_GETEVENT, SDL_MOUSEWHEEL, SDL_MOUSEWHEEL) > 0) {
    }
}

#endif
