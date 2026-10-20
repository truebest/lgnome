#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "native_app.h"
#include "native_loop_internal.h"
#include "native_input_route.h"
#include "native_mixer_overlay.h"
#include "native_presentation.h"
#include "native_rgba_owner.h"
#include "native_session.h"
#include "native_switch.h"
#include "native_rdp_state.h"
#include "native_config_storage.h"
#include "native_sdl_events.h"
#include "native_pointer.h"

static int grabs, overlay_hides, stops, fader_updates;
static SDL_Window *keyboard_focus;
static bool ui_visible, fail_survivor;
static uint32_t ticks = 10000;
uint32_t SDL_GetTicks(void) {
    return ticks;
}

void SDL_WarpMouseInWindow(SDL_Window *w, int x, int y) {
    (void)w;
    (void)x;
    (void)y;
}

void SDL_DestroyTexture(SDL_Texture *t) {
    (void)t;
}

int SDL_SetRenderTarget(SDL_Renderer *r, SDL_Texture *t) {
    (void)r;
    (void)t;
    return 0;
}

int SDL_SetRenderDrawBlendMode(SDL_Renderer *r, int b) {
    (void)r;
    (void)b;
    return 0;
}

int SDL_SetRenderDrawColor(SDL_Renderer *r, uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
    (void)r;
    (void)a;
    (void)b;
    (void)c;
    (void)d;
    return 0;
}

int SDL_RenderClear(SDL_Renderer *r) {
    (void)r;
    return 0;
}

void SDL_RenderPresent(SDL_Renderer *r) {
    (void)r;
}

int SDL_GetRendererOutputSize(SDL_Renderer *renderer, int *width, int *height) {
    (void)renderer;
    if (width) {
        *width = 1920;
    }
    if (height) {
        *height = 1080;
    }
    return 0;
}
int SDL_RenderFillRect(SDL_Renderer *r, const SDL_Rect *rect) {
    (void)r;
    (void)rect;
    return 0;
}

void native_input_set_active(NativeInput *i, bool b) {
    atomic_store(&i->active, b);
}

void native_input_set_session(NativeInput *i, RdpSession *s) {
    i->session=s;
}

void native_input_set_desktop_size(NativeInput *i, uint16_t w, uint16_t h) {
    atomic_store(&i->desktop_width, w);
    atomic_store(&i->desktop_height, h);
}

bool native_input_sync_locks(NativeInput *i, bool s, bool n, bool c) {
    (void)i;
    (void)s;
    (void)n;
    (void)c;
    return true;
}

SDL_Window *SDL_GetKeyboardFocus(void) {
    return keyboard_focus;
}

bool native_evdev_input_start(NativeEvdevInput *input) {
    (void)input;
    grabs++;
    return true;
}

void native_evdev_input_stop_with_buttons(NativeEvdevInput *input, uint8_t *buttons) {
    (void)input;
    (void)buttons;
    stops++;
}

bool native_evdev_input_keyboard_active(NativeEvdevInput *input) {
    (void)input;
    return true;
}


void native_flush_held_inputs(App *a) {
    (void)a;
}

void native_request_pointer_window_size_update(App *a) {
    (void)a;
}

void native_cursor_reassert(NativeCursor *c) {
    (void)c;
}

void native_cursor_reset(NativeCursor *c) {
    (void)c;
}

bool native_evdev_input_mouse_active(NativeEvdevInput *i) {
    (void)i;
    return true;
}

bool native_evdev_input_probe_keyboard(void) {
    return true;
}

bool native_evdev_input_probe_mouse(void) {
    return true;
}

bool native_config_save_persisted(const NativeSettings *s) {
    (void)s;
    return true;
}

void native_preconnect_ui_set_visible(NativePreconnectUi *u, bool b) {
    (void)u;
    ui_visible=b;
}

void native_preconnect_ui_set_connecting(NativePreconnectUi *u, int i, bool b, const char *s) {
    (void)u;
    (void)i;
    (void)b;
    (void)s;
}

void native_preconnect_ui_set_keyboard_available(NativePreconnectUi *u, bool b) {
    (void)u;
    (void)b;
}

void native_preconnect_ui_set_mouse_available(NativePreconnectUi *u, bool b) {
    (void)u;
    (void)b;
}

void native_preconnect_ui_set_input_unavailable(NativePreconnectUi *u) {
    (void)u;
}

