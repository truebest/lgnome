#ifndef LGNOME_TEST_POINTER_SDL_H
#define LGNOME_TEST_POINTER_SDL_H
#include "../fake_runtime/SDL.h"
typedef struct SDL_DisplayMode { uint32_t format; int w, h, refresh_rate; void *driverdata; } SDL_DisplayMode;
#define SDL_BUTTON_RIGHT 3
#define SDL_BUTTON_MIDDLE 2
const char *SDL_GetError(void);
int SDL_GetNumVideoDisplays(void);
int SDL_GetDesktopDisplayMode(int display, SDL_DisplayMode *mode);
int SDL_GetCurrentDisplayMode(int display, SDL_DisplayMode *mode);
int SDL_GetNumDisplayModes(int display);
int SDL_GetDisplayMode(int display, int index, SDL_DisplayMode *mode);
#endif
