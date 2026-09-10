#ifndef LGNOME_NATIVE_RGBA_OWNER_H
#define LGNOME_NATIVE_RGBA_OWNER_H

#include <stdbool.h>

/* RGBA canvas ownership (see native_rgba_owner.c). _locked functions require
 * App.video_lock; texture-destroy entry points are SDL-thread-only. */

typedef struct App App;

bool native_rgba_owner_matches(const App *app, int slot, unsigned epoch);
void native_reset_rgba_owner(App *app);
void native_close_rgba_locked(App *app, bool defer_texture);
void native_clear_hub_return_replacement(App *app);
void native_close_hub_return_rgba_locked(App *app);

#ifdef LGNOME_TARGET_WEBOS
void native_defer_rgba_texture_destroy(App *app);
void native_destroy_rgba_renderer_textures(App *app);
void native_capture_hub_return_rgba_locked(App *app);
void native_arm_hub_return_replacement_locked(App *app, int slot, unsigned epoch, unsigned baseline_frames);
bool native_promote_hub_return_rgba_locked(App *app, int slot, unsigned epoch);
#endif

#endif
