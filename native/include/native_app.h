#ifndef GNOMECAST_NATIVE_APP_H
#define GNOMECAST_NATIVE_APP_H

/* The application state shared by the native modules split out of main.c: the
 * four session slots, the single App object, the tuning constants, and the
 * accessors every module needs. All mutable state lives in one App threaded by
 * pointer; there are no globals. */

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef HELLOLG_TARGET_WEBOS
#include <SDL.h>
#endif

#include "camera_v4l2.h"
#include "capture_redirect.h"
#include "cursor_sdl.h"
#include "native_json.h"
#include "native_time.h"
#ifdef HELLOLG_TARGET_WEBOS
#include "input_evdev.h"
#endif
#include "input_sdl.h"
#include "rdp_ffi.h"
#include "rdp_log.h"
#ifdef HELLOLG_TARGET_WEBOS
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

/* webOS always renders the app's graphics/UI plane on a virtual ~1920x1080 logical canvas
 * that the platform scales to the panel; the video decoder plane is independent and can run
 * at the panel's true native resolution regardless of this. There is no benefit to a locally
 * larger SDL surface, so this is fixed rather than user-selectable. */
#define NATIVE_LOCAL_SURFACE_WIDTH 1920
#define NATIVE_LOCAL_SURFACE_HEIGHT 1080

/* If the freshly switched-to session produces no decodable frame within this window,
 * force a reconnect (guaranteed IDR) — covers servers where neither suppress-resume nor
 * the display-control refresh yields a keyframe (e.g. grd mirror mode). */
#define NATIVE_SWITCH_KEYFRAME_TIMEOUT_MS 2000u
/* How long the colored active-session indicator stays on screen after a switch. */
#define NATIVE_INDICATOR_SHOW_MS 1500u
/* A snapshotted background stream must have been silent this long before its cache may
 * be replayed: covers the worker-queue + RTT tail after suppress (tens of ms) with a
 * wide margin, and rejects servers that ignore TS_SUPPRESS_OUTPUT_PDU outright. */
#define NATIVE_SNAPSHOT_QUIET_MS 500u
/* Hard ceiling on how long a deferred switch may keep the user on the old stream. The
 * normal worst case (2s keyframe deadline + reconnect + quiet window) fits well inside;
 * on expiry the press is answered with the immediate old-style switch — covers targets
 * that never quiet down (suppress ignored) or never produce an AU at all. */
#define NATIVE_PENDING_SWITCH_TIMEOUT_MS 8000u
/* Raw-evdev bypasses compositor pointer activity, so webOS cannot reliably restore a
 * cursor plane it hid on standby. Reassert on the first movement after a short pause and
 * occasionally during continuous movement; this is far below pointer-event frequency. */
#define NATIVE_CURSOR_ACTIVITY_IDLE_MS 750u
#define NATIVE_CURSOR_ACTIVITY_HEARTBEAT_MS 5000u
/* Volume-mixer overlay: the fader model constants (range, step, auto-hide) and both
 * renderers live in ui_mixer.h/.c; this file keeps the state machine and key routing. */
/* System-volume poll cadence while streaming: the LS2 getVolume round-trip is an
 * in-process bus call (no subprocess), cheap enough to keep the MASTER fader and the
 * auto-raise detector live at 5 Hz. */
#define NATIVE_SYSTEM_VOLUME_POLL_MS 200u
/* System-volume poll cadence while the overlay is up (~2 Hz keeps the MASTER knob in
 * step with the remote's VOL keys at negligible bus traffic). */
#define NATIVE_MIXER_OVERLAY_VOLUME_POLL_MS 500u

_Static_assert(NATIVE_AUDIO_PIPELINE_MAX_SOURCES >= NATIVE_SETTINGS_MAX_SESSIONS,
               "every session slot needs an audio source");

typedef struct App App;

/* One remote-button session slot (green/yellow). The slot owns its RDP session handle,
 * connection config (stable while connected — on_log redaction and reconnects read it),
 * per-session server state (desktop size, cursor) and its audio-pipeline routing. The slot
 * struct itself is the callback ctx for all RdpCallbacks of its session. */
