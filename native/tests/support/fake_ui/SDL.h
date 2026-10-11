#ifndef LGNOME_TEST_UI_SDL_H
#define LGNOME_TEST_UI_SDL_H
#include <stdint.h>
typedef uint32_t Uint32;
typedef int32_t SDL_Keycode;
typedef struct SDL_Window { int unused; } SDL_Window;
typedef struct SDL_Renderer { int unused; } SDL_Renderer;
typedef struct SDL_Texture { int unused; } SDL_Texture;
typedef struct SDL_Rect { int x, y, w, h; } SDL_Rect;
typedef enum SDL_bool { SDL_FALSE, SDL_TRUE } SDL_bool;
typedef enum SDL_eventaction { SDL_ADDEVENT, SDL_PEEKEVENT, SDL_GETEVENT } SDL_eventaction;
typedef struct SDL_Keysym { int scancode; SDL_Keycode sym; uint16_t mod; } SDL_Keysym;
typedef struct SDL_KeyboardEvent { Uint32 type; uint8_t repeat; SDL_Keysym keysym; } SDL_KeyboardEvent;
typedef struct SDL_MouseMotionEvent { Uint32 type, state; int x, y; } SDL_MouseMotionEvent;
typedef struct SDL_MouseButtonEvent { Uint32 type; uint8_t button; int x, y; } SDL_MouseButtonEvent;
typedef struct SDL_TextInputEvent { Uint32 type; char text[32]; } SDL_TextInputEvent;
typedef union SDL_Event { Uint32 type; SDL_KeyboardEvent key; SDL_MouseMotionEvent motion; SDL_MouseButtonEvent button; SDL_TextInputEvent text; } SDL_Event;
#define KMOD_NUM 0x1000
#define SDL_KEYDOWN 0x300u
#define SDL_KEYUP 0x301u
#define SDL_TEXTINPUT 0x303u
#define SDL_MOUSEMOTION 0x400u
#define SDL_MOUSEBUTTONDOWN 0x401u
#define SDL_MOUSEBUTTONUP 0x402u
#define SDL_BUTTON_LEFT 1
#define SDL_BUTTON_LMASK 1u
#define SDL_BLENDMODE_BLEND 1
#define SDL_PIXELFORMAT_BGRA32 1u
#define SDL_VERSION_ATLEAST(a,b,c) 1
#define SDL_TICKS_PASSED(a,b) ((int32_t)((b)-(a)) <= 0)
Uint32 SDL_GetTicks(void);
int SDL_PeepEvents(SDL_Event *events, int max, SDL_eventaction action, Uint32 low, Uint32 high);
SDL_bool SDL_HasEvent(Uint32 type);
int SDL_SetRenderTarget(SDL_Renderer *renderer, SDL_Texture *texture);
int SDL_SetRenderDrawBlendMode(SDL_Renderer *renderer, int mode);
int SDL_SetRenderDrawColor(SDL_Renderer *renderer, uint8_t r, uint8_t g, uint8_t b, uint8_t a);
int SDL_RenderClear(SDL_Renderer *renderer);
int SDL_SetTextureBlendMode(SDL_Texture *texture, int mode);
int SDL_RenderCopy(SDL_Renderer *renderer, SDL_Texture *texture, const SDL_Rect *src, const SDL_Rect *dst);
void SDL_RenderPresent(SDL_Renderer *renderer);
#define KMOD_CAPS 8192
#define KMOD_SHIFT 3
#define SDLK_0 48
#define SDLK_9 57
#define SDLK_BACKQUOTE 96
#define SDLK_BACKSLASH 92
#define SDLK_BACKSPACE 8
#define SDLK_COLON 58
#define SDLK_COMMA 44
#define SDLK_DELETE 127
#define SDLK_DOWN 1073741905
#define SDLK_END 1073741901
#define SDLK_EQUALS 61
#define SDLK_ESCAPE 27
#define SDLK_HOME 1073741898
#define SDLK_KP_0 1073741922
#define SDLK_KP_1 1073741913
#define SDLK_KP_9 1073741921
#define SDLK_KP_DIVIDE 1073741908
#define SDLK_KP_ENTER 1073741912
#define SDLK_KP_EQUALS 1073741927
#define SDLK_KP_MINUS 1073741910
#define SDLK_KP_MULTIPLY 1073741909
#define SDLK_KP_PERIOD 1073741923
#define SDLK_KP_PLUS 1073741911
#define SDLK_LEFT 1073741904
#define SDLK_LEFTBRACKET 91
#define SDLK_MINUS 45
#define SDLK_PERIOD 46
#define SDLK_QUOTE 39
#define SDLK_RETURN 13
#define SDLK_RETURN2 1073741982
#define SDLK_RIGHT 1073741903
#define SDLK_RIGHTBRACKET 93
#define SDLK_SEMICOLON 59
#define SDLK_SLASH 47
#define SDLK_SPACE 32
#define SDLK_TAB 9
#define SDLK_UP 1073741906
#define SDLK_a 97
#define SDLK_z 122
#define SDL_SCANCODE_ESCAPE 41
#endif
