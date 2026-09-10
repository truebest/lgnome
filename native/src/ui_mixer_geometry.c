/* Layout arithmetic for the volume-mixer overlay: the floating console and
 * centered bank are deterministic, so pointer hit-testing, fader value
 * mapping, and every widget position are pure integer math with no LVGL
 * traversal in the SDL event path - and host-testable (test-ui-mixer-geometry)
 * where the LVGL renderer never compiles. */

#include "ui_mixer_geometry.h"

/* Both faders travel the same track; only the value domain differs (dB vs percent). */
int ui_mixer_master_pct_clamped(int pct) {
    if (pct < 0) {
        return 0; /* unknown volume parks the (dimmed) knob at the bottom stop */
    }
    return pct > 100 ? 100 : pct;
}

int ui_mixer_channel_width(int slot) {
    return slot == NATIVE_UI_MIXER_MASTER ? UI_MIXER_MASTER_W : UI_MIXER_SOURCE_W;
}

int ui_mixer_channel_x(int slot) {
    if (slot == NATIVE_UI_MIXER_MASTER) {
        return UI_MIXER_GROUP_X + NATIVE_SETTINGS_MAX_SESSIONS * (UI_MIXER_SOURCE_W + UI_MIXER_CHANNEL_GAP) +
               UI_MIXER_MASTER_GAP;
    }
    return UI_MIXER_GROUP_X + slot * (UI_MIXER_SOURCE_W + UI_MIXER_CHANNEL_GAP);
}

int ui_mixer_pair_cx(int slot) {
    return ui_mixer_channel_width(slot) / 2 + 24;
}

int ui_mixer_db_to_y(int db) {
    return UI_MIXER_FADER_Y +
           (NATIVE_MIXER_FADER_MAX_DB - db) * UI_MIXER_FADER_H / (NATIVE_MIXER_FADER_MAX_DB - NATIVE_MIXER_FADER_MIN_DB);
}

int ui_mixer_pct_to_y(int pct) {
    return UI_MIXER_FADER_Y + (100 - ui_mixer_master_pct_clamped(pct)) * UI_MIXER_FADER_H / 100;
}

/* The floating console and centered bank are deterministic, so pointer hit-testing
 * remains pure arithmetic — no LVGL traversal in the SDL event path. */

int ui_mixer_panel_x(int win_w) {
    return (win_w - UI_MIXER_PANEL_W) / 2;
}

int ui_mixer_panel_y(int win_h) {
    return win_h - UI_MIXER_PANEL_BOTTOM - UI_MIXER_PANEL_H;
}

bool ui_mixer_point_in_panel(int win_w, int win_h, int x, int y) {
    int panel_x = ui_mixer_panel_x(win_w);
    int panel_y = ui_mixer_panel_y(win_h);
    if (x < panel_x || x >= panel_x + UI_MIXER_PANEL_W || y < panel_y || y >= panel_y + UI_MIXER_PANEL_H) {
        return false;
    }

    /* The shadow is deliberately not interactive, and the transparent pixels outside
     * the 30 px corner arcs count as outside the form too. */
    int rel_x = x - panel_x;
    int rel_y = y - panel_y;
    int dx = 0;
    int dy = 0;
    if (rel_x < UI_MIXER_PANEL_RADIUS) {
        dx = UI_MIXER_PANEL_RADIUS - rel_x;
    } else if (rel_x >= UI_MIXER_PANEL_W - UI_MIXER_PANEL_RADIUS) {
        dx = rel_x - (UI_MIXER_PANEL_W - UI_MIXER_PANEL_RADIUS - 1);
    }
    if (rel_y < UI_MIXER_PANEL_RADIUS) {
        dy = UI_MIXER_PANEL_RADIUS - rel_y;
    } else if (rel_y >= UI_MIXER_PANEL_H - UI_MIXER_PANEL_RADIUS) {
        dy = rel_y - (UI_MIXER_PANEL_H - UI_MIXER_PANEL_RADIUS - 1);
    }
    return dx == 0 || dy == 0 || dx * dx + dy * dy <= UI_MIXER_PANEL_RADIUS * UI_MIXER_PANEL_RADIUS;
}