typedef struct NativeSessionSlot {
    App *app;
    int index; /* NATIVE_SESSION_SLOT_* */
    RdpSession *rdp;
    NativeSessionConfig config;
    atomic_int current_state;
    atomic_int terminal_state;
    /* Set by worker callbacks on terminal events; drained on the SDL thread, which
     * decides between auto-switching to the surviving session and showing the UI. */
    atomic_bool session_failed;
    /* Server EGFX graphics output size (worker writes, SDL thread reads on switch). */
    atomic_uint desktop_width;
    atomic_uint desktop_height;
    /* Bumped by THIS slot's worker whenever one of its frames decodes OK (H.264 or
     * RemoteFX RGBA). The switch watchdog baselines the TARGET slot's counter: with a
     * single global counter, a frame the previous owner had in flight during the switch
     * could bump it after the baseline was armed and satisfy the watchdog without the
     * new slot ever decoding anything. */
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
    /* SDL thread only: a switch to this slot already had to fall back to the watchdog
     * reconnect — its server yields no keyframe on request (no Display Control channel,
     * Refresh Rect ignored). Subsequent switches reconnect immediately instead of
     * burning the watchdog timeout on a keyframe that will not come. */
    bool refresh_ineffective;
    /* Compressed-AU snapshot of this slot's latest background (re)connect: the connect
     * restart AU (SPS + PPS before IDR) plus deltas that raced the suppress. Guarded by
     * app->video_lock (this slot's worker appends while backgrounded; the SDL thread
     * arms, replays and resets). Replaying it on switch-to rebuilds exactly the reference
     * state the server's next delta assumes — see au_snapshot.h for the protocol story. */
    NativeAuSnapshot snapshot;
    /* Worker -> SDL thread: the snapshot is seeded with a cached decoder restart AU. */
    atomic_bool snapshot_idr_ready;
    /* SDL thread only: hidden background reconnect in flight for this slot; cleared when
     * the switch tick suppresses it (IDR cached) or a switch makes it active again. */
    bool snapshot_pending;
    /* SDL thread only: deadline for the pending snapshot to receive its first AU, armed
     * by the switch tick once the reconnect reaches ACTIVE (0 = not armed yet). A stream
     * that never feeds on_video_au — e.g. the RemoteFX bitmap path — would otherwise
     * stay pending and unsuppressed forever. */
    uint32_t snapshot_deadline_ticks;
    /* Monotonic-ms stamp of the latest background AU while the snapshot was armed
     * (worker writes, SDL thread reads). A stream still audible shortly before a replay
     * is either the (bounded) suppress tail or a server ignoring the suppress — either
     * way its in-flight AUs could interleave with the replay and corrupt the reference
     * chain, so the replay defers to the reconnect fallback until things go quiet. */
    atomic_uint snapshot_last_au_ms;
    /* SDL thread only: the current deferred switch already spent its one hidden
     * reconnect on this slot. A second no-AU deadline then means the stream does not
     * feed on_video_au at all (RemoteFX bitmap path) — retrying reconnects forever
     * would strand the switch, so the press is answered old-style instead. */
    bool snapshot_retry_used;
    /* Which graphics path this slot's stream last used (worker writes on every frame,
     * SDL thread reads). A bitmap-path slot cannot be snapshot-switched (no AUs to
     * cache) and must not be mistaken for a refresh-ineffective H.264 server: it
     * switches immediately the old way — resume simply restarts bitmap updates. */
    atomic_bool video_via_bitmap;
} NativeSessionSlot;

