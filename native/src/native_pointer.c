/* Pointer, render-size, and held-input state for the SDL thread: renderer
 * output sizing, window<->desktop input mapping, the virtual mouse the grabbed
 * evdev reader integrates into, OS-pointer clamp/warp draining, the cursor
 * reassert tick, wheel scaling, and held button/key tracking with the flush
 * that prevents stuck inputs across focus changes. SDL builds only. */
#ifdef HELLOLG_TARGET_WEBOS

#include "native_pointer.h"

#include <stdatomic.h>
#include <stdlib.h>

#include "native_app.h"

#include "clog.h"

clog_define(g_native_log_pointer, cLogLevelInfo, cLogFlags_Default, "native", NULL);

static uint16_t clamp_sdl_dimension(int value) {
    if (value <= 0) {
        return 1;
    }
    if (value > UINT16_MAX) {
        return UINT16_MAX;
    }
    return (uint16_t)value;
}


void native_log_sdl_display_modes(void) {
    int display_count = SDL_GetNumVideoDisplays();
    if (display_count <= 0) {
        clog(cLogLevelWarning, "SDL video displays unavailable: %s", SDL_GetError());
        return;
    }

    clog(cLogLevelDebug, "SDL video displays=%d", display_count);
    for (int display = 0; display < display_count; display++) {
        SDL_DisplayMode desktop_mode;
        if (SDL_GetDesktopDisplayMode(display, &desktop_mode) == 0) {
            clog(cLogLevelDebug, "SDL display %d desktop=%dx%d@%d format=0x%x", display,
                 desktop_mode.w, desktop_mode.h, desktop_mode.refresh_rate, (unsigned)desktop_mode.format);
        } else {
            clog(cLogLevelDebug, "SDL display %d desktop mode unavailable: %s", display, SDL_GetError());
        }

        SDL_DisplayMode current_mode;
        if (SDL_GetCurrentDisplayMode(display, &current_mode) == 0) {
            clog(cLogLevelDebug, "SDL display %d current=%dx%d@%d format=0x%x", display,
                 current_mode.w, current_mode.h, current_mode.refresh_rate, (unsigned)current_mode.format);
        } else {
            clog(cLogLevelDebug, "SDL display %d current mode unavailable: %s", display, SDL_GetError());
        }

        int mode_count = SDL_GetNumDisplayModes(display);
        if (mode_count < 0) {
            clog(cLogLevelDebug, "SDL display %d modes unavailable: %s", display, SDL_GetError());
            continue;
        }
        clog(cLogLevelTrace, "SDL display %d modes=%d", display, mode_count);
        for (int mode_index = 0; mode_index < mode_count; mode_index++) {
            SDL_DisplayMode mode;
            if (SDL_GetDisplayMode(display, mode_index, &mode) == 0) {
                clog(cLogLevelTrace, "SDL display %d mode %d=%dx%d@%d format=0x%x", display,
                     mode_index, mode.w, mode.h, mode.refresh_rate, (unsigned)mode.format);
            }
        }
    }
}

static bool native_read_renderer_output_size(SDL_Renderer *renderer, int *width, int *height) {
    if (!renderer) {
        return false;
    }
    int output_width = 0;
    int output_height = 0;
    if (SDL_GetRendererOutputSize(renderer, &output_width, &output_height) != 0 || output_width <= 0 || output_height <= 0) {
        clog(cLogLevelError, "SDL renderer output size unavailable: %s", SDL_GetError());
        return false;
    }
    if (width) {
        *width = output_width;
    }
    if (height) {
        *height = output_height;
    }
    return true;
}

bool native_update_render_size(App *app, SDL_Renderer *renderer) {
    int render_width = 0;
    int render_height = 0;
    if (!native_read_renderer_output_size(renderer, &render_width, &render_height)) {
        return false;
    }
    uint16_t clamped_width = clamp_sdl_dimension(render_width);
    uint16_t clamped_height = clamp_sdl_dimension(render_height);
    if (app && (atomic_load(&app->render_width) != clamped_width || atomic_load(&app->render_height) != clamped_height)) {
        atomic_store(&app->render_width, clamped_width);
        atomic_store(&app->render_height, clamped_height);
        clog(cLogLevelDebug, "SDL renderer output=%dx%d", render_width, render_height);
        pthread_mutex_lock(&app->video_lock);
        native_media_set_viewport(app->media, clamped_width, clamped_height);
        pthread_mutex_unlock(&app->video_lock);
    }
    return true;
}

/* Input mapping uses the renderer OUTPUT size as the window size. On this target they
 * are the same surface: the webOS window is fullscreen at the compositor resolution and
 * is created without HIGHDPI, so SDL's logical window coordinates equal renderer output
 * pixels. A desktop/high-DPI port would have to map input from SDL_GetWindowSize
 * instead — mouse events and SDL_WarpMouseInWindow live in logical window coordinates,
 * not pixels. */
