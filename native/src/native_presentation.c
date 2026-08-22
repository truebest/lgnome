/* Presentation for the SDL thread: the one-shot hole-punch frame over the
 * hardware video plane, the RemoteFX RGBA draw (including HUB-return
 * replacement retirement), the pre-connect background, the colored session
 * indicator badge, the switch splash, and the streaming-frame composition.
 * SDL builds only. */
#ifdef HELLOLG_TARGET_WEBOS

#include "native_presentation.h"

#include <stdatomic.h>

#include "native_app.h"
#include "native_mixer_overlay.h"
#include "native_rdp_state.h"
#include "native_rgba_owner.h"

#include "clog.h"

clog_define(g_native_log_presentation, cLogLevelInfo, cLogFlags_Default, "native", NULL);

void native_present_renderer_frame(App *app, SDL_Renderer *renderer, bool *logged) {
    if (!renderer || !app || app->video_plane_punched) {
        return;
    }
    /* Present the transparent hole-punch frame exactly once. Re-clearing and
     * re-presenting every loop tick raced the NDL hardware video plane's own buffer
     * swaps on this webOS compositor and produced visible flicker between the (empty)
     * graphics layer and the video plane underneath. */
    SDL_SetRenderTarget(renderer, NULL);
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 0);
    SDL_RenderClear(renderer);
    SDL_RenderPresent(renderer);
    app->video_plane_punched = true;
    if (logged && !*logged) {
        clog(cLogLevelDebug, "presented initial SDL renderer frame");
        *logged = true;
    }
}

/* Draws the cached RemoteFX/bitmap desktop into the current window backbuffer without
 * presenting. Returns 1 for a completed draw, 2 for a recoverable skipped draw (the
 * previous front buffer remains owned by RGBA), 0 when no frame exists, and -1 for a
 * terminal failure. Streaming presents immediately below; HUB uses the same draw as its
 * bottom layer, then LVGL adds translucent chrome and performs the single present. */
int native_draw_rgba_frame(App *app, SDL_Renderer *renderer, bool *logged) {
    if (!app || !renderer) {
        return 0;
    }
    int status = 0;
    int failed_owner_slot = -1;
    NativeRgbaResult failed_result = NATIVE_RGBA_OK;
    pthread_mutex_lock(&app->video_lock);
    if (app->pending_texture_destroy) {
        SDL_DestroyTexture(app->pending_texture_destroy);
        app->pending_texture_destroy = NULL;
    }
    int active_index = atomic_load(&app->active_index);
    NativeSessionSlot *active = &app->sessions[active_index];
    unsigned active_epoch = atomic_load(&active->connect_epoch);
    bool active_rgba_ready = native_rgba_owner_matches(app, active_index, active_epoch) &&
                             native_rgba_surface_has_frame(app->rgba);
    bool active_h264_ready = app->video && app->video_owner_slot == active_index &&
                             app->video_owner_epoch == active_epoch;
    bool replacement_ready =
        app->hub_return_rgba && app->hub_return_replacement_slot == active_index &&
        app->hub_return_replacement_epoch == active_epoch &&
        atomic_load(&active->current_state) == (int)RDP_STATE_ACTIVE &&
        atomic_load(&active->video_ok_frames) != app->hub_return_replacement_baseline_frames &&
        (active_rgba_ready || active_h264_ready);
    bool drawing_return = app->hub_return_rgba && !replacement_ready;
    NativeRgbaSurface *surface = active_rgba_ready ? app->rgba : NULL;
    if (drawing_return) {
        surface = app->hub_return_rgba;
    }
    if (surface && native_rgba_surface_has_frame(surface)) {
        uint16_t viewport_width = (uint16_t)atomic_load(&app->render_width);
        uint16_t viewport_height = (uint16_t)atomic_load(&app->render_height);
        NativeRgbaResult result = native_rgba_surface_render(surface, renderer, viewport_width, viewport_height);
        if (result == NATIVE_RGBA_OK) {
            status = 1;
            /* RGBA presents opaque content into the same renderer the NDL hole-punch
             * uses. If the stream later switches back to H.264, the punch-through latch
             * must fire again so the SDL layer clears back to transparent instead of
             * leaving this opaque frame in front of the video plane. */
            native_present_invalidate(app);
        } else if (result == NATIVE_RGBA_RETRY) {
            /* Do not fall through to the transparent NDL punch or present the cleared
             * backbuffer. The next tick retries from the cached CPU frame. */
            status = 2;
        } else {
            failed_result = result;
            failed_owner_slot = drawing_return ? app->hub_return_rgba_owner_slot : active_index;
            status = -1;
            if (drawing_return) {
                /* This frozen texture exhausted its renderer-recovery budget. Drop it
                 * after preserving the owner for one terminal report; otherwise every
                 * HUB tick would retry and re-report the same unusable cache forever. */
                native_close_hub_return_rgba_locked(app);
                drawing_return = false;
            }
        }
    }
    /* A successful software draw, or a hardware owner with one accepted AU, is the
     * replacement's first presentable frame. Retire the frozen desktop only now; a
     * transient SDL retry deliberately keeps it for the next tick. */
    if (app->hub_return_rgba && replacement_ready &&
        (active_h264_ready || (status == 1 && !drawing_return))) {
        native_close_hub_return_rgba_locked(app);
    }
    pthread_mutex_unlock(&app->video_lock);
    if (status == 1 && logged && !*logged) {
        clog(cLogLevelDebug, "presented initial native RemoteFX RGBA frame");
        *logged = true;
    } else if (status < 0) {
        app->decoder_errors++;
        clog(cLogLevelError,
             "terminal native error: DecoderError; RemoteFX RGBA present failed result=%d",
             (int)failed_result);
        NativeSessionSlot *failed_slot =
            failed_owner_slot >= 0 && failed_owner_slot < NATIVE_SETTINGS_MAX_SESSIONS
                ? &app->sessions[failed_owner_slot]
                : native_active_slot(app);
        native_slot_report_terminal(failed_slot, RDP_STATE_DECODER_ERROR,
                                    rdp_state_exit_code(RDP_STATE_DECODER_ERROR));
    }
    return status;
}

