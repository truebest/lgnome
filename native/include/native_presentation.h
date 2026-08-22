#ifndef GNOMECAST_NATIVE_PRESENTATION_H
#define GNOMECAST_NATIVE_PRESENTATION_H

/* SDL-thread presentation (see native_presentation.c). SDL builds only. */
#ifdef HELLOLG_TARGET_WEBOS

#include <SDL.h>
#include <stdbool.h>

typedef struct App App;

void native_present_renderer_frame(App *app, SDL_Renderer *renderer, bool *logged);
int native_draw_rgba_frame(App *app, SDL_Renderer *renderer, bool *logged);
int native_present_rgba_frame(App *app, SDL_Renderer *renderer, bool *logged);
bool native_draw_preconnect_background(void *ctx, SDL_Renderer *renderer);
void native_present_indicator_frame(App *app, SDL_Renderer *renderer);
void native_present_switch_splash(App *app, SDL_Renderer *renderer);
void native_present_mixer_overlay_frame(App *app, SDL_Renderer *renderer);
void native_present_streaming_frame(App *app, SDL_Renderer *renderer, bool *logged);
void native_show_session_indicator(App *app, int slot);

#endif
#endif
