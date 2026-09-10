#ifndef LGNOME_NATIVE_POINTER_H
#define LGNOME_NATIVE_POINTER_H

/* SDL-thread pointer/render-size/held-input helpers (see native_pointer.c).
 * SDL builds only. */
#ifdef LGNOME_TARGET_WEBOS

#include <SDL.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "input_sdl.h"

typedef struct App App;

void native_log_sdl_display_modes(void);
bool native_update_render_size(App *app, SDL_Renderer *renderer);
void native_sync_input_window_size(App *app);
void native_update_pointer_window_size(App *app);
void native_request_pointer_window_size_update(App *app);
void native_drain_pointer_clamp(App *app);
void native_drain_pointer_warp(App *app, SDL_Window *window);
void native_cursor_tick(App *app);
bool native_init_render_state(App *app, SDL_Window *window, SDL_Renderer *renderer, int *window_width,
                              int *window_height, char *message, size_t message_cap);
void native_send_scaled_wheel(App *app, int wheel_y);
int native_clamp_window_coord(int value, uint16_t limit);
void native_set_virtual_mouse_position(App *app, int x, int y);
void native_init_virtual_mouse_position(App *app);
NativeInputButton native_button_from_sdl(uint8_t button);
uint8_t native_overlay_button_bit(uint8_t button);
void native_track_button(App *app, NativeInputButton button, bool down);
void native_track_key(App *app, uint8_t scancode, bool extended, bool down);
void native_flush_held_inputs(App *app);

#endif
#endif
