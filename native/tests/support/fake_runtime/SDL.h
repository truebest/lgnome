#ifndef LGNOME_TEST_RUNTIME_SDL_H
#define LGNOME_TEST_RUNTIME_SDL_H
#include <stdint.h>
typedef struct SDL_Window { int unused; } SDL_Window;
typedef struct SDL_Renderer { int unused; } SDL_Renderer;
typedef struct SDL_Texture { int unused; } SDL_Texture;
typedef struct SDL_Cursor { int unused; } SDL_Cursor;
typedef uint32_t Uint32;
typedef struct SDL_Keysym { int scancode; int sym; } SDL_Keysym;
typedef struct SDL_KeyboardEvent { uint32_t type; uint8_t repeat; SDL_Keysym keysym; } SDL_KeyboardEvent;
typedef union SDL_Event {
    uint32_t type;
    SDL_KeyboardEvent key;
    struct { uint32_t type; uint8_t event; int data1, data2; } window;
    struct { uint32_t type, state; int x, y; } motion;
    struct { uint32_t type; uint8_t button; int x, y; } button;
    struct { uint32_t type; int y; uint32_t direction; } wheel;
} SDL_Event;
enum {
    SDL_QUIT = 1, SDL_APP_TERMINATING, SDL_APP_WILLENTERBACKGROUND,
    SDL_APP_DIDENTERBACKGROUND, SDL_APP_WILLENTERFOREGROUND,
    SDL_APP_DIDENTERFOREGROUND, SDL_WINDOWEVENT, SDL_MOUSEMOTION,
    SDL_MOUSEBUTTONDOWN, SDL_MOUSEBUTTONUP, SDL_MOUSEWHEEL, SDL_KEYDOWN, SDL_KEYUP
};
enum {
    SDL_WINDOWEVENT_CLOSE = 1, SDL_WINDOWEVENT_SIZE_CHANGED, SDL_WINDOWEVENT_RESIZED,
    SDL_WINDOWEVENT_FOCUS_LOST, SDL_WINDOWEVENT_HIDDEN, SDL_WINDOWEVENT_MINIMIZED,
    SDL_WINDOWEVENT_FOCUS_GAINED, SDL_WINDOWEVENT_RESTORED, SDL_WINDOWEVENT_SHOWN,
    SDL_WINDOWEVENT_EXPOSED
};
enum { SDL_BUTTON_LEFT = 1, SDL_BUTTON_X1 = 4, SDL_BUTTON_X2 = 5 };
enum { SDLK_RETURN = 13, SDLK_KP_ENTER = 271, SDLK_RETURN2 = 272 };
#define SDL_BUTTON_LMASK 1u
#define SDL_MOUSEWHEEL_FLIPPED 1u
#define SDL_GETEVENT 2
#define SDL_VERSION_ATLEAST(a, b, c) 1
typedef struct SDL_Rect { int x,y,w,h; } SDL_Rect;
#define SDL_BLENDMODE_BLEND 1
#define SDL_BLENDMODE_NONE 0
#define SDL_TICKS_PASSED(A,B) ((int32_t)((B)-(A)) <= 0)
uint32_t SDL_GetTicks(void);
void SDL_WarpMouseInWindow(SDL_Window *, int, int);
void SDL_DestroyTexture(SDL_Texture *);
int SDL_SetRenderTarget(SDL_Renderer *, SDL_Texture *);
int SDL_SetRenderDrawBlendMode(SDL_Renderer *, int);
int SDL_SetRenderDrawColor(SDL_Renderer *, uint8_t,uint8_t,uint8_t,uint8_t);
int SDL_RenderClear(SDL_Renderer *);
void SDL_RenderPresent(SDL_Renderer *);
int SDL_GetRendererOutputSize(SDL_Renderer *, int *, int *);
int SDL_RenderFillRect(SDL_Renderer *, const SDL_Rect *);
SDL_Window *SDL_GetKeyboardFocus(void);
void SDL_GetWindowSize(SDL_Window *, int *, int *);
void SDL_Delay(Uint32);
void SDL_FilterEvents(int (*)(void *, SDL_Event *), void *);
int SDL_PeepEvents(SDL_Event *, int, int, uint32_t, uint32_t);
void SDL_PumpEvents(void);
#endif
