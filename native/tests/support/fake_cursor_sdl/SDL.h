#ifndef LGNOME_TEST_CURSOR_SDL_H
#define LGNOME_TEST_CURSOR_SDL_H
#include <stdint.h>
typedef struct SDL_Window { int unused; } SDL_Window;
SDL_Window *SDL_GetMouseFocus(void);
typedef struct SDL_Cursor { int id; } SDL_Cursor;
typedef struct SDL_Surface { int unused; } SDL_Surface;
typedef enum SDL_bool { SDL_FALSE, SDL_TRUE } SDL_bool;
#define SDL_ENABLE 1
#define SDL_DISABLE 0
#define SDL_QUERY -1
#define SDL_PIXELFORMAT_RGBA32 1u
SDL_Cursor *SDL_GetDefaultCursor(void);
void SDL_SetCursor(SDL_Cursor *cursor);
int SDL_ShowCursor(int toggle);
void SDL_FreeCursor(SDL_Cursor *cursor);
void SDL_ClearError(void);
const char *SDL_GetError(void);
SDL_Surface *SDL_CreateRGBSurfaceWithFormatFrom(void *pixels, int w, int h, int depth, int pitch, uint32_t format);
SDL_Cursor *SDL_CreateColorCursor(SDL_Surface *surface, int hot_x, int hot_y);
void SDL_FreeSurface(SDL_Surface *surface);
#endif
