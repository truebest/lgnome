#ifndef GNOMECAST_UI_MIXER_GEOMETRY_H
#define GNOMECAST_UI_MIXER_GEOMETRY_H

/* Mixer-overlay layout constants and pure geometry (see ui_mixer_geometry.c).
 * No LVGL/SDL dependency: compiled and tested on the host. The public
 * hit-test/fader entry points are declared in ui_mixer.h. */

#include <stdbool.h>

#include "ui_mixer.h"

#define UI_MIXER_BANK_W 1260
#define UI_MIXER_BANK_H 668
#define UI_MIXER_PANEL_PAD_X 32
#define UI_MIXER_PANEL_PAD_TOP 22
#define UI_MIXER_PANEL_PAD_BOTTOM 22
#define UI_MIXER_PANEL_W (UI_MIXER_BANK_W + 2 * UI_MIXER_PANEL_PAD_X)
#define UI_MIXER_PANEL_H (UI_MIXER_BANK_H + UI_MIXER_PANEL_PAD_TOP + UI_MIXER_PANEL_PAD_BOTTOM)
#define UI_MIXER_PANEL_BOTTOM 48
#define UI_MIXER_PANEL_RADIUS 30
#define UI_MIXER_CHANNEL_Y 52
#define UI_MIXER_CHANNEL_H 570
#define UI_MIXER_SOURCE_W 218
#define UI_MIXER_MASTER_W 250
#define UI_MIXER_CHANNEL_GAP 18
#define UI_MIXER_MASTER_GAP 16
#define UI_MIXER_GROUP_W                                                                                              \
    (NATIVE_SETTINGS_MAX_SESSIONS * UI_MIXER_SOURCE_W + NATIVE_SETTINGS_MAX_SESSIONS * UI_MIXER_CHANNEL_GAP +        \
     UI_MIXER_MASTER_GAP + UI_MIXER_MASTER_W)
#define UI_MIXER_GROUP_X ((UI_MIXER_BANK_W - UI_MIXER_GROUP_W) / 2)
#define UI_MIXER_FADER_Y 100
#define UI_MIXER_FADER_H 380
#define UI_MIXER_METER_W 16
#define UI_MIXER_METER_GAP 22
#define UI_MIXER_KNOB_W 66
#define UI_MIXER_KNOB_H 26
#define UI_MIXER_PANEL_COLOR 0x10141b
#define UI_MIXER_STRIP_COLOR 0x101319
#define UI_MIXER_MASTER_COLOR 0x171a20
/* Peak-hold marks: a detached segment riding above each meter bar at the loudest
 * recent level — held, then released (console bridge style). */
#define UI_MIXER_PEAK_MARK_H 5
#define UI_MIXER_PEAK_HOLD_MS 1500u
#define UI_MIXER_PEAK_DECAY_DB_S 20.0f
/* Clip threshold: the honest 0 dBFS. The meters see the PRE-saturation sum, so unlike
 * hardware meters (which cannot read past their rail and warn early at ~-0.3 dB) red
 * here means the clamp really cut samples; a legal full-scale hit stays yellow. */
#define UI_MIXER_CLIP_DB (0.0f)
/* The channel's identity color lives in its fader KNOB (the MASTER's is white); the
 * channel bottom carries the M / D / S button row (mute, duck-trigger, solo — session
 * channels only). The split constants divide the bottom pointer band between the three
 * plates at the midpoints between them. */
#define UI_MIXER_MS_W 54
#define UI_MIXER_MS_H 54
#define UI_MIXER_MS_BOTTOM 18
#define UI_MIXER_MS_MUTE_X 18
#define UI_MIXER_MS_DUCK_X 82
#define UI_MIXER_MS_SOLO_X 146
#define UI_MIXER_MS_SPLIT_L 77
#define UI_MIXER_MS_SPLIT_R 141

int ui_mixer_master_pct_clamped(int pct);
int ui_mixer_channel_width(int slot);
int ui_mixer_channel_x(int slot);
int ui_mixer_pair_cx(int slot);
int ui_mixer_db_to_y(int db);
int ui_mixer_pct_to_y(int pct);
int ui_mixer_panel_x(int win_w);
int ui_mixer_panel_y(int win_h);
bool ui_mixer_point_in_panel(int win_w, int win_h, int x, int y);
void native_ui_mixer_geometry_log_panel(int win_w, int win_h);

#endif
