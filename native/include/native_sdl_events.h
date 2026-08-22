#ifndef GNOMECAST_NATIVE_SDL_EVENTS_H
#define GNOMECAST_NATIVE_SDL_EVENTS_H

/* SDL event handling for the streaming screen (see native_sdl_events.c). */
#ifdef HELLOLG_TARGET_WEBOS

#include <SDL.h>

#include "native_app.h"

void native_handle_sdl_event(App *app, SDL_Window *window, SDL_Renderer *renderer, const SDL_Event *event);

/* Pumps only system/lifecycle events while the preconnect UI owns the screen
 * (the UI reads input through its own SDL hooks). */
void native_pump_preconnect_system_events(App *app, SDL_Window *window, SDL_Renderer *renderer,
                                          NativePreconnectUi *ui, int *window_width, int *window_height);

#endif
#endif
