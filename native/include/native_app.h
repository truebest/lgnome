#ifndef LGNOME_NATIVE_APP_H
#define LGNOME_NATIVE_APP_H

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef LGNOME_TARGET_WEBOS
#include <SDL.h>
#endif

#include "camera_v4l2.h"
#include "capture_redirect.h"
#include "cursor_sdl.h"
#include "native_json.h"
#include "native_time.h"
#ifdef LGNOME_TARGET_WEBOS
#include "input_evdev.h"
#endif
#include "input_sdl.h"
#include "rdp_ffi.h"
#include "rdp_log.h"
#ifdef LGNOME_TARGET_WEBOS
#include "ui_preconnect.h"
#else
typedef void NativePreconnectUi;
#endif
#include "au_snapshot.h"
#include "audio_pipeline.h"
#include "audio_opus.h"
#include "luna_volume.h"
#include "ui_mixer.h"
#include "ui_slot_palette.h"
#include "audio_backend.h"
#include "media_backend.h"
#include "native_settings.h"
#include "video_rgba_sdl.h"
#include "video_backend.h"
#include "webos_platform.h"

/* webOS scales the SDL canvas independently of the hardware video plane. */
#define NATIVE_LOCAL_SURFACE_WIDTH 1920
#define NATIVE_LOCAL_SURFACE_HEIGHT 1080

#define NATIVE_SWITCH_KEYFRAME_TIMEOUT_MS 2000u
/* How long the colored active-session indicator stays on screen after a switch. */
#define NATIVE_INDICATOR_SHOW_MS 1500u
#define NATIVE_SNAPSHOT_QUIET_MS 500u
#define NATIVE_PENDING_SWITCH_TIMEOUT_MS 8000u
/* Raw evdev activity does not restore the webOS cursor plane after standby. */
#define NATIVE_CURSOR_ACTIVITY_IDLE_MS 750u
#define NATIVE_CURSOR_ACTIVITY_HEARTBEAT_MS 5000u
#define NATIVE_SYSTEM_VOLUME_POLL_MS 200u
#define NATIVE_MIXER_OVERLAY_VOLUME_POLL_MS 500u

_Static_assert(NATIVE_AUDIO_PIPELINE_MAX_SOURCES >= NATIVE_SETTINGS_MAX_SESSIONS,
               "every session slot needs an audio source");

typedef struct App App;

/* RDP callbacks use this slot as ctx. Config stays stable while its worker runs. */
typedef struct NativeSessionSlot {
    App *app;
    int index; /* NATIVE_SESSION_SLOT_* */
    RdpSession *rdp;
    NativeSessionConfig config;
    /* Codec flips within this connection only; guarded by App.video_lock. */
    int graphics_path_switches;
    atomic_int current_state;
    atomic_int terminal_state;
    atomic_int terminal_reason;
    atomic_bool reconnecting;
    atomic_bool was_active;
    /* Set by worker callbacks on terminal events; drained on the SDL thread, which
     * decides between auto-switching to the surviving session and showing the UI. */
    atomic_bool session_failed;
    /* Server EGFX graphics output size (worker writes, SDL thread reads on switch). */
    atomic_uint desktop_width;
    atomic_uint desktop_height;
    /* Per-slot progress; another slot must not satisfy this slot's switch watchdog. */
    atomic_uint video_ok_frames;
    /* Server-driven cursor. Background sessions keep caching their latest shape here;
     * the SDL thread applies only the active slot's cursor each tick. */
    NativeCursor cursor;
    /* Audio-pipeline routing (worker thread of this session only). The negotiated
     * format snapshot is atomic because the SDL thread mirrors it into HUB metadata. */
    bool audio_routed;
    bool audio_incompatible_logged;
    atomic_uint audio_codec;
    atomic_uint audio_sample_rate;
    atomic_uint audio_channels;
    /* Per-session Opus decoder (worker thread; created in on_audio_format, destroyed
     * after the worker is joined in native_stop_slot). NULL for raw-PCM sessions. */
    NativeOpusDecoder *opus_decoder;
    /* SDL thread only: whether we asked the server to suppress graphics (background). */
    bool suppressed;
    /* Bumped on every (re)connect of this slot; with the slot index it identifies which
     * stream generation the shared video decoder holds (see App.video_owner_*). */
    atomic_uint connect_epoch;
    /* Diagnostics: AUs of this stream dropped while waiting for its keyframe after a
     * switch (worker thread; logged throttled). */
    atomic_uint keyframe_wait_drops;
    /* SDL thread: server requires reconnect for a switch keyframe. */
    bool refresh_ineffective;
    /* Guarded by video_lock; background worker appends, SDL thread replays and resets. */
    NativeAuSnapshot snapshot;
    /* Worker -> SDL thread: the snapshot is seeded with a cached decoder restart AU. */
    atomic_bool snapshot_idr_ready;
    /* SDL thread only: hidden background reconnect in flight for this slot; cleared when
     * the switch tick suppresses it (IDR cached) or a switch makes it active again. */
    bool snapshot_pending;
    /* SDL-thread deadline for the first snapshot AU; 0 = unarmed. */
    uint32_t snapshot_deadline_ticks;
    /* Latest background AU arrival (monotonic ms); replay requires a quiet stream. */
    atomic_uint snapshot_last_au_ms;
    /* SDL thread: at most one hidden reconnect per deferred switch. */
    bool snapshot_retry_used;
    /* Worker publishes the graphics path; bitmap streams cannot use AU snapshots. */
    atomic_bool video_via_bitmap;
} NativeSessionSlot;