int native_present_rgba_frame(App *app, SDL_Renderer *renderer, bool *logged) {
    int status = native_draw_rgba_frame(app, renderer, logged);
    if (status == 1) {
        SDL_RenderPresent(renderer);
    }
    return status;
}

bool native_draw_preconnect_background(void *ctx, SDL_Renderer *renderer) {
    return native_draw_rgba_frame((App *)ctx, renderer, NULL) > 0;
}

static void native_set_slot_badge_color(SDL_Renderer *renderer, int slot) {
    uint32_t rgb = native_ui_slot_rgb(slot);
    SDL_SetRenderDrawColor(renderer, (uint8_t)(rgb >> 16), (uint8_t)(rgb >> 8), (uint8_t)rgb, 230);
}

/* Colored square in the top-right corner naming the active session for a short moment
 * after a switch. Drawn over the transparent hole-punch frame, so the hardware video
 * plane stays visible around it; presented once (latch) and cleared back to the punch
 * frame on expiry. Skipped while the RemoteFX RGBA path owns the renderer. */
void native_present_indicator_frame(App *app, SDL_Renderer *renderer) {
    if (app->indicator_slot < 0) {
        return;
    }
    if (SDL_TICKS_PASSED(SDL_GetTicks(), app->indicator_until_ticks)) {
        app->indicator_slot = -1;
        app->indicator_drawn = false;
        /* Re-arm the punch latch so the next tick clears the square away. */
        native_present_invalidate(app);
        return;
    }
    if (app->indicator_drawn) {
        return;
    }
    int output_width = 0;
    if (SDL_GetRendererOutputSize(renderer, &output_width, NULL) != 0) {
        output_width = NATIVE_LOCAL_SURFACE_WIDTH;
    }
    SDL_SetRenderTarget(renderer, NULL);
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 0);
    SDL_RenderClear(renderer);
    native_set_slot_badge_color(renderer, app->indicator_slot);
    SDL_Rect badge = {output_width - 72, 24, 48, 48};
    SDL_RenderFillRect(renderer, &badge);
    SDL_RenderPresent(renderer);
    app->indicator_drawn = true;
    /* The frame on screen is not the plain punch frame anymore. */
    app->video_plane_punched = true;
}

/* Opaque splash covering the video plane while a swap is in flight: the pipeline reload
 * window (track close -> load -> PLAYING) would otherwise show through as raw black.
 * Drawn once (latch); a ~300ms grace keeps in-band handovers splash-free. */
void native_present_switch_splash(App *app, SDL_Renderer *renderer) {
    if (app->switch_splash_drawn) {
        return;
    }
    uint32_t switch_started = app->switch_deadline_ticks - NATIVE_SWITCH_KEYFRAME_TIMEOUT_MS;
    if (!SDL_TICKS_PASSED(SDL_GetTicks(), switch_started + 300u)) {
        return; /* grace: quick handovers finish before the splash would flash */
    }
    int output_width = 0;
    if (SDL_GetRendererOutputSize(renderer, &output_width, NULL) != 0) {
        output_width = NATIVE_LOCAL_SURFACE_WIDTH;
    }
    SDL_SetRenderTarget(renderer, NULL);
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(renderer, 16, 20, 24, 255);
    SDL_RenderClear(renderer);
    native_set_slot_badge_color(renderer, atomic_load(&app->active_index));
    SDL_Rect badge = {output_width - 72, 24, 48, 48};
    SDL_RenderFillRect(renderer, &badge);
    SDL_RenderPresent(renderer);
    app->switch_splash_drawn = true;
    app->video_plane_punched = true; /* screen content is ours, not the punch frame */
}

