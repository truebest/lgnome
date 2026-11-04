#include <assert.h>
#include <string.h>
#include "lvgl_sdl_line.c"

typedef void (*LineCallback)(lv_draw_ctx_t *, const lv_draw_line_dsc_t *, const lv_point_t *, const lv_point_t *);
_Static_assert(_Generic(&lv_draw_sdl_draw_line, LineCallback: 1, default: 0), "SDL line callback ABI mismatch");

static SDL_Texture texture;
static SDL_Texture *cached;
static bool accept_cache = true, fail_texture;
static unsigned created, destroyed, drawn;

SDL_Texture *SDL_CreateTexture(SDL_Renderer *r, Uint32 f, int a, int w, int h) {
    (void)r; (void)f; (void)a; assert(w > 0 && h > 0);
    if (fail_texture) return NULL;
    created++; return &texture;
}
SDL_Texture *SDL_GetRenderTarget(SDL_Renderer *r) { (void)r; return NULL; }
void SDL_DestroyTexture(SDL_Texture *t) { assert(t == &texture); destroyed++; }
int SDL_SetRenderTarget(SDL_Renderer *r, SDL_Texture *t) { (void)r; (void)t; return 0; }
int SDL_SetTextureBlendMode(SDL_Texture *t, int m) { assert(t); (void)m; return 0; }
int SDL_SetRenderDrawBlendMode(SDL_Renderer *r, int m) { (void)r; (void)m; return 0; }
int SDL_SetRenderDrawColor(SDL_Renderer *r, uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
    (void)r; (void)a; (void)b; (void)c; (void)d; return 0;
}
int SDL_RenderFillRect(SDL_Renderer *r, const SDL_Rect *a) { (void)r; (void)a; return 0; }
int SDL_SetTextureColorMod(SDL_Texture *t, uint8_t a, uint8_t b, uint8_t c) {
    assert(t); (void)a; (void)b; (void)c; return 0;
}
int SDL_SetTextureAlphaMod(SDL_Texture *t, uint8_t a) { assert(t); (void)a; return 0; }
int SDL_RenderSetClipRect(SDL_Renderer *r, const SDL_Rect *a) { (void)r; (void)a; return 0; }
int SDL_RenderCopy(SDL_Renderer *r, SDL_Texture *t, const SDL_Rect *a, const SDL_Rect *b) {
    (void)r; (void)t; (void)a; (void)b; return 0;
}
int SDL_RenderCopyEx(SDL_Renderer *r, SDL_Texture *t, const SDL_Rect *a, const SDL_Rect *b,
                     double angle, const SDL_Point *c, int flip) {
    (void)r; (void)a; (void)b; (void)angle; (void)c; (void)flip;
    assert(t == &texture); drawn++; return 0;
}
SDL_bool SDL_RectEquals(const SDL_Rect *a, const SDL_Rect *b) { return memcmp(a, b, sizeof(*a)) == 0; }
SDL_Texture *lv_draw_sdl_texture_cache_get(lv_draw_sdl_ctx_t *c, const void *k, size_t n, bool *found) {
    (void)c; (void)k; (void)n; (void)found; return cached;
}
bool lv_draw_sdl_texture_cache_put(lv_draw_sdl_ctx_t *c, const void *k, size_t n, SDL_Texture *t) {
    (void)c; (void)k; (void)n; assert(t);
    if (accept_cache) cached = t;
    return accept_cache;
}
void lv_area_to_sdl_rect(const lv_area_t *a, SDL_Rect *b) {
    *b = (SDL_Rect){a->x1, a->y1, a->x2 - a->x1 + 1, a->y2 - a->y1 + 1};
}
void lv_color_to_sdl_color(const lv_color_t *a, SDL_Color *b) { (void)a; *b = (SDL_Color){0}; }
bool lv_draw_sdl_composite_begin(lv_draw_sdl_ctx_t *c, const lv_area_t *a, const lv_area_t *clip,
    const lv_area_t *ext, lv_blend_mode_t mode, lv_area_t *out, lv_area_t *out_clip, lv_area_t *apply) {
    (void)c; (void)ext; (void)mode; *out = *a; *out_clip = *clip; *apply = *a; return false;
}
void lv_draw_sdl_composite_end(lv_draw_sdl_ctx_t *c, const lv_area_t *a, lv_blend_mode_t mode) {
    (void)c; (void)a; (void)mode;
}
void lv_draw_mask_radius_init(lv_draw_mask_radius_param_t *p, const lv_area_t *a, lv_coord_t r, bool inv) {
    (void)p; (void)a; (void)r; (void)inv; assert(0);
}
int16_t lv_draw_mask_add(void *p, void *id) { (void)p; (void)id; assert(0); return 0; }
void *lv_draw_mask_remove_id(int16_t id) { (void)id; assert(0); return NULL; }
SDL_Texture *lv_draw_sdl_mask_dump_texture(SDL_Renderer *r, const lv_area_t *a, const int16_t *ids, int16_t n) {
    (void)r; (void)a; (void)ids; (void)n; assert(0); return NULL;
}

int main(void) {
    lv_area_t clip = {0, 0, 500, 100};
    lv_draw_sdl_ctx_t ctx = {.base_draw = {.clip_area = &clip, .draw_line = lv_draw_sdl_draw_line}};
    lv_draw_line_dsc_t d; lv_draw_line_dsc_init(&d); d.width = 2;
    lv_point_t a = {0, 20}, b = {30, 5};
    lv_draw_line(&ctx.base_draw, &d, &a, &b);
    assert(created == 1 && drawn == 1 && destroyed == 0);
    lv_draw_line(&ctx.base_draw, &d, &a, &b);
    assert(created == 1 && drawn == 2 && destroyed == 0);
    cached = NULL; accept_cache = false;
    lv_draw_line(&ctx.base_draw, &d, &a, &b);
    assert(created == 2 && drawn == 3 && destroyed == 1);
    fail_texture = true;
    lv_draw_line(&ctx.base_draw, &d, &a, &b);
    assert(created == 2 && drawn == 3 && destroyed == 1);
    return 0;
}
