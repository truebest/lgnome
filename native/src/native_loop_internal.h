#ifndef GNOMECAST_NATIVE_LOOP_INTERNAL_H
#define GNOMECAST_NATIVE_LOOP_INTERNAL_H

#ifdef HELLOLG_TARGET_WEBOS

#include <SDL.h>

#include "native_loop.h"
#include "ui_preconnect.h"

/* SDL-thread loop phases kept private to native_loop.c's ordered coordinator. */
void native_session_supervisor_tick(App *app, NativePreconnectUi *ui);
void native_hub_runtime_publish(App *app, NativePreconnectUi *ui);
void native_streaming_transition_tick(App *app, NativePreconnectUi *ui, NativeSettings *settings,
                                      SDL_Window *window);

#endif

#endif