void native_preconnect_ui_set_slot_status(NativePreconnectUi *u, int i, NativePreconnectSessionState s, RdpDisconnectReason r, const char *d) {
    (void)u;
    (void)i;
    (void)s;
    (void)r;
    (void)d;
}

void native_preconnect_ui_set_status(NativePreconnectUi *u, const char *s, bool b) {
    (void)u;
    (void)s;
    (void)b;
}

void native_preconnect_ui_select_slot(NativePreconnectUi *u, int i) {
    (void)u;
    (void)i;
}

void native_preconnect_ui_cancel_hub_close(NativePreconnectUi *u) {
    (void)u;
}

void native_system_volume_rebaseline(App *a) {
    (void)a;
}

void native_switch_watchdog_baseline(App *a, NativeSessionSlot *s) {
    a->switch_baseline_frames=atomic_load(&s->video_ok_frames);
}

void native_switch_watchdog_start(App *a, uint32_t t) {
    a->switch_deadline_ticks=ticks+t;
}

void rdp_set_suppress_output(RdpSession *s, bool b) {
    (void)s;
    (void)b;
}

void rdp_request_refresh(RdpSession *s) {
    (void)s;
}

void native_duck_retarget(App *a) {
    (void)a;
}

void native_background_slot(App *a, int i) {
    (void)a;
    (void)i;
}

void native_capture_pause_for_active_slot_change(App *a) {
    (void)a;
}

bool native_snapshot_replay(App *a, int i) {
    (void)a;
    (void)i;
    return false;
}

bool native_slot_connect(App *a, int i, bool b) {
    (void)a;
    (void)i;
    (void)b;
    return true;
}

void native_au_snapshot_reset(NativeAuSnapshot *s) {
    memset(s, 0, sizeof(*s));
}

void native_mixer_overlay_hide(App *a) {
    overlay_hides++;
    a->mixer_overlay_visible=false;
}

void native_mixer_overlay_force_hide(App *a) {
    native_mixer_overlay_hide(a);
}

void native_stop_slot(App *a, int i) {
    a->sessions[i].rdp=NULL;
    atomic_store(&a->sessions[i].current_state, RDP_STATE_IDLE);
    atomic_store(&a->sessions[i].session_failed, false);
    a->sessions[i].snapshot_pending=false;
}

bool native_any_slot_connected(const App *app) {
    for (int i = 0; i < NATIVE_SETTINGS_MAX_SESSIONS; i++) {
        if (app->sessions[i].rdp) {
            return true;
        }
    }
    return false;
}
void native_stop_media(App *a) {
    a->video=NULL;
    a->rgba=NULL;
}

void native_close_rgba_locked(App *a, bool b) {
    (void)b;
    a->rgba=NULL;
}

void native_close_hub_return_rgba_locked(App *a) {
    a->hub_return_rgba=NULL;
}

bool native_promote_hub_return_rgba_locked(App *a, int i, unsigned e) {
    (void)a;
    (void)i;
    (void)e;
    return false;
}

void native_arm_hub_return_replacement_locked(App *a, int i, unsigned e, unsigned f) {
    (void)a;
    (void)i;
    (void)e;
    (void)f;
}

bool native_rgba_owner_matches(const App *a, int i, unsigned e) {
    return a->rgba&&a->rgba_owner_slot==i&&a->rgba_owner_epoch==e;
}

int native_rgba_surface_has_frame(const NativeRgbaSurface *s) {
    return s!=NULL;
}

NativeRgbaResult native_rgba_surface_render(NativeRgbaSurface *s, SDL_Renderer *r, uint16_t w, uint16_t h) {
    (void)s;
    (void)r;
    (void)w;
    (void)h;
    return NATIVE_RGBA_OK;
}

bool native_audio_pipeline_get_source_stats(NativeAudioPipeline *p, int i, NativeAudioSourceStats *s) {
    (void)p;
    (void)i;
    memset(s, 0, sizeof(*s));
    return false;
}

void native_audio_pipeline_get_source_peaks(NativeAudioPipeline *p, int i, int32_t *l, int32_t *r) {
    (void)p;
    (void)i;
    *l=*r=0;
}

void native_audio_pipeline_get_output_peaks(NativeAudioPipeline *p, int32_t *l, int32_t *r) {
    (void)p;
    *l=*r=0;
}

void native_luna_volume_refresh(NativeLunaVolume *v) {
    (void)v;
}

bool native_luna_volume_available(const NativeLunaVolume *v) {
    (void)v;
    return false;
}

int native_luna_volume_cached(const NativeLunaVolume *v) {
    (void)v;
    return 0;
}