bool native_ui_mixer_hit_test(int win_w, int win_h, int x, int y, int *slot, NativeUiMixerHit *zone) {
    if (slot) {
        *slot = -1;
    }
    if (zone) {
        *zone = NATIVE_UI_MIXER_HIT_BODY;
    }
    int panel_y = ui_mixer_panel_y(win_h);
    if (!ui_mixer_point_in_panel(win_w, win_h, x, y)) {
        return false;
    }
    int bank_x = (win_w - UI_MIXER_BANK_W) / 2;
    int rel_x = x - bank_x;
    int channel_y = panel_y + UI_MIXER_PANEL_PAD_TOP + UI_MIXER_CHANNEL_Y;
    int cy = y - channel_y;
    int cx = -1; /* channel-relative x once a slot resolves */
    int hit_slot = -1;
    if (cy >= 0 && cy < UI_MIXER_CHANNEL_H) {
        for (int i = 0; i < NATIVE_UI_MIXER_CHANNELS; i++) {
            int channel_x = ui_mixer_channel_x(i);
            int channel_w = ui_mixer_channel_width(i);
            if (rel_x >= channel_x && rel_x < channel_x + channel_w) {
                hit_slot = i;
                if (slot) {
                    *slot = i;
                }
                cx = rel_x - channel_x;
                break;
            }
        }
    }
    if (zone) {
        /* Fader band: within the track vertically (a knob's worth of slop at both
         * ends) — a click there jumps/drags the fader; elsewhere in the channel it only
         * selects, so clicking a label must not slam the level to the bottom stop. */
        int fy = cy - UI_MIXER_FADER_Y;
        if (cx >= 0 && fy >= -UI_MIXER_KNOB_H / 2 && fy <= UI_MIXER_FADER_H + UI_MIXER_KNOB_H / 2) {
            *zone = NATIVE_UI_MIXER_HIT_FADER;
        } else if (hit_slot >= 0 && hit_slot < NATIVE_UI_MIXER_MASTER &&
                   cy >= UI_MIXER_CHANNEL_H - UI_MIXER_MS_BOTTOM - UI_MIXER_MS_H - 6 &&
                   cy <= UI_MIXER_CHANNEL_H - 2) {
            /* Bottom controls band, split at the midpoints between the M / D / S plates. */
            *zone = cx <= UI_MIXER_MS_SPLIT_L   ? NATIVE_UI_MIXER_HIT_MUTE
                    : cx >= UI_MIXER_MS_SPLIT_R ? NATIVE_UI_MIXER_HIT_SOLO
                                                : NATIVE_UI_MIXER_HIT_DUCK;
        }
    }
    return true;
}

int native_ui_mixer_fader_db_at(int win_h, int y) {
    const int span = NATIVE_MIXER_FADER_MAX_DB - NATIVE_MIXER_FADER_MIN_DB;
    int track_y = ui_mixer_panel_y(win_h) + UI_MIXER_PANEL_PAD_TOP + UI_MIXER_CHANNEL_Y + UI_MIXER_FADER_Y;
    int fy = y - track_y;
    if (fy < 0) {
        fy = 0;
    }
    if (fy > UI_MIXER_FADER_H) {
        fy = UI_MIXER_FADER_H;
    }
    int db = NATIVE_MIXER_FADER_MAX_DB - (fy * span + UI_MIXER_FADER_H / 2) / UI_MIXER_FADER_H;
    /* Snap onto the 3 dB fader steps (the gain LUT is indexed by them). */
    db = ((db - NATIVE_MIXER_FADER_MIN_DB + NATIVE_MIXER_OVERLAY_GAIN_STEP_DB / 2) /
          NATIVE_MIXER_OVERLAY_GAIN_STEP_DB) *
             NATIVE_MIXER_OVERLAY_GAIN_STEP_DB +
         NATIVE_MIXER_FADER_MIN_DB;
    if (db > NATIVE_MIXER_FADER_MAX_DB) {
        db = NATIVE_MIXER_FADER_MAX_DB;
    }
    return db;
}

int native_ui_mixer_fader_pct_at(int win_h, int y) {
    int track_y = ui_mixer_panel_y(win_h) + UI_MIXER_PANEL_PAD_TOP + UI_MIXER_CHANNEL_Y + UI_MIXER_FADER_Y;
    int fy = y - track_y;
    if (fy < 0) {
        fy = 0;
    }
    if (fy > UI_MIXER_FADER_H) {
        fy = UI_MIXER_FADER_H;
    }
    return 100 - (fy * 100 + UI_MIXER_FADER_H / 2) / UI_MIXER_FADER_H;
}
