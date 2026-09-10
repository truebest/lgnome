#ifndef LGNOME_UI_MIXER_H
#define LGNOME_UI_MIXER_H

#include <stdbool.h>
#include <stdint.h>

#include "audio_pipeline.h"
#include "native_settings.h"

/* LVGL mixer view and dB fader model; lifecycle and input routing live in native_mixer_overlay. */

#define NATIVE_MIXER_FADER_MIN_DB (-60)
#define NATIVE_MIXER_FADER_MAX_DB 6
#define NATIVE_MIXER_OVERLAY_GAIN_STEP_DB 3
#define NATIVE_MIXER_OVERLAY_IDLE_HIDE_MS 6000u
#define NATIVE_MIXER_METER_DECAY_DB_S 30.0f
#define NATIVE_MIXER_METER_FLOOR_DB (-90.0f)

/* MASTER meters show app mix output; its fader controls webOS system volume. */
#define NATIVE_UI_MIXER_CHANNELS (NATIVE_SETTINGS_MAX_SESSIONS + 1)
#define NATIVE_UI_MIXER_MASTER NATIVE_SETTINGS_MAX_SESSIONS
#define NATIVE_UI_MIXER_MASTER_STEP_PCT 3

/* 3 dB-step fader position NATIVE_MIXER_FADER_MIN_DB..MAX_DB -> Q15 mixer gain
 * (round(32768 * 10^(dB/20)); a lookup table keeps libm out of the shared code path).
 * The bottom stop maps to 0: the fader floor is a full mute, not an audible -60 dB. */
int32_t native_ui_mixer_gain_db_to_q15(int gain_db);

/* Window-coordinate hit test: slot -1 means padding/gaps. Fader mapping clamps and snaps to 3 dB. */
typedef enum NativeUiMixerHit {
    NATIVE_UI_MIXER_HIT_BODY = 0, /* inside the channel, on no control: select only */
    NATIVE_UI_MIXER_HIT_FADER,
    NATIVE_UI_MIXER_HIT_DUCK,
    NATIVE_UI_MIXER_HIT_MUTE,
    NATIVE_UI_MIXER_HIT_SOLO,
} NativeUiMixerHit;
bool native_ui_mixer_hit_test(int win_w, int win_h, int x, int y, int *slot, NativeUiMixerHit *zone);
int native_ui_mixer_fader_db_at(int win_h, int y);

/* The MASTER fader value for y: system volume 0..100, clamped to the track (integer
 * steps — the system volume's own granularity). */
int native_ui_mixer_fader_pct_at(int win_h, int y);

#ifdef LGNOME_TARGET_WEBOS
#include <SDL.h>

typedef struct NativeUiMixer NativeUiMixer;

/* Create after registering the preconnect LVGL display; refresh texture on resize.
 * All functions tolerate NULL. */
NativeUiMixer *native_ui_mixer_create(SDL_Renderer *renderer);
void native_ui_mixer_destroy(NativeUiMixer *mixer);
void native_ui_mixer_set_texture(NativeUiMixer *mixer, SDL_Texture *texture);
/* Updates the four source-strip labels from persisted profile names/hosts. */
void native_ui_mixer_set_profiles(NativeUiMixer *mixer, const NativeSessionConfig *sessions);

/* Loads the overlay screen, remembering the active one to restore on hide, and drops the
 * display background so only the panel paints — the video plane fills the rest. */
void native_ui_mixer_show(NativeUiMixer *mixer);
void native_ui_mixer_hide(NativeUiMixer *mixer);
bool native_ui_mixer_active(const NativeUiMixer *mixer);

/* peaks has one L/R pair per channel, including MASTER; queue/target/gain arrays cover slots only.
 * master_pct < 0 means unknown; now_ticks uses SDL_GetTicks(). Render refreshes LVGL directly. */
void native_ui_mixer_render(NativeUiMixer *mixer, const int32_t (*peaks)[2], const unsigned *queue_ms,
                            const unsigned *target_ms, const int8_t *gain_db, int master_pct, int selected,
                            unsigned connected_mask, int active_slot, unsigned duck_mask,
                            unsigned mute_mask, unsigned solo_mask, uint32_t now_ticks);

#endif /* LGNOME_TARGET_WEBOS */

#endif
