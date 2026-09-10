#include "cursor_sdl.h"
#include <assert.h>

static SDL_Cursor system_cursor = {1};
static SDL_Cursor server_cursor = {2};
static SDL_Surface surface;
static SDL_Window window;
static bool mouse_focus = true;
static bool skip_same_cursor;
static SDL_Cursor *displayed;
static SDL_Cursor *selected = &system_cursor;
static bool sdl_enabled = true;
static bool platform_visible;
static bool visibility_supported;
static unsigned redraws;
static unsigned hides;

SDL_Window *SDL_GetMouseFocus(void) { return mouse_focus ? &window : NULL; }
SDL_Cursor *SDL_GetDefaultCursor(void) { return &system_cursor; }
void SDL_SetCursor(SDL_Cursor *cursor) {
    /* 2.0.14 on the TV redraws an unchanged cursor; 2.30 in the SDK skips it.
     * Both choose the default artwork for NULL when mouse focus is absent. */
    if (skip_same_cursor && cursor == selected) return;
    if (cursor) selected = cursor;
    displayed = sdl_enabled ? (cursor ? cursor : (mouse_focus ? selected : &system_cursor)) : NULL;
    redraws++;
    platform_visible = displayed != NULL;
}
int SDL_ShowCursor(int toggle) {
    int previous = sdl_enabled;
    if (toggle >= 0) {
        if (toggle == SDL_DISABLE) hides++;
        bool changed = sdl_enabled != (toggle != 0);
        sdl_enabled = toggle != 0;
        if (changed) SDL_SetCursor(NULL);
    }
    return previous;
}
SDL_bool SDL_webOSCursorVisibility(SDL_bool visible) {
    if (!visibility_supported) return SDL_FALSE;
    platform_visible = visible != SDL_FALSE;
    return SDL_TRUE;
}
void SDL_FreeCursor(SDL_Cursor *cursor) { (void)cursor; }
void SDL_ClearError(void) {}
const char *SDL_GetError(void) { return ""; }
SDL_Surface *SDL_CreateRGBSurfaceWithFormatFrom(void *pixels, int w, int h, int depth, int pitch, uint32_t format) {
    (void)pixels; (void)w; (void)h; (void)depth; (void)pitch; (void)format;
    return &surface;
}
SDL_Cursor *SDL_CreateColorCursor(SDL_Surface *s, int x, int y) { (void)s; (void)x; (void)y; return &server_cursor; }
void SDL_FreeSurface(SDL_Surface *s) { (void)s; }

static void test_restore(bool modern_sdl) {
    selected = &system_cursor;
    displayed = &system_cursor;
    skip_same_cursor = modern_sdl;
    mouse_focus = true;
    sdl_enabled = true;
    redraws = hides = 0;
    NativeCursor cursor;
    native_cursor_init(&cursor);
    /* A focused pointer plane was hidden externally while SDL stayed enabled.
     * This tests redraw requests, not acquisition of initial compositor focus. */
    visibility_supported = false;
    platform_visible = false;
    native_cursor_reassert(&cursor);
    assert(platform_visible && redraws > 0 && hides == 0);

    const uint8_t rgba[4] = {255, 255, 255, 255};
    native_cursor_submit_bitmap(&cursor, 1, 1, 0, 0, rgba, sizeof(rgba));
    native_cursor_apply(&cursor, 1920, 1080, 1920, 1080);
    assert(selected == &server_cursor);
    /* TV menu hides the same cursor without changing SDL's enabled state. */
    unsigned before = redraws;
    platform_visible = false;
    native_cursor_reassert(&cursor);
    assert(platform_visible && selected == &server_cursor && displayed == &server_cursor && redraws > before);

    /* Window/keyboard focus can return before pointer focus. Explicitly applying
     * a shape must not be overwritten with the default artwork by NULL redraw. */
    mouse_focus = false;
    selected = &system_cursor;
    displayed = &system_cursor;
    native_cursor_reassert(&cursor);
    assert(selected == &server_cursor && displayed == &server_cursor);
    native_cursor_reassert(&cursor);
    assert(displayed == &server_cursor);
    mouse_focus = true;
    platform_visible = false;
    native_cursor_reassert(&cursor);
    assert(platform_visible && displayed == &server_cursor);

    /* A failed platform hide must not actively redraw a hidden plane visible. */
    platform_visible = false;
    native_cursor_submit_state(&cursor, NATIVE_CURSOR_HIDDEN);
    native_cursor_reassert(&cursor);
    assert(!platform_visible && sdl_enabled && hides == 0);

    /* A genuine server hide must be applied after enabling SDL; never
     * disable SDL's pointer delivery to hide the image. */
    visibility_supported = true;
    native_cursor_submit_state(&cursor, NATIVE_CURSOR_HIDDEN);
    native_cursor_reassert(&cursor);
    assert(!platform_visible && sdl_enabled && hides == 0);
    native_cursor_submit_bitmap(&cursor, 1, 1, 0, 0, rgba, sizeof(rgba));
    native_cursor_reassert(&cursor);
    assert(platform_visible && selected == &server_cursor);
    native_cursor_destroy(&cursor);
}

int main(void) {
    test_restore(false); /* SDL 2.0.14 on TV */
    test_restore(true);  /* SDL 2.30 in SDK */
    return 0;
}