void native_sync_input_window_size(App *app) {
    if (!app) {
        return;
    }
    uint16_t render_width = (uint16_t)atomic_load(&app->render_width);
    uint16_t render_height = (uint16_t)atomic_load(&app->render_height);
    uint16_t width = render_width ? render_width : (uint16_t)atomic_load(&app->input.window_width);
    uint16_t height = render_height ? render_height : (uint16_t)atomic_load(&app->input.window_height);
    native_input_set_window_size(&app->input, width, height);
}

/* SDL-thread-only: updates the window-size mapping and immediately re-clamps
 * virtual_mouse_x/y into the new bounds. */
void native_update_pointer_window_size(App *app) {
    native_sync_input_window_size(app);
    if (!app) {
        return;
    }
    native_set_virtual_mouse_position(app, atomic_load(&app->virtual_mouse_x), atomic_load(&app->virtual_mouse_y));
}

/* RDP-worker-thread-safe counterpart of native_update_pointer_window_size(): updates the
 * window-size mapping directly (native_input's fields are themselves atomic), but leaves
 * the actual virtual_mouse_x/y re-clamp to the SDL thread's next loop tick instead of
 * writing it here, so the SDL thread stays the sole writer of virtual_mouse_x/y. */
void native_request_pointer_window_size_update(App *app) {
    native_sync_input_window_size(app);
    if (!app) {
        return;
    }
    atomic_store(&app->pointer_clamp_pending, true);
}

void native_drain_pointer_clamp(App *app) {
    if (!app) {
        return;
    }
    if (atomic_exchange(&app->pointer_clamp_pending, false)) {
        native_set_virtual_mouse_position(app, atomic_load(&app->virtual_mouse_x), atomic_load(&app->virtual_mouse_y));
    }
}

void native_drain_pointer_warp(App *app, SDL_Window *window) {
    if (!app || !window || !atomic_exchange(&app->pointer_warp_pending, false)) {
        return;
    }
    if (atomic_load(&app->pointer_warp_slot) != atomic_load(&app->active_index)) {
        return; /* the outgoing session's warp, published across a switch: stale */
    }
    uint16_t dw = (uint16_t)atomic_load(&app->input.desktop_width);
    uint16_t dh = (uint16_t)atomic_load(&app->input.desktop_height);
    uint16_t ww = (uint16_t)atomic_load(&app->input.window_width);
    uint16_t wh = (uint16_t)atomic_load(&app->input.window_height);
    unsigned x = atomic_load(&app->pointer_warp_x);
    unsigned y = atomic_load(&app->pointer_warp_y);
    int wx = (dw != 0 && ww != 0) ? (int)((x * (unsigned)ww) / (unsigned)dw) : (int)x;
    int wy = (dh != 0 && wh != 0) ? (int)((y * (unsigned)wh) / (unsigned)dh) : (int)y;
    /* The mouse is read from grabbed evdev, so there is no SDL_MOUSEMOTION echo to update
     * virtual_mouse_x/y after the warp. Set it to the warp target directly so the next
     * relative delta integrates from here (otherwise the first move snaps the cursor back),
     * then warp the real pointer so the server cursor on the platform plane follows. */
    native_set_virtual_mouse_position(app, wx, wy);
    SDL_WarpMouseInWindow(window, wx, wy);
}

/* Apply pending server cursor shape/visibility on the SDL thread (cheap when idle). */
void native_cursor_tick(App *app) {
    if (!app) {
        return;
    }
    native_cursor_apply(&native_active_slot(app)->cursor, (uint16_t)atomic_load(&app->input.desktop_width),
                        (uint16_t)atomic_load(&app->input.desktop_height),
                        (uint16_t)atomic_load(&app->input.window_width),
                        (uint16_t)atomic_load(&app->input.window_height));
}

bool native_init_render_state(App *app, SDL_Window *window, SDL_Renderer *renderer, int *window_width,
                                     int *window_height, char *message, size_t message_cap) {
    int current_width = 0;
    int current_height = 0;
    SDL_GetWindowSize(window, &current_width, &current_height);
    clog(cLogLevelDebug, "SDL window size=%dx%d", current_width, current_height);
    if (!native_update_render_size(app, renderer)) {
        if (message && message_cap > 0) {
            (void)snprintf(message, message_cap, "Failed to read local SDL render target.");
        }
        return false;
    }
    if (window_width) {
        *window_width = current_width;
    }
    if (window_height) {
        *window_height = current_height;
    }
    native_update_pointer_window_size(app);
    native_init_virtual_mouse_position(app);
    return true;
}