NativeUiMixer *native_preconnect_ui_mixer(NativePreconnectUi *u) {
    (void)u;
    return NULL;
}

void native_ui_mixer_render(NativeUiMixer *m, const int32_t (*p)[2], const unsigned *q, const unsigned *t, const int8_t *g, int v, int s, unsigned c, int f, unsigned d, unsigned mu, unsigned so, uint32_t n) {
    (void)m;
    (void)p;
    (void)q;
    (void)t;
    (void)g;
    (void)v;
    (void)s;
    (void)c;
    (void)f;
    (void)d;
    (void)mu;
    (void)so;
    (void)n;
}

void SDL_GetWindowSize(SDL_Window *window, int *width, int *height) {
    (void)window;
    *width = 1920;
    *height = 1080;
}
bool native_update_render_size(App *app, SDL_Renderer *renderer) {
    (void)app;
    (void)renderer;
    return true;
}

void native_update_pointer_window_size(App *app) {
    (void)app;
}

int native_ui_mixer_fader_pct_at(int height, int y) {
    (void)height;
    return y;
}

int native_ui_mixer_fader_db_at(int height, int y) {
    (void)height;
    return y;
}

bool native_ui_mixer_hit_test(int width, int height, int x, int y, int *slot, NativeUiMixerHit *zone) {
    (void)width; (void)height; (void)x; (void)y; (void)slot; (void)zone; return false;
}
void native_mixer_overlay_set_master_pct(App *app, int pct) {
    (void)app;
    (void)pct;
    fader_updates++;
}

void native_mixer_overlay_set_db(App *app, int slot, int gain) {
    (void)app;
    (void)slot;
    (void)gain;
    fader_updates++;
}

void native_mixer_overlay_select(App *app, int slot) {
    (void)app;
    (void)slot;
}

void native_mixer_overlay_toggle_mute(App *app, int slot) {
    (void)app;
    (void)slot;
}

void native_mixer_overlay_toggle_solo(App *app, int slot) {
    (void)app;
    (void)slot;
}

void native_mixer_overlay_toggle_duck(App *app, int slot) {
    (void)app;
    (void)slot;
}

void native_mixer_overlay_touch(App *app) {
    (void)app;
}

void native_mixer_overlay_show(App *app) {
    (void)app;
}

void native_mixer_overlay_adjust(App *app, int steps) {
    (void)app;
    (void)steps;
}

void native_mixer_overlay_sdl_key(App *app, const SDL_KeyboardEvent *event) {
    (void)app;
    (void)event;
}

void native_set_virtual_mouse_position(App *app, int x, int y) {
    atomic_store(&app->virtual_mouse_x, x);
    atomic_store(&app->virtual_mouse_y, y);
}
bool native_input_pointer_move(NativeInput *input, int x, int y) {
    (void)input;
    (void)x;
    (void)y;
    return true;
}

bool native_input_pointer_button(NativeInput *input, int x, int y, NativeInputButton button, bool down) {
    (void)input; (void)x; (void)y; (void)button; (void)down; return true;
}
uint8_t native_overlay_button_bit(uint8_t button) {
    return button >= 1u && button <= 8u ? (uint8_t)(1u << (button - 1u)) : 0u;
}
NativeInputButton native_button_from_sdl(uint8_t button) {
    (void)button;
    return 0;
}

void native_track_button(App *app, NativeInputButton button, bool down) {
    (void)app;
    (void)button;
    (void)down;
}

void native_send_scaled_wheel(App *app, int delta) {
    (void)app;
    (void)delta;
}

void native_cursor_note_platform_visibility(NativeCursor *cursor, bool visible) {
    (void)cursor;
    (void)visible;
}

void native_show_hub(App *app) {
    (void)app;
}

void native_request_session_switch(App *app, int slot) {
    (void)app;
    (void)slot;
}

void native_ring_switch(App *app, int direction) {
    (void)app;
    (void)direction;
}


bool __real_native_complete_session_switch(App *, int);
bool __wrap_native_complete_session_switch(App *app, int index) {
    if (fail_survivor) {
        atomic_store(&app->sessions[index].current_state, RDP_STATE_NETWORK_ERROR);
        atomic_store(&app->sessions[index].session_failed, true);
    }
    return __real_native_complete_session_switch(app, index);
}

