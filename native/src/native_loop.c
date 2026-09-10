/* The app loop: window/renderer bring-up, ordered SDL-thread phase dispatch,
 * presentation, pacing, and teardown. main() calls exactly one function here. */

#include "native_loop.h"

#include <pthread.h>
#include <stdatomic.h>

#include "camera_preview.h"
#ifdef LGNOME_TARGET_WEBOS
#include "input_evdev.h"
#endif
#include "native_app.h"
#include "native_hub_actions.h"
#include "native_input_route.h"
#include "native_loop_internal.h"
#include "native_mixer_overlay.h"
#include "native_pointer.h"
#include "native_presentation.h"
#include "native_rgba_owner.h"
#include "native_sdl_events.h"
#include "native_session.h"
#include "native_settings.h"
#include "native_switch.h"

#include "clog.h"

clog_define(g_native_log_loop, cLogLevelInfo, "native");

#ifdef LGNOME_TARGET_WEBOS
/* Early-exit teardown for the loop bring-up ladder. Any argument may be NULL /
 * text_input false at the stage that failed; teardown order matters (renderer
 * before window before the subsystem). */
static void loop_teardown_sdl(SDL_Window *window, SDL_Renderer *renderer, bool text_input) {
    if (text_input) {
        SDL_StopTextInput();
    }
    if (renderer) {
        SDL_DestroyRenderer(renderer);
    }
    if (window) {
        SDL_DestroyWindow(window);
    }
    SDL_QuitSubSystem(SDL_INIT_VIDEO | SDL_INIT_EVENTS);
}