typedef struct App {
    NativeWebosPlatformInfo webos_platform;
    NativeSessionSlot sessions[NATIVE_SETTINGS_MAX_SESSIONS];
    /* Screen/input owner; SDL thread writes, worker callbacks read. */
    atomic_int active_index;
    /* SDL thread only: keyframe deadline after a video switch (0 = no deadline).
     * switch_baseline_frames snapshots the WATCHED slot's video_ok_frames. */
    uint32_t switch_deadline_ticks;
    unsigned switch_baseline_frames;
    bool switch_reconnect_used;
    /* Worker requests an SDL-thread refresh after a shared-pipeline reload. */
    atomic_bool video_refresh_needed;
    /* LGNOME_SNAPSHOT_FORCE=1: IDR-snapshot backgrounding for every slot, not only
     * the learned refresh-ineffective ones (device experiment knob; set once in main). */
    bool snapshot_force;
    /* SDL-thread deferred-switch target (-1 = none); current stream stays visible. */
    int pending_switch_target;
    /* SDL thread only: when the deferred switch gives up and answers old-style. */
    uint32_t pending_switch_deadline_ticks;
    /* video_lock: dirty-rectangle canvas owned by one slot and connection epoch. */
    NativeRgbaSurface *rgba;
    int rgba_owner_slot;
    unsigned rgba_owner_epoch;
    /* video_lock: frozen HUB return surface until the replacement renders.
     * SDL thread alone destroys its texture. */
    NativeRgbaSurface *hub_return_rgba;
    int hub_return_rgba_owner_slot;
    unsigned hub_return_rgba_owner_epoch;
    int hub_return_replacement_slot;
    unsigned hub_return_replacement_epoch;
    unsigned hub_return_replacement_baseline_frames;
    /* Media lifetime/video use video_lock; audio pointer/feed use audio_lock. */
    NativeMedia *media;
    NativeVideo *video;
    /* video_lock: decoder owner. Handoff requires the new owner's keyframe. */
    int video_owner_slot;
    unsigned video_owner_epoch;
    /* audio_lock: mixed 48 kHz stereo track and feed, independent of video_lock. */
    NativeAudio *audio;
    /* Headless miniaudio engine plus lock-free per-session adaptive sources. Its pump
     * thread feeds app->audio; the engine format never follows a source format. */
    NativeAudioPipeline audio_pipeline;
    uint16_t audio_codec;        /* NATIVE_AUDIO_CODEC_*: auto (Opus) or lossless PCM */
    /* SDL-thread fader dB; gain survives source reopen. Bottom stop mutes. */
    int8_t mixer_gain_db[NATIVE_SETTINGS_MAX_SESSIONS];
    /* SDL thread: duck_mask[i] selects triggers for slot i; its own bit is ignored. */
    uint8_t duck_mask[NATIVE_SETTINGS_MAX_SESSIONS];
    /* Console M/S state (SDL thread; bit = slot). Runtime-only like the faders — never
     * persisted; mute wins over solo inside the pipeline. */
    uint8_t mixer_mute_mask;
    uint8_t mixer_solo_mask;
    NativeSettings *settings;
    /* Dedicated native-H.264 V4L2 and ALSA workers. Only the active full-screen slot
     * owns physical capture; each RDP worker contributes protocol demand/credits. */
    NativeCaptureRedirect capture_redirect;
    /* Last configuration handed to the workers. Restarting them removes the camera
     * from every connected profile, so a per-profile permission edit must not do it. */
    NativeCaptureRedirectConfig capture_applied;
    char capture_applied_camera_id[NATIVE_SETTINGS_STRING_MAX];
    char capture_applied_audio_id[NATIVE_SETTINGS_STRING_MAX];
    bool capture_applied_valid;
    /* System-volume bridge for the mixer overlay's MASTER fader (luna_volume.h): the
     * fader mirrors and drives the webOS volume, never the app's own mix. */
    NativeLunaVolume luna_volume;
    /* Lock order is video_lock -> audio_lock everywhere both are needed. */
    pthread_mutex_t video_lock;
    pthread_mutex_t audio_lock;
    /* Protects config writes against other workers' credential-redaction scans. */
    pthread_mutex_t redaction_lock;
    NativeInput input;
#ifdef LGNOME_TARGET_WEBOS
    /* Raw evdev mouse+keyboard reader (active during streaming); one background thread polls
     * grabbed /dev/input devices and wakes the SDL loop through eventfd. */
    NativeEvdevInput evdev_input;
#endif
    int decoder_errors;
    /* Still-region refinement tiles dropped because the H.264 plane was live. */
    unsigned tiles_dropped_under_video;
    /* Rolling window behind the video pacing summary (see native_rdp_video.c). */
    unsigned video_frames_window;
    uint64_t video_bytes_window;
    uint32_t video_pacing_window_ms;
    uint32_t video_last_feed_ms;
    bool video_pacing_started;
    bool video_feed_started;
    bool decoder_keyframe_pending;
    /* Present the transparent SDL frame once; repeated presents flicker over NDL. */
    bool video_plane_punched;
    atomic_bool running;
    atomic_int exit_code;
    bool interactive_ui;
    /* Explicit launch-only diagnostic: show the locally attached V4L2 camera instead
     * of starting HUB/RDP. This is intentionally not persisted as profile state. */
    bool camera_preview;
    /* Debug-only: slot named by connectSlot/--connect-slot, or -1. */
    int debug_connect_slot;
#ifdef LGNOME_TARGET_WEBOS
    /* Colored square shown briefly after a session switch (SDL thread only). */
    int indicator_slot;
    uint32_t indicator_until_ticks;
    bool indicator_drawn;
    /* SDL-thread mixer overlay state. */
    bool mixer_overlay_visible;
    int mixer_overlay_selected;
    uint32_t mixer_overlay_hide_ticks;
    /* Left button held on a fader: motion drags the selected knob (SDL thread only). */
    bool mixer_overlay_dragging;
    /* Pointer button pressed outside the floating console. Dismiss on its release so
     * that release is consumed before the evdev grab and RDP input are restored. */
    uint8_t mixer_overlay_dismiss_button;
    /* SDL button bits 1..8; defer overlay close until all buttons are released. */
    uint8_t mixer_overlay_pointer_buttons;
    bool mixer_overlay_dismiss_pending;
    /* SDL-thread press latch: ignore evdev autorepeat. */
    bool mixer_overlay_ok_held;
    /* System-volume baseline (-1 = none); LS2 subscriptions require polling fallback. */
    int system_volume_seen;
    /* Luna reply_seq recorded when streaming was (re)entered: the baseline may only be
     * taken from a reply newer than this — the cache itself can predate streaming. */
    unsigned system_volume_baseline_seq;
    uint32_t system_volume_poll_ticks;
    /* Next system-volume poll while the overlay is visible (the MASTER knob follows
     * remote/headphone volume changes by reading, not by trusting bus events). */
    uint32_t mixer_overlay_volume_poll_ticks;
    /* SDL-thread splash covers the decoder reload during a switch. */
    bool switch_splash_drawn;
    /* SDL thread only: latched when the streaming screen's side effects ran (UI hidden,
     * evdev grabbed); cleared when the screen returns to a configurator. */
    bool streaming_visible;
    /* SDL thread only: the user explicitly opened HUB over a still-active session.
     * This keeps the normal ACTIVE-state auto-entry from immediately hiding HUB again. */
    bool hub_visible;
    /* Open HUB on release so dropping the grab cannot leak an orphaned key edge. */
    bool hub_open_key_held;
    /* Same held-latch contract for the mouse's side/back button opening HUB. */
    bool hub_open_button_held;
    /* Session that owned the video plane when HUB opened. BACK returns here even if a
     * nested setup form temporarily selected or backgrounded another slot. */
    int hub_return_slot;
    /* Async Connect/Save-and-connect started from HUB. HUB stays logically open until
     * this slot reaches ACTIVE, so a failure still lets BACK resume hub_return_slot. */
    int hub_connect_target;
    /* SDL-thread session duration in seconds; hidden reconnects preserve the start. */
    uint32_t session_started_s[NATIVE_SETTINGS_MAX_SESSIONS];
    bool session_runtime_active[NATIVE_SETTINGS_MAX_SESSIONS];
    /* SDL thread owns input arming and NumLock sync; workers must not write them. */
    bool input_locks_synced;
    /* SDL thread only: last connection state mirrored into the configurator status. */
    int ui_last_state;
#ifdef LGNOME_TARGET_WEBOS
    /* Borrowed while native_run_app_loop is on the preconnect screen, so the SDL event
     * filter can route green/yellow remote keys to the slot selector. */
    NativePreconnectUi *preconnect_ui;
#endif
    uint16_t wheel_step;
    uint16_t wheel_scroll_divisor;
    int wheel_accumulator;
    /* SDL thread is the sole writer; workers read these coordinates atomically. */
    atomic_int virtual_mouse_x;
    atomic_int virtual_mouse_y;
    /* Worker requests an SDL-thread coordinate clamp after desktop resize. */
    atomic_bool pointer_clamp_pending;
    /* Worker-published desktop coordinates; SDL thread applies the pointer warp. */
    atomic_bool pointer_warp_pending;
    atomic_uint pointer_warp_x;
    atomic_uint pointer_warp_y;
    /* Drop queued warps whose owner no longer matches the active slot. */
    atomic_int pointer_warp_slot;
    atomic_uint render_width;
    atomic_uint render_height;
    /* SDL thread: focus loss releases evdev and disables SDL RDP input routing. */
    bool window_unfocused;
    /* SDL thread only. webOS can background the app without a matching FOCUS_LOST
     * event; keep this separate from window focus for lifecycle/input handling. */
    bool app_backgrounded;
    /* SDL thread only. Armed by focus regain and webOS cursor-hide pseudo-events; consumed
     * by real mouse movement so the visibility request coincides with pointer activity. */
    bool cursor_reassert_pending;
    /* SDL ticks for the low-rate activity recovery. A first move after idle catches LSM
     * standby hides; the heartbeat catches cursor-plane loss during continuous movement. */
    uint32_t cursor_last_activity_ticks;
    uint32_t cursor_last_reassert_ticks;
    /* SDL-thread forwarded-down state, released before ungrab.
     * held_mouse: NativeInputButton-1; held_keys: scancode | (extended ? 0x100 : 0). */
    bool held_mouse[3];
    uint8_t held_keys[64];
    /* video_lock: worker requests texture disposal; SDL thread performs it. */
    SDL_Texture *pending_texture_destroy;
#endif
} App;

/* Marks the composited screen stale: the next present tick re-punches the
 * hole frame (or redraws) instead of trusting what is on screen. */
static inline void native_present_invalidate(App *app) {
    app->video_plane_punched = false;
}

static inline NativeSessionSlot *native_active_slot(App *app) {
    return &app->sessions[atomic_load(&app->active_index)];
}

/* rdp_session_stop joins the worker; reject transitions that would block the UI. */
static inline bool native_session_is_still_connecting(const NativeSessionSlot *slot) {
    return slot && slot->rdp && atomic_load(&slot->current_state) != (int)RDP_STATE_ACTIVE;
}

static inline bool native_slot_is_active(const NativeSessionSlot *slot) {
    return atomic_load(&slot->app->active_index) == slot->index;
}

static inline bool native_slot_is_live_stream_target(const NativeSessionSlot *slot) {
    return slot && slot->rdp && !atomic_load(&slot->session_failed) &&
           (atomic_load(&slot->current_state) == (int)RDP_STATE_ACTIVE || slot->snapshot_pending);
}

#endif