static void init(App *app) {
    memset(app, 0, sizeof(*app));
    assert(pthread_mutex_init(&app->video_lock, NULL) == 0);
    assert(pthread_mutex_init(&app->redaction_lock, NULL) == 0);
    atomic_store(&app->running, true);
    app->interactive_ui = true;
    app->pending_switch_target = -1;
    app->hub_connect_target = -1;
    for (int i = 0; i < NATIVE_SETTINGS_MAX_SESSIONS; i++) {
        app->sessions[i].app = app;
        app->sessions[i].index = i;
    }
    ui_visible = true;
    grabs = overlay_hides = stops = fader_updates = 0;
    keyboard_focus = NULL;
    fail_survivor = false;
}

static void finish(App *app) {
    assert(pthread_mutex_destroy(&app->video_lock) == 0);
    assert(pthread_mutex_destroy(&app->redaction_lock) == 0);
}

static void test_focus_controls_grab_and_input_arming(void) {
    App app;
    NativeSettings settings = {0};
    SDL_Window window = {0};
    init(&app);
    app.sessions[0].rdp = (RdpSession *)1;
    atomic_store(&app.sessions[0].current_state, RDP_STATE_ACTIVE);
    app.window_unfocused = true;
    app.app_backgrounded = true;
    native_streaming_transition_tick(&app, NULL, &settings, &window);
    assert(grabs == 0 && app.window_unfocused && app.app_backgrounded && app.streaming_visible);
    assert(!atomic_load(&app.input.active));
    SDL_Event event = {.type = SDL_WINDOWEVENT};
    event.window.event = SDL_WINDOWEVENT_SHOWN;
    native_handle_sdl_event(&app, &window, NULL, &event);
    event.window.event = SDL_WINDOWEVENT_RESTORED;
    native_handle_sdl_event(&app, &window, NULL, &event);
    assert(grabs == 0 && app.window_unfocused);
    event.type = SDL_APP_WILLENTERFOREGROUND;
    native_handle_sdl_event(&app, &window, NULL, &event);
    assert(app.app_backgrounded);
    event.type = SDL_APP_DIDENTERFOREGROUND;
    native_handle_sdl_event(&app, &window, NULL, &event);
    assert(grabs == 0 && app.window_unfocused && !app.app_backgrounded);
    event.type = SDL_WINDOWEVENT;
    event.window.event = SDL_WINDOWEVENT_FOCUS_GAINED;
    native_handle_sdl_event(&app, &window, NULL, &event);
    assert(grabs == 0 && app.window_unfocused);
    keyboard_focus = &window;
    native_handle_sdl_event(&app, &window, NULL, &event);
    assert(grabs == 1 && !app.window_unfocused);
    native_streaming_transition_tick(&app, NULL, &settings, &window);
    assert(atomic_load(&app.input.active));
    event.window.event = SDL_WINDOWEVENT_FOCUS_LOST;
    keyboard_focus = NULL;
    atomic_store(&app.pointer_warp_pending, true);
    native_handle_sdl_event(&app, &window, NULL, &event);
    assert(!atomic_load(&app.pointer_warp_pending) && !atomic_load(&app.input.active));
    native_streaming_transition_tick(&app, NULL, &settings, &window);
    assert(!atomic_load(&app.input.active) && stops > 0);
    assert(native_start_streaming_input(&app) == NATIVE_INPUT_START_UNAVAILABLE && grabs == 1);
    finish(&app);
}

static void test_survivor_failure_returns_to_ui(void) {
    App app;
    init(&app);
    app.streaming_visible = true;
    ui_visible = false;
    for (int i = 0; i < 2; i++) {
        app.sessions[i].rdp = (RdpSession *)1;
        atomic_store(&app.sessions[i].current_state, RDP_STATE_ACTIVE);
    }
    atomic_store(&app.sessions[0].session_failed, true);
    atomic_store(&app.sessions[0].terminal_state, RDP_STATE_NETWORK_ERROR);
    fail_survivor = true;
    native_session_supervisor_tick(&app, NULL);
    assert(!native_any_slot_connected(&app) && !app.streaming_visible && ui_visible);
    native_session_supervisor_tick(&app, NULL);
    assert(!app.streaming_visible && ui_visible);
    finish(&app);
}

static void test_rgba_transition_releases_mixer_ownership(void) {
    App app;
    SDL_Renderer renderer = {0};
    init(&app);
    app.rgba = (NativeRgbaSurface *)1;
    app.rgba_owner_slot = 0;
    app.rgba_owner_epoch = 0;
    app.streaming_visible = true;
    app.mixer_overlay_visible = true;
    app.mixer_overlay_dragging = true;
    app.mixer_overlay_hide_ticks = 1;
    native_present_streaming_frame(&app, &renderer, NULL);
    assert(!app.mixer_overlay_visible && !app.mixer_overlay_dragging && overlay_hides == 1);
    finish(&app);
}