int native_run_app_loop(App *app, NativeSettings *settings) {
    if (SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) {
        clog(cLogLevelError, "SDL_InitSubSystem(VIDEO|EVENTS) failed: %s", SDL_GetError());
        return 4;
    }
    native_log_sdl_display_modes();

    clog(cLogLevelInfo, "creating borderless SDL window %dx%d", NATIVE_LOCAL_SURFACE_WIDTH,
         NATIVE_LOCAL_SURFACE_HEIGHT);
    uint32_t window_flags = SDL_WINDOW_SHOWN | SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_BORDERLESS;
    SDL_Window *window = SDL_CreateWindow("lgnome", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
                                          NATIVE_LOCAL_SURFACE_WIDTH, NATIVE_LOCAL_SURFACE_HEIGHT, window_flags);
    if (!window) {
        clog(cLogLevelError, "SDL_CreateWindow failed: %s", SDL_GetError());
        loop_teardown_sdl(NULL, NULL, false);
        return 4;
    }
    /* Cursor visibility is event-driven from here on: the preconnect UI keeps the default
     * arrow, and during a session the server's pointer updates drive shape/visibility
     * through native_cursor_apply (cursor_sdl.c). */

    SDL_Renderer *renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_TARGETTEXTURE);
    if (!renderer) {
        clog(cLogLevelError, "SDL_CreateRenderer failed: %s", SDL_GetError());
        loop_teardown_sdl(window, NULL, false);
        return 4;
    }

    int window_width = 0;
    int window_height = 0;
    char surface_message[128];
    if (!native_init_render_state(app, window, renderer, &window_width, &window_height, surface_message,
                                  sizeof(surface_message))) {
        clog(cLogLevelError, "%s", surface_message);
        loop_teardown_sdl(window, renderer, false);
        return 4;
    }
    SDL_RaiseWindow(window);
    SDL_StartTextInput();

    if (app->camera_preview) {
        clog(cLogLevelNotice, "starting local V4L2 camera preview mode");
        int preview_result = native_camera_preview_run(renderer, &app->running);
        loop_teardown_sdl(window, renderer, true);
        return preview_result;
    }

    uint16_t loop_fps = settings->sessions[NATIVE_SESSION_SLOT_GREEN].fps;
    clog(cLogLevelInfo, "SDL loop running at target %u fps", (unsigned)loop_fps);

    NativePreconnectUi *ui =
        native_preconnect_ui_create(window, renderer, settings);
    if (!ui) {
        clog(cLogLevelError, "failed to create pre-connect UI");
        loop_teardown_sdl(window, renderer, true);
        return 4;
    }
    native_preconnect_ui_set_background_drawer(ui, native_draw_preconnect_background, app);
    app->preconnect_ui = ui;
    int initial_hub_slot = native_preconnect_ui_selected_slot(ui);
    if (initial_hub_slot >= 0 && initial_hub_slot < NATIVE_SETTINGS_MAX_SESSIONS) {
        atomic_store(&app->active_index, initial_hub_slot);
        native_duck_retarget(app);
    }

    app->interactive_ui = true;
    app->ui_last_state = -1;
    app->streaming_visible = false;
    app->hub_visible = false;
    bool present_logged = false;

    if (app->debug_connect_slot >= 0) {
        /* Take the same route a colour-button press takes, so this shortcut cannot
         * skip profile validation or reach a state the remote could not. */
        clog(cLogLevelNotice, "debug launch option: opening the %s profile",
             native_session_slot_name(app->debug_connect_slot));
        if (!native_preconnect_ui_request_connect(ui, app->debug_connect_slot)) {
            clog(cLogLevelWarning,
                 "the %s profile is not configured; leaving its setup drawer open",
                 native_session_slot_name(app->debug_connect_slot));
        }
    }

    /* The hub mirrors the current USB input probes in its compact icon indicators. */
    native_preconnect_ui_set_keyboard_available(ui, native_evdev_input_probe_keyboard());
    native_preconnect_ui_set_mouse_available(ui, native_evdev_input_probe_mouse());

    while (atomic_load(&app->running)) {
        NativeSessionSlot *active = native_active_slot(app);
        int event_state = atomic_load(&active->current_state);
        if (app->streaming_visible && active->rdp && event_state == (int)RDP_STATE_ACTIVE) {
            native_drain_pointer_clamp(app);
            native_drain_pointer_warp(app, window);
            native_cursor_tick(app);
            native_drain_evdev_input(app, window);
            SDL_Event event;
            while (SDL_PollEvent(&event)) {
                native_handle_sdl_event(app, window, renderer, &event);
            }
        } else {
            native_pump_preconnect_system_events(app, window, renderer, ui, &window_width, &window_height);
        }

        native_switch_tick(app);
        native_system_volume_tick(app);

        native_session_supervisor_tick(app, ui);
        native_hub_runtime_publish(app, ui);
        native_streaming_transition_tick(app, ui, settings, window);

        if (app->streaming_visible) {
            native_present_streaming_frame(app, renderer, &present_logged);
        }

        native_hub_actions_tick(app, ui, settings);

        pthread_mutex_lock(&app->video_lock);
        bool hardware_video_plane = app->video != NULL;
        pthread_mutex_unlock(&app->video_lock);
        native_preconnect_ui_set_hardware_video_plane(ui, hardware_video_plane);
        native_preconnect_ui_tick(ui);
        uint16_t active_fps = native_active_slot(app)->config.fps;
        if (active_fps == 0) {
            active_fps = loop_fps;
        }
        native_wait_for_loop_tick(app, active_fps > 0 ? (uint32_t)(1000u / active_fps) : 16u);
    }

    app->preconnect_ui = NULL;
    native_preconnect_ui_destroy(ui);

    /* Free the SDL cursor objects while the video subsystem is still up. */
    for (int i = 0; i < NATIVE_SETTINGS_MAX_SESSIONS; i++) {
        native_cursor_reset(&app->sessions[i].cursor);
    }

    SDL_StopTextInput();
    if (renderer) {
        native_destroy_rgba_renderer_textures(app);
        SDL_DestroyRenderer(renderer);
    }
    SDL_DestroyWindow(window);
    SDL_QuitSubSystem(SDL_INIT_VIDEO | SDL_INIT_EVENTS);
    return atomic_load(&app->exit_code);
}
#else
int native_run_app_loop(App *app, NativeSettings *settings) {
    (void)settings;
    clog(cLogLevelInfo,
         "SDL event loop is not compiled in; deterministic host smoke loop exits after start");
    return atomic_load(&app->exit_code);
}
#endif