void native_send_scaled_wheel(App *app, int wheel_y) {
    if (!app || wheel_y == 0) {
        return;
    }

    int units = wheel_y * (int)app->wheel_step;
    if ((app->wheel_accumulator > 0 && units < 0) || (app->wheel_accumulator < 0 && units > 0)) {
        app->wheel_accumulator = 0;
    }
    app->wheel_accumulator += units;

    const int threshold = (int)app->wheel_step * (int)app->wheel_scroll_divisor;
    int mouse_x = atomic_load(&app->virtual_mouse_x);
    int mouse_y = atomic_load(&app->virtual_mouse_y);
    while (app->wheel_accumulator >= threshold) {
        native_input_pointer_wheel(&app->input, mouse_x, mouse_y, (int16_t)app->wheel_step);
        app->wheel_accumulator -= threshold;
    }
    while (app->wheel_accumulator <= -threshold) {
        native_input_pointer_wheel(&app->input, mouse_x, mouse_y, (int16_t)-app->wheel_step);
        app->wheel_accumulator += threshold;
    }
}

int native_clamp_window_coord(int value, uint16_t limit) {
    if (value < 0) {
        return 0;
    }
    if (limit == 0) {
        return 0;
    }
    if (value >= (int)limit) {
        return (int)limit - 1;
    }
    return value;
}

void native_set_virtual_mouse_position(App *app, int x, int y) {
    if (!app) {
        return;
    }
    atomic_store(&app->virtual_mouse_x, native_clamp_window_coord(x, (uint16_t)atomic_load(&app->input.window_width)));
    atomic_store(&app->virtual_mouse_y, native_clamp_window_coord(y, (uint16_t)atomic_load(&app->input.window_height)));
}

void native_init_virtual_mouse_position(App *app) {
    if (!app) {
        return;
    }
    native_set_virtual_mouse_position(app, (int)atomic_load(&app->input.window_width) / 2,
                                       (int)atomic_load(&app->input.window_height) / 2);
}

NativeInputButton native_button_from_sdl(uint8_t button) {
    switch (button) {
    case SDL_BUTTON_LEFT:
        return NATIVE_INPUT_BUTTON_LEFT;
    case SDL_BUTTON_RIGHT:
        return NATIVE_INPUT_BUTTON_RIGHT;
    case SDL_BUTTON_MIDDLE:
        return NATIVE_INPUT_BUTTON_MIDDLE;
    default:
        return 0;
    }
}

/* Bit for tracking which pointer buttons are physically down while the mixer overlay
 * owns the pointer; buttons beyond 8 fold to 0 (untracked, never blocks a close — SDL
 * itself delivers only 1..5). */
uint8_t native_overlay_button_bit(uint8_t button) {
    return (button >= 1 && button <= 8) ? (uint8_t)(1u << (button - 1)) : 0;
}

/* Track which mouse buttons / key scancodes we have sent a down for but not yet an up, so
 * native_flush_held_inputs() can release them to the server before the evdev grab is dropped
 * on focus loss. Called right after each forwarded button/key event (evdev drains and the SDL
 * fallback). SDL thread only. */
void native_track_button(App *app, NativeInputButton button, bool down) {
    if (app && button >= NATIVE_INPUT_BUTTON_LEFT && button <= NATIVE_INPUT_BUTTON_MIDDLE) {
        app->held_mouse[button - 1] = down;
    }
}

void native_track_key(App *app, uint8_t scancode, bool extended, bool down) {
    if (!app) {
        return;
    }
    unsigned idx = (unsigned)scancode | (extended ? 0x100u : 0u);
    if (down) {
        app->held_keys[idx / 8u] |= (uint8_t)(1u << (idx % 8u));
    } else {
        app->held_keys[idx / 8u] &= (uint8_t) ~(1u << (idx % 8u));
    }
}

/* Send an up for every input still held down, so a focus loss mid-press does not strand the
 * server in a drag or an auto-repeating key once we release the grab. Runs on the SDL thread
 * while the session is still active (focus loss / background, not session teardown). */
void native_flush_held_inputs(App *app) {
    if (!app) {
        return;
    }
    int wx = atomic_load(&app->virtual_mouse_x);
    int wy = atomic_load(&app->virtual_mouse_y);
    for (int b = 0; b < 3; b++) {
        if (app->held_mouse[b]) {
            app->held_mouse[b] = false;
            native_input_pointer_button(&app->input, wx, wy, (NativeInputButton)(b + 1), false);
        }
    }
    for (unsigned idx = 0; idx < 512u; idx++) {
        if (app->held_keys[idx / 8u] & (uint8_t)(1u << (idx % 8u))) {
            app->held_keys[idx / 8u] &= (uint8_t) ~(1u << (idx % 8u));
            native_input_key(&app->input, (uint8_t)(idx & 0xffu), false, (idx & 0x100u) != 0u);
        }
    }
}

#endif