static void test_drag_cannot_survive_lost_button_state(void) {
    App app;
    SDL_Window window = {0};
    init(&app);
    app.streaming_visible = true;
    app.mixer_overlay_visible = true;
    app.mixer_overlay_dragging = true;
    SDL_Event event = {.type = SDL_WINDOWEVENT};
    event.window.event = SDL_WINDOWEVENT_FOCUS_LOST;
    native_handle_sdl_event(&app, &window, NULL, &event);
    assert(!app.mixer_overlay_dragging);
    keyboard_focus = &window;
    event.window.event = SDL_WINDOWEVENT_FOCUS_GAINED;
    native_handle_sdl_event(&app, &window, NULL, &event);
    event.type = SDL_MOUSEMOTION;
    event.motion.state = 0;
    native_handle_sdl_event(&app, &window, NULL, &event);
    assert(fader_updates == 0);
    app.mixer_overlay_dragging = true;
    native_handle_sdl_event(&app, &window, NULL, &event);
    assert(!app.mixer_overlay_dragging && fader_updates == 0);
    const uint32_t background_events[] = {SDL_APP_WILLENTERBACKGROUND, SDL_APP_DIDENTERBACKGROUND};
    for (size_t i = 0; i < sizeof(background_events) / sizeof(background_events[0]); i++) {
        app.mixer_overlay_dragging = true;
        event.type = background_events[i];
        native_handle_sdl_event(&app, &window, NULL, &event);
        assert(!app.mixer_overlay_dragging);
    }
    finish(&app);
}

static void test_mixer_motion_preserves_handoff_buttons(void) {
    App app;
    SDL_Window window = {0};
    init(&app);
    app.mixer_overlay_visible = true;
    app.mixer_overlay_pointer_buttons = SDL_BUTTON_LMASK;
    app.mixer_overlay_dismiss_pending = true;
    SDL_Event event = {.type = SDL_MOUSEMOTION};
    event.motion.state = 0;
    native_handle_sdl_event(&app, &window, NULL, &event);
    assert(app.mixer_overlay_pointer_buttons == SDL_BUTTON_LMASK);
    assert(app.mixer_overlay_visible && overlay_hides == 0);
    event.type = SDL_MOUSEBUTTONUP;
    event.button.button = SDL_BUTTON_LEFT;
    native_handle_sdl_event(&app, &window, NULL, &event);
    assert(!app.mixer_overlay_visible && overlay_hides == 1);
    finish(&app);
}

static void test_mixer_motion_recovers_lost_release(void) {
    App app;
    SDL_Window window = {0};
    init(&app);
    app.mixer_overlay_visible = true;
    app.mixer_overlay_dragging = true;
    app.mixer_overlay_pointer_buttons = SDL_BUTTON_LMASK | native_overlay_button_bit(SDL_BUTTON_X1);
    app.mixer_overlay_sdl_buttons = app.mixer_overlay_pointer_buttons;
    app.mixer_overlay_dismiss_button = SDL_BUTTON_X1;
    SDL_Event event = {.type = SDL_MOUSEMOTION};
    event.motion.state = SDL_BUTTON_LMASK;
    native_handle_sdl_event(&app, &window, NULL, &event);
    assert(app.mixer_overlay_pointer_buttons == SDL_BUTTON_LMASK);
    assert(app.mixer_overlay_dismiss_button == 0);
    assert(app.mixer_overlay_dismiss_pending);
    assert(app.mixer_overlay_visible && overlay_hides == 0);
    event.motion.state = 0;
    native_handle_sdl_event(&app, &window, NULL, &event);
    assert(app.mixer_overlay_pointer_buttons == 0);
    assert(!app.mixer_overlay_dragging && !app.mixer_overlay_visible);
    assert(overlay_hides == 1);
    finish(&app);
}

int main(void) {
    test_focus_controls_grab_and_input_arming();
    test_survivor_failure_returns_to_ui();
    test_rgba_transition_releases_mixer_ownership();
    test_drag_cannot_survive_lost_button_state();
    test_mixer_motion_recovers_lost_release();
    test_mixer_motion_preserves_handoff_buttons();
    puts("PASS native-runtime");
    return 0;
}