void native_present_mixer_overlay_frame(App *app, SDL_Renderer *renderer) {
    if (!app->mixer_overlay_visible) {
        return;
    }
    if (!app->mixer_overlay_dragging && app->mixer_overlay_dismiss_button == 0 &&
        SDL_TICKS_PASSED(SDL_GetTicks(), app->mixer_overlay_hide_ticks)) {
        native_mixer_overlay_hide(app);
        return;
    }
    /* Poll the system volume while the panel is on screen so the MASTER knob follows
     * the remote's VOL keys / headphone buttons. Not while dragging: a poll reply from
     * before the drag's newest set would briefly yank the knob backwards. */
    if (!app->mixer_overlay_dragging &&
        SDL_TICKS_PASSED(SDL_GetTicks(), app->mixer_overlay_volume_poll_ticks)) {
        app->mixer_overlay_volume_poll_ticks = SDL_GetTicks() + NATIVE_MIXER_OVERLAY_VOLUME_POLL_MS;
        native_luna_volume_refresh(&app->luna_volume);
    }
    unsigned connected_mask = 0;
    for (int i = 0; i < NATIVE_SETTINGS_MAX_SESSIONS; i++) {
        NativeSessionSlot *slot = &app->sessions[i];
        if (slot->rdp && atomic_load(&slot->current_state) == (int)RDP_STATE_ACTIVE) {
            connected_mask |= 1u << i;
        }
    }
    /* The MASTER channel is "connected" once the Luna bus has answered: its knob then
     * mirrors the live system volume (dimmed until the first round-trip lands). */
    int master_pct = native_luna_volume_cached(&app->luna_volume);
    if (native_luna_volume_available(&app->luna_volume) && master_pct >= 0) {
        connected_mask |= 1u << NATIVE_UI_MIXER_MASTER;
    }
    NativeUiMixer *mixer_ui = native_preconnect_ui_mixer(app->preconnect_ui);
    if (mixer_ui) {
        /* Meter/stat snapshots are atomic and never enter the callback path. */
        int32_t peaks[NATIVE_UI_MIXER_CHANNELS][2] = {{0, 0}};
        unsigned queue_ms[NATIVE_SETTINGS_MAX_SESSIONS] = {0};
        unsigned target_ms[NATIVE_SETTINGS_MAX_SESSIONS] = {0};
        for (int i = 0; i < NATIVE_SETTINGS_MAX_SESSIONS; i++) {
            NativeAudioSourceStats stats;
            native_audio_pipeline_get_source_peaks(&app->audio_pipeline, i, &peaks[i][0], &peaks[i][1]);
            if (native_audio_pipeline_get_source_stats(&app->audio_pipeline, i, &stats)) {
                queue_ms[i] = stats.queue_ms;
                target_ms[i] = stats.target_delay_ms;
            }
        }
        native_audio_pipeline_get_output_peaks(&app->audio_pipeline, &peaks[NATIVE_UI_MIXER_MASTER][0],
                                               &peaks[NATIVE_UI_MIXER_MASTER][1]);
        int duck_fg = atomic_load(&app->active_index);
        native_ui_mixer_render(mixer_ui, peaks, queue_ms, target_ms, app->mixer_gain_db, master_pct,
                               app->mixer_overlay_selected, connected_mask, duck_fg, app->duck_mask[duck_fg],
                               app->mixer_mute_mask, app->mixer_solo_mask, SDL_GetTicks());
        app->video_plane_punched = true; /* screen content is ours, not the punch frame */
        return;
    }
    /* No LVGL mixer: show() refuses to open the overlay, so this is unreachable. */
    (void)renderer;
}

void native_present_streaming_frame(App *app, SDL_Renderer *renderer, bool *logged) {
    if (native_present_rgba_frame(app, renderer, logged) != 0) {
        return;
    }
    if (app->switch_deadline_ticks != 0) {
        /* A swap is in flight (keyframe watchdog armed): cover the reload window. */
        native_present_switch_splash(app, renderer);
        return;
    }
    if (app->switch_splash_drawn) {
        /* Swap finished: reveal the new stream (re-punch) and repaint the badge. */
        app->switch_splash_drawn = false;
        app->indicator_drawn = false;
        native_present_invalidate(app);
    }
    if (app->mixer_overlay_visible) {
        native_present_mixer_overlay_frame(app, renderer);
        return;
    }
    if (app->indicator_slot >= 0) {
        native_present_indicator_frame(app, renderer);
        return;
    }
    native_present_renderer_frame(app, renderer, logged);
}

void native_show_session_indicator(App *app, int slot) {
    app->indicator_slot = slot;
    app->indicator_until_ticks = SDL_GetTicks() + NATIVE_INDICATOR_SHOW_MS;
    app->indicator_drawn = false;
}

#endif
