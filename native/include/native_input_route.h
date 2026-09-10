#ifndef LGNOME_NATIVE_INPUT_ROUTE_H
#define LGNOME_NATIVE_INPUT_ROUTE_H

/* Streaming-input routing: the evdev grab lifecycle and the SDL/evdev drains
 * (drains still in main.c pending extraction). SDL builds only. */
#ifdef LGNOME_TARGET_WEBOS

#include <SDL.h>
#include <stdbool.h>
#include <stdint.h>

typedef struct App App;

typedef enum NativeInputStartResult {
    NATIVE_INPUT_START_OK,          /* keyboard reader is active */
    NATIVE_INPUT_START_NO_KEYBOARD, /* readers up (or nothing attached) but no keyboard to grab */
    NATIVE_INPUT_START_UNAVAILABLE, /* the input subsystem itself could not start (eventfd/thread) */
} NativeInputStartResult;

NativeInputStartResult native_start_streaming_input(App *app);
void native_stop_streaming_input(App *app);
void native_resume_streaming_input(App *app, SDL_Window *window);

/* SDL remote-key decoding (color slots, cursor-visibility pseudo-keys, OK). */
int native_sdl_webos_color_slot(const SDL_KeyboardEvent *event);
int native_sdl_webos_cursor_visibility(const SDL_KeyboardEvent *event);
bool native_sdl_confirm_key(const SDL_KeyboardEvent *event);
int native_filter_webos_system_keys(void *userdata, SDL_Event *event);

void native_cursor_reassert_for_activity(App *app);

/* Grabbed-evdev drains and the loop pacing that waits on their wake fd. */
void native_drain_evdev_input(App *app, SDL_Window *window);
void native_drain_evdev_mouse(App *app, SDL_Window *window);
void native_drain_evdev_keyboard(App *app);
void native_wait_for_loop_tick(App *app, uint32_t delay_ms);

#endif
#endif
