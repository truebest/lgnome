#ifndef LGNOME_NATIVE_SETTINGS_H
#define LGNOME_NATIVE_SETTINGS_H

#include <stdbool.h>
#include <stdint.h>

/* Application settings model shared by config acquisition, persistence, the
 * native session runtime, and the on-TV UI. JSON encoding is deliberately kept
 * behind settings_json.h so model-only consumers do not depend on that codec. */

#define NATIVE_SETTINGS_STRING_MAX 512u
/* Remote color-button slots, in the remote's own button order: red, green, yellow, blue.
 * All four color keys are sessions; app exit is system-driven (webOS EXIT/home). */
#define NATIVE_SETTINGS_MAX_SESSIONS 4

#define NATIVE_SESSION_SLOT_RED 0
#define NATIVE_SESSION_SLOT_GREEN 1
#define NATIVE_SESSION_SLOT_YELLOW 2
#define NATIVE_SESSION_SLOT_BLUE 3

/* Highest valid duck_mask value: every other slot ducks this one (own bit ignored). */
#define NATIVE_SETTINGS_DUCK_MASK_ALL ((1u << NATIVE_SETTINGS_MAX_SESSIONS) - 1u)

typedef struct NativeSessionConfig {
    /* Optional human-facing profile label. Empty keeps the color/host fallback used by
     * callers and by settings written before the field was introduced. */
    char name[NATIVE_SETTINGS_STRING_MAX];
    char host[NATIVE_SETTINGS_STRING_MAX];
    char username[NATIVE_SETTINGS_STRING_MAX];
    char password[NATIVE_SETTINGS_STRING_MAX];
    char domain[NATIVE_SETTINGS_STRING_MAX];
    uint16_t port;
    uint16_t fps;
    /* Desktop size requested from THIS server ("desktopWidth"/"desktopHeight"). Servers
     * that mirror a real monitor ignore it; Windows builds its session at this size. */
    uint16_t desktop_width;
    uint16_t desktop_height;
    /* Which slots' audio ducks THIS session -12 dB while it is on screen ("duckTriggers"
     * JSON key, bit = slot index, own bit ignored; default: nobody -- ducking is opt-in).
     * Toggled from the mixer overlay's channel color-bar buttons, shown relative to the
     * active session. (Renamed from a short-lived "duckMask" key whose builds wrote an
     * all-on default; the old key is deliberately ignored so those masks reset.) */
    uint16_t duck_mask;
    /* Local capture devices are global, but each remote profile independently opts
     * into exposing the camera and microphone. Both default false. */
    bool camera_redirect;
    bool audio_input_redirect;
} NativeSessionConfig;

/* Global audio codec preference ("audioCodec" JSON key). AUTO advertises Opus+PCM and
 * the server picks Opus (~96kbps, in-process decode); PCM advertises PCM only for a
 * lossless stream (~1.4Mbps per session). Global, not per-slot; per-source converters
 * normalize simultaneous 44.1/48 kHz streams into the fixed 48 kHz graph. */
#define NATIVE_AUDIO_CODEC_AUTO 0
#define NATIVE_AUDIO_CODEC_PCM 1

typedef struct NativeSettings {
    NativeSessionConfig sessions[NATIVE_SETTINGS_MAX_SESSIONS];
    uint16_t wheel_step;
    uint16_t wheel_scroll_divisor;
    uint16_t audio_codec; /* NATIVE_AUDIO_CODEC_* */
    bool camera_enabled;
    char camera_device_id[NATIVE_SETTINGS_STRING_MAX];
    uint16_t camera_width;
    uint16_t camera_height;
    uint16_t camera_fps;
    bool audio_input_enabled;
    char audio_input_device_id[NATIVE_SETTINGS_STRING_MAX];
    int16_t audio_input_gain_db;
} NativeSettings;

/* Bounds for a profile's requested desktop size. The floor keeps a typo from asking for
 * a desktop no server will build; the ceiling is the largest panel this runs on. */
#define NATIVE_SETTINGS_DESKTOP_MIN_WIDTH 640u
#define NATIVE_SETTINGS_DESKTOP_MIN_HEIGHT 480u
#define NATIVE_SETTINGS_DESKTOP_MAX_WIDTH 3840u
#define NATIVE_SETTINGS_DESKTOP_MAX_HEIGHT 2160u

/* Human-facing slot name ("green"/"yellow"); "?" for out-of-range indices. */
const char *native_session_slot_name(int slot);

void native_settings_defaults(NativeSettings *settings);
/* Empty profile with valid connection defaults, also used when deleting a slot. */
void native_session_config_defaults(NativeSessionConfig *session);

#endif