typedef struct App {
    NativeWebosPlatformInfo webos_platform;
    NativeSessionSlot sessions[NATIVE_SETTINGS_MAX_SESSIONS];
    /* Which slot owns the screen: it feeds the single hardware video plane (when
     * connected) or shows its configurator form (when not), and receives input. Written
     * on the SDL thread, read by every worker callback for routing. */
    atomic_int active_index;
    /* SDL thread only: keyframe deadline after a video switch (0 = no deadline).
     * switch_baseline_frames snapshots the WATCHED slot's video_ok_frames. */
    uint32_t switch_deadline_ticks;
    unsigned switch_baseline_frames;
    bool switch_reconnect_used;
    /* Set by worker callbacks that had to drop the live video track (audio-track open
     * reloads the shared pipeline); the SDL thread turns it into rdp_request_refresh +
     * the keyframe watchdog — gnome-remote-desktop never resends an IDR on its own. */
    atomic_bool video_refresh_needed;
    /* HELLOLG_SNAPSHOT_FORCE=1: IDR-snapshot backgrounding for every slot, not only
     * the learned refresh-ineffective ones (device experiment knob; set once in main). */
    bool snapshot_force;
    /* SDL thread only: a color-key switch whose target stream is not ready yet (-1 =
     * none). The screen stays on the LIVE current session while the target fills its AU
     * snapshot in the background (resume+refresh, or a hidden reconnect); the switch
     * tick completes the switch by replay once the cache is ready and quiet — the user
     * never watches a black reload window. */
    int pending_switch_target;
    /* SDL thread only: when the deferred switch gives up and answers old-style. */
    uint32_t pending_switch_deadline_ticks;
    /* Mutable bitmap canvas for exactly one stream generation. RemoteFX updates are
     * dirty rectangles, so reusing it across either a slot switch or a same-slot
     * reconnect would expose pixels from the previous desktop. Guarded by video_lock. */
    NativeRgbaSurface *rgba;
    int rgba_owner_slot;
    unsigned rgba_owner_epoch;
    /* Frozen RemoteFX desktop that was below HUB when an asynchronous replacement
     * started. The replacement renders into `rgba`; this separate owner-tagged canvas
     * remains the compositor background and BACK fallback until that exact replacement
     * generation reaches ACTIVE and decodes a frame. Its SDL texture is destroyed only
     * on the SDL thread. Guarded by video_lock. */
    NativeRgbaSurface *hub_return_rgba;
    int hub_return_rgba_owner_slot;
    unsigned hub_return_rgba_owner_epoch;
    int hub_return_replacement_slot;
    unsigned hub_return_replacement_epoch;
    unsigned hub_return_replacement_baseline_frames;
    /* Shared SS4S library/player owner; video and audio attach tracks to it. Its lifetime
     * and the video track use video_lock; the audio pointer/feed use audio_lock, while the
     * backend uses atomic generations for concurrent NDL feeds and serializes
     * only control-plane reload/lifecycle calls. */
    NativeMedia *media;
    NativeVideo *video;
    /* Which slot/connection generation the open video track decodes (video_lock). A
     * mismatch against the arriving AU's slot+epoch means the decoder holds a FOREIGN
     * stream's state: the swap to the new stream is deferred until its keyframe is in
     * hand, so the old picture stays up (no black gap) and the shared pipeline reloads
     * exactly once per switch (one audio interruption instead of two). */
    int video_owner_slot;
    unsigned video_owner_epoch;
    /* The single mixed-PCM audio track; all sessions flow through the fixed 48 kHz stereo
     * engine into it. Its pointer/lifetime and feed calls use audio_lock so RemoteFX RGBA
     * copies and SDL texture uploads under video_lock cannot stall the 10 ms PCM pump. */
    NativeAudio *audio;
    /* Headless miniaudio engine plus lock-free per-session adaptive sources. Its pump
     * thread feeds app->audio; the engine format never follows a source format. */
    NativeAudioPipeline audio_pipeline;
    uint16_t audio_codec;        /* NATIVE_AUDIO_CODEC_*: auto (Opus) or lossless PCM */
    /* Per-slot fader position in dB (NATIVE_MIXER_FADER_MIN_DB..MAX_DB, 3 dB steps,
     * default 0 = unity; the bottom stop mutes). Only the SDL thread writes it (volume
     * overlay). The pipeline gain target is atomic and survives source reopen. */
    int8_t mixer_gain_db[NATIVE_SETTINGS_MAX_SESSIONS];
    /* Notification ducking (SDL thread): duck_mask[i] = which slots' audio ducks slot i
     * -12 dB while slot i is on screen (bit = slot index, own bit ignored). Mirrors the
     * per-session settings duck_mask; the settings pointer is the persistence target for
     * the overlay's per-channel duck buttons. */
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
    /* Guards slot config strings between the SDL thread overwriting a slot's config on
     * (re)Connect and OTHER slots' rdp-workers scanning every config in
     * redact_if_sensitive. A slot's own worker is joined before its config is written,
     * but that never protected the cross-slot scan. */
    pthread_mutex_t redaction_lock;
    NativeInput input;
#ifdef HELLOLG_TARGET_WEBOS
    /* Raw evdev mouse+keyboard reader (active during streaming); one background thread polls
     * grabbed /dev/input devices and wakes the SDL loop through eventfd. */
    NativeEvdevInput evdev_input;
#endif
    int decoder_errors;
    bool decoder_keyframe_pending;
    /* Set once the SDL graphics layer has presented a single transparent frame so the
     * NDL hardware video plane underneath shows through. Re-presenting a transparent
     * frame every loop tick raced the video plane's own buffer swaps and produced
     * visible flicker, so this latches to "present once, then leave the window alone". */
    bool video_plane_punched;
    atomic_bool running;
    atomic_int exit_code;
    bool interactive_ui;
    /* Explicit launch-only diagnostic: show the locally attached V4L2 camera instead
     * of starting HUB/RDP. This is intentionally not persisted as profile state. */
    bool camera_preview;
    /* Debug-only: slot named by connectSlot/--connect-slot, or -1. */
    int debug_connect_slot;
#ifdef HELLOLG_TARGET_WEBOS
    /* Colored square shown briefly after a session switch (SDL thread only). */
    int indicator_slot;
    uint32_t indicator_until_ticks;
    bool indicator_drawn;
    /* Volume-mixer overlay over the live stream (SDL thread only): opened by re-pressing
     * the ACTIVE slot's color button; one vertical slider per slot adjusts that mixer
     * source live. */
    bool mixer_overlay_visible;
    int mixer_overlay_selected;
    uint32_t mixer_overlay_hide_ticks;
    /* Left button held on a fader: motion drags the selected knob (SDL thread only). */
    bool mixer_overlay_dragging;
    /* Pointer button pressed outside the floating console. Dismiss on its release so
     * that release is consumed before the evdev grab and RDP input are restored. */
    uint8_t mixer_overlay_dismiss_button;
    /* Pointer buttons physically down while the overlay owns the pointer (bit per SDL
     * button 1..8): no close may complete while any is held, or the re-grab would leak
     * that button's release into the session. */
    uint8_t mixer_overlay_pointer_buttons;
    bool mixer_overlay_dismiss_pending;
    /* OK/ENTER currently held (SDL thread only): evdev autorepeat arrives as extra
     * down events, and activation must fire once per press, not once per repeat —
     * a repeated activate would XOR the duck bit and rewrite settings every time. */
    bool mixer_overlay_ok_held;
    /* Last system volume seen by the auto-raise detector (-1 = no baseline yet) and the
     * next poll tick: com.webos.audio idles out between requests and drops
     * subscriptions, so the cache is kept live by polling getVolume over LS2 —
     * in-process calls, no fork, safe next to the video pipeline. */
    int system_volume_seen;
    /* Luna reply_seq recorded when streaming was (re)entered: the baseline may only be
     * taken from a reply newer than this — the cache itself can predate streaming. */
    unsigned system_volume_baseline_seq;
    uint32_t system_volume_poll_ticks;
    /* Next system-volume poll while the overlay is visible (the MASTER knob follows
     * remote/headphone volume changes by reading, not by trusting bus events). */
    uint32_t mixer_overlay_volume_poll_ticks;
    /* SDL thread only: an opaque switch splash currently covers the video plane (drawn
     * while the keyframe watchdog is armed, i.e. a swap is in flight; the pipeline
     * reload window would otherwise show through as a black screen). */
    bool switch_splash_drawn;
    /* SDL thread only: latched when the streaming screen's side effects ran (UI hidden,
     * evdev grabbed); cleared when the screen returns to a configurator. */
    bool streaming_visible;
    /* SDL thread only: the user explicitly opened HUB over a still-active session.
     * This keeps the normal ACTIVE-state auto-entry from immediately hiding HUB again. */
    bool hub_visible;
    /* A grabbed remote confirm press opens HUB on its release. Keeping both edges in
     * evdev prevents the compositor from inheriting an orphaned release when HUB drops
     * the grab (some webOS remotes translate that orphan into BACK). */
    bool hub_open_key_held;
    /* Same held-latch contract for the mouse's side/back button opening HUB. */
    bool hub_open_button_held;
    /* Session that owned the video plane when HUB opened. BACK returns here even if a
     * nested setup form temporarily selected or backgrounded another slot. */
    int hub_return_slot;
    /* Async Connect/Save-and-connect started from HUB. HUB stays logically open until
     * this slot reaches ACTIVE, so a failure still lets BACK resume hub_return_slot. */
    int hub_connect_target;
    /* SDL-thread logical-session clocks for the HUB's per-slot "Session N min"
     * metadata. A hidden snapshot/watchdog reconnect belongs to the same user session,
     * so its transient non-ACTIVE state must not restart this 64-bit monotonic clock. */
    uint64_t session_started_ms[NATIVE_SETTINGS_MAX_SESSIONS];
    bool session_runtime_active[NATIVE_SETTINGS_MAX_SESSIONS];
    /* SDL thread only: NumLock was synced to the current active session. Input arming is
     * derived on the SDL thread each tick (worker callbacks must not write shared input
     * state — a demoted slot's stale write could race a switch and strand input off). */
    bool input_locks_synced;
    /* SDL thread only: last connection state mirrored into the configurator status. */
    int ui_last_state;
#ifdef HELLOLG_TARGET_WEBOS
    /* Borrowed while native_run_app_loop is on the preconnect screen, so the SDL event
     * filter can route green/yellow remote keys to the slot selector. */
    NativePreconnectUi *preconnect_ui;
#endif
    uint16_t wheel_step;
    uint16_t wheel_scroll_divisor;
    int wheel_accumulator;
    /* Only ever written by the SDL thread (the evdev mouse drain and the pointer-clamp/warp
     * drains below), so plain last-writer-wins races with a second writer can't happen;
     * atomic only for torn-access safety against the RDP worker thread's reads via
     * native_send_scaled_wheel()/native_update_pointer_window_size(). */
    atomic_int virtual_mouse_x;
    atomic_int virtual_mouse_y;
    /* Set by the RDP worker thread (on_desktop_size, on a RDPGFX_RESET_GRAPHICS_PDU) to ask
     * the SDL thread to re-clamp virtual_mouse_x/y into the new window bounds on its next
     * loop tick, keeping the SDL thread the sole writer of virtual_mouse_x/y. */
    atomic_bool pointer_clamp_pending;
    /* Server-initiated pointer warp (on_pointer_position, RDP worker thread) in desktop
     * coordinates; the SDL thread repositions virtual_mouse_x/y and warps the real mouse on
     * its next tick so it stays the sole writer of virtual_mouse_x/y. */
    atomic_bool pointer_warp_pending;
    atomic_uint pointer_warp_x;
    atomic_uint pointer_warp_y;
    /* Which slot published the warp: the drain drops it when it no longer matches the
     * active slot — an outgoing worker can pass its active check right before a switch
     * and publish afterwards, and its desktop coordinates are meaningless (and visibly
     * jumpy) mapped through the NEW session's dimensions. */
    atomic_int pointer_warp_slot;
    atomic_uint render_width;
    atomic_uint render_height;
    /* SDL thread only. Set true on window FOCUS_LOST (a remote-invoked webOS overlay such as
     * the TV menu steals focus without backgrounding the app); while set, the evdev grabs are
     * released and the SDL pointer fallback is ignored so input drives the overlay, not RDP.
     * Zero-initialised, i.e. focused. */
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
    /* SDL thread only. Which inputs we have forwarded a down for but not yet an up, so their
     * releases can be flushed to the server before the evdev grab is dropped on focus loss —
     * otherwise the up goes to the overlay, not RDP, leaving a stuck drag or auto-repeating
     * key. held_mouse is indexed by NativeInputButton-1 (LEFT/RIGHT/MIDDLE); held_keys is a
     * bitset over scancode | (extended ? 0x100 : 0). */
    bool held_mouse[3];
    uint8_t held_keys[64];
    /* Set (under video_lock) by RDP worker-thread callbacks that need to drop the RGBA
     * surface's SDL texture but don't own the renderer's thread; drained and actually
     * destroyed on the SDL thread in native_present_rgba_frame()/native_stop_rdp(). */
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

/* Stopping an RDP worker is synchronous: rdp_session_stop() joins it. A worker that is
 * still resolving/connecting can be inside an uninterruptible libc/TCP timeout, so UI
 * actions must reject that transition instead of joining it on the SDL thread. */
static inline bool native_session_is_still_connecting(const NativeSessionSlot *slot) {
    return slot && slot->rdp && atomic_load(&slot->current_state) != (int)RDP_STATE_ACTIVE;
}

static inline bool native_slot_is_active(const NativeSessionSlot *slot) {
    return atomic_load(&slot->app->active_index) == slot->index;
}

/* A slot may take the screen while ACTIVE, or while its hidden reconnect is filling a
 * snapshot for a deferred switch. Keep this predicate in one place: navigation must
 * never select a slot whose worker has already reported a terminal failure. */
static inline bool native_slot_is_live_stream_target(const NativeSessionSlot *slot) {
    return slot && slot->rdp && !atomic_load(&slot->session_failed) &&
           (atomic_load(&slot->current_state) == (int)RDP_STATE_ACTIVE || slot->snapshot_pending);
}

#endif
