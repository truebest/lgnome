/* Ownership of the shared RemoteFX/bitmap canvases: the live `rgba` surface,
 * the frozen HUB-return desktop, the replacement generation that retires it,
 * and the deferred SDL texture destroy. Every _locked function requires
 * App.video_lock held by the caller. */

#include "native_rgba_owner.h"

#include <stdbool.h>

#include "native_app.h"

#include "clog.h"

clog_define(g_native_log_rgba_owner, cLogLevelInfo, "native");

#ifdef LGNOME_TARGET_WEBOS
/* SDL_DestroyTexture() must run on the thread that owns the renderer (the SDL/main thread),
 * but native_rgba_surface_close()/resize() can be invoked from the RDP worker thread
 * (on_bitmap_update, on_video_au). Call this (while holding video_lock) before either, so
 * they find no texture to destroy themselves; the detached texture is destroyed later on
 * the SDL thread, drained in native_present_rgba_frame()/native_stop_rdp(). */
void native_defer_rgba_texture_destroy(App *app) {
    if (!app || !app->rgba) {
        return;
    }
    SDL_Texture *stale = native_rgba_surface_take_texture(app->rgba);
    if (!stale) {
        return;
    }
    if (app->pending_texture_destroy) {
        clog(cLogLevelWarning,
             "leaking undrained RGBA texture to avoid cross-thread SDL_DestroyTexture");
    }
    app->pending_texture_destroy = stale;
}

/* SDL/main thread, while the renderer is still alive. The RDP workers may keep updating
 * CPU pixels until main joins them after native_run_app_loop(), but no render happens
 * after this point. Detach every renderer-owned texture so the later CPU-surface teardown
 * cannot call SDL_DestroyTexture after SDL_DestroyRenderer has invalidated it. */
void native_destroy_rgba_renderer_textures(App *app) {
    if (!app) {
        return;
    }
    pthread_mutex_lock(&app->video_lock);
    SDL_Texture *rgba_texture = native_rgba_surface_take_texture(app->rgba);
    SDL_Texture *return_texture = native_rgba_surface_take_texture(app->hub_return_rgba);
    SDL_Texture *pending_texture = app->pending_texture_destroy;
    app->pending_texture_destroy = NULL;
    pthread_mutex_unlock(&app->video_lock);

    if (rgba_texture) {
        SDL_DestroyTexture(rgba_texture);
    }
    if (return_texture) {
        SDL_DestroyTexture(return_texture);
    }
    if (pending_texture) {
        SDL_DestroyTexture(pending_texture);
    }
}
#endif

/* video_lock protects both RGBA pointers and every owner/context field below. */
bool native_rgba_owner_matches(const App *app, int slot, unsigned epoch) {
    return app && app->rgba && app->rgba_owner_slot == slot && app->rgba_owner_epoch == epoch;
}

void native_reset_rgba_owner(App *app) {
    app->rgba_owner_slot = -1;
    app->rgba_owner_epoch = 0;
}

void native_close_rgba_locked(App *app, bool defer_texture) {
    if (!app || !app->rgba) {
        if (app) {
            native_reset_rgba_owner(app);
        }
        return;
    }
#ifdef LGNOME_TARGET_WEBOS
    if (defer_texture) {
        native_defer_rgba_texture_destroy(app);
    }
#else
    (void)defer_texture;
#endif
    native_rgba_surface_close(app->rgba);
    app->rgba = NULL;
    native_reset_rgba_owner(app);
}

void native_clear_hub_return_replacement(App *app) {
    app->hub_return_replacement_slot = -1;
    app->hub_return_replacement_epoch = 0;
    app->hub_return_replacement_baseline_frames = 0;
}

/* Main/SDL thread only: a return surface may own this renderer's texture, so close it
 * directly instead of consuming the worker-to-SDL pending texture handoff slot. */
void native_close_hub_return_rgba_locked(App *app) {
    if (!app) {
        return;
    }
    native_rgba_surface_close(app->hub_return_rgba);
    app->hub_return_rgba = NULL;
    app->hub_return_rgba_owner_slot = -1;
    app->hub_return_rgba_owner_epoch = 0;
    native_clear_hub_return_replacement(app);
}

/* Freeze the frame that belongs to the session HUB promised BACK would return to.
 * A retry runs after active_index has already moved to the failed target, so it must
 * never replace this cache from old_index/current ownership. */
#ifdef LGNOME_TARGET_WEBOS
void native_capture_hub_return_rgba_locked(App *app) {
    if (!app || !app->hub_visible || app->hub_return_rgba) {
        return;
    }
    int return_slot = app->hub_return_slot;
    if (return_slot < 0 || return_slot >= NATIVE_SETTINGS_MAX_SESSIONS) {
        return;
    }
    unsigned return_epoch = atomic_load(&app->sessions[return_slot].connect_epoch);
    if (!native_rgba_owner_matches(app, return_slot, return_epoch) ||
        !native_rgba_surface_has_frame(app->rgba)) {
        return;
    }
    app->hub_return_rgba = app->rgba;
    app->hub_return_rgba_owner_slot = app->rgba_owner_slot;
    app->hub_return_rgba_owner_epoch = app->rgba_owner_epoch;
    app->rgba = NULL;
    native_reset_rgba_owner(app);
    native_clear_hub_return_replacement(app);
}

void native_arm_hub_return_replacement_locked(App *app, int slot, unsigned epoch,
                                                      unsigned baseline_frames) {
    if (!app || !app->hub_return_rgba) {
        return;
    }
    app->hub_return_replacement_slot = slot;
    app->hub_return_replacement_epoch = epoch;
    app->hub_return_replacement_baseline_frames = baseline_frames;
}

/* BACK may resume the frozen canvas only while its worker generation is unchanged.
 * If backgrounding reconnected that slot, keep the old pixels as a read-only fallback;
 * its fresh generation will replace them after its first decoded frame. */
bool native_promote_hub_return_rgba_locked(App *app, int slot, unsigned epoch) {
    if (!app || !app->hub_return_rgba || app->hub_return_rgba_owner_slot != slot ||
        app->hub_return_rgba_owner_epoch != epoch) {
        return false;
    }
    native_close_rgba_locked(app, false);
    app->rgba = app->hub_return_rgba;
    app->rgba_owner_slot = app->hub_return_rgba_owner_slot;
    app->rgba_owner_epoch = app->hub_return_rgba_owner_epoch;
    app->hub_return_rgba = NULL;
    app->hub_return_rgba_owner_slot = -1;
    app->hub_return_rgba_owner_epoch = 0;
    native_clear_hub_return_replacement(app);
    return true;
}
#endif
