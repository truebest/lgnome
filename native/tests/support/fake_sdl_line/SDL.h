#ifndef LGNOME_TEST_SDL_LINE_H
#define LGNOME_TEST_SDL_LINE_H
#include "../fake_ui/SDL.h"
#include <math.h>
typedef struct SDL_Color { uint8_t r, g, b, a; } SDL_Color;
typedef struct SDL_Point { int x, y; } SDL_Point;
typedef struct SDL_Palette SDL_Palette;
typedef struct SDL_Surface SDL_Surface;
#define SDL_TEXTUREACCESS_TARGET 2
#define SDL_sqrt sqrt
#define SDL_pow pow
#define SDL_atan2 atan2
SDL_Texture *SDL_CreateTexture(SDL_Renderer *, Uint32, int, int, int);
SDL_Texture *SDL_GetRenderTarget(SDL_Renderer *);
void SDL_DestroyTexture(SDL_Texture *);
int SDL_RenderFillRect(SDL_Renderer *, const SDL_Rect *);
int SDL_SetTextureColorMod(SDL_Texture *, uint8_t, uint8_t, uint8_t);
int SDL_SetTextureAlphaMod(SDL_Texture *, uint8_t);
int SDL_RenderSetClipRect(SDL_Renderer *, const SDL_Rect *);
int SDL_RenderCopyEx(SDL_Renderer *, SDL_Texture *, const SDL_Rect *, const SDL_Rect *, double, const SDL_Point *, int);
SDL_bool SDL_RectEquals(const SDL_Rect *, const SDL_Rect *);
#endif
