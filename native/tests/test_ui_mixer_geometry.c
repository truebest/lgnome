#include "ui_mixer_geometry.h"

#include <assert.h>
#include <stdio.h>

/* The overlay renders on the fixed 1920x1080 logical canvas. */
#define WIN_W 1920
#define WIN_H 1080

static int track_top(void) {
    return ui_mixer_panel_y(WIN_H) + UI_MIXER_PANEL_PAD_TOP + UI_MIXER_CHANNEL_Y + UI_MIXER_FADER_Y;
}

static void test_panel_bounds_and_corners(void) {
    int px = ui_mixer_panel_x(WIN_W);
    int py = ui_mixer_panel_y(WIN_H);
    assert(ui_mixer_point_in_panel(WIN_W, WIN_H, px + UI_MIXER_PANEL_W / 2, py + UI_MIXER_PANEL_H / 2));
    assert(!ui_mixer_point_in_panel(WIN_W, WIN_H, px - 1, py));
    assert(!ui_mixer_point_in_panel(WIN_W, WIN_H, px + UI_MIXER_PANEL_W, py));
    /* The very corner pixel sits outside the 30px arc; the arc's inner edge is in. */
    assert(!ui_mixer_point_in_panel(WIN_W, WIN_H, px, py));
    assert(ui_mixer_point_in_panel(WIN_W, WIN_H, px + UI_MIXER_PANEL_RADIUS, py + UI_MIXER_PANEL_RADIUS));
}

static void test_hit_zones(void) {
    int slot = -2;
    NativeUiMixerHit zone;
    /* Outside the console: no hit, slot resets. */
    assert(!native_ui_mixer_hit_test(WIN_W, WIN_H, 0, 0, &slot, &zone));
    assert(slot == -1);

    /* Center of channel 0's fader track. */
    int bank_x = (WIN_W - UI_MIXER_BANK_W) / 2;
    int fader_x = bank_x + ui_mixer_channel_x(0) + ui_mixer_channel_width(0) / 2;
    int fader_y = track_top() + UI_MIXER_FADER_H / 2;
    assert(native_ui_mixer_hit_test(WIN_W, WIN_H, fader_x, fader_y, &slot, &zone));
    assert(slot == 0);
    assert(zone == NATIVE_UI_MIXER_HIT_FADER);

    /* Bottom controls band: M / D / S split at the plate midpoints. */
    int channel_y = ui_mixer_panel_y(WIN_H) + UI_MIXER_PANEL_PAD_TOP + UI_MIXER_CHANNEL_Y;
    int plates_y = channel_y + UI_MIXER_CHANNEL_H - UI_MIXER_MS_BOTTOM - UI_MIXER_MS_H / 2;
    int ch1_x = bank_x + ui_mixer_channel_x(1);
    assert(native_ui_mixer_hit_test(WIN_W, WIN_H, ch1_x + UI_MIXER_MS_MUTE_X + UI_MIXER_MS_W / 2, plates_y,
                                    &slot, &zone));
    assert(slot == 1 && zone == NATIVE_UI_MIXER_HIT_MUTE);
    assert(native_ui_mixer_hit_test(WIN_W, WIN_H, ch1_x + UI_MIXER_MS_DUCK_X + UI_MIXER_MS_W / 2, plates_y,
                                    &slot, &zone));
    assert(slot == 1 && zone == NATIVE_UI_MIXER_HIT_DUCK);
    assert(native_ui_mixer_hit_test(WIN_W, WIN_H, ch1_x + UI_MIXER_MS_SOLO_X + UI_MIXER_MS_W / 2, plates_y,
                                    &slot, &zone));
    assert(slot == 1 && zone == NATIVE_UI_MIXER_HIT_SOLO);

    /* MASTER has no M/D/S plates: the same band is body-only. */
    int master_x = bank_x + ui_mixer_channel_x(NATIVE_UI_MIXER_MASTER) + UI_MIXER_MS_MUTE_X;
    assert(native_ui_mixer_hit_test(WIN_W, WIN_H, master_x, plates_y, &slot, &zone));
    assert(slot == NATIVE_UI_MIXER_MASTER && zone == NATIVE_UI_MIXER_HIT_BODY);
}

static void test_fader_values(void) {
    /* Track ends clamp to the range ends; above/below the track clamps too. */
    assert(native_ui_mixer_fader_db_at(WIN_H, track_top()) == NATIVE_MIXER_FADER_MAX_DB);
    assert(native_ui_mixer_fader_db_at(WIN_H, track_top() - 500) == NATIVE_MIXER_FADER_MAX_DB);
    assert(native_ui_mixer_fader_db_at(WIN_H, track_top() + UI_MIXER_FADER_H) == NATIVE_MIXER_FADER_MIN_DB);
    assert(native_ui_mixer_fader_db_at(WIN_H, WIN_H + 500) == NATIVE_MIXER_FADER_MIN_DB);
    /* Every returned value sits on the 3 dB grid. */
    for (int y = track_top(); y <= track_top() + UI_MIXER_FADER_H; y += 7) {
        int db = native_ui_mixer_fader_db_at(WIN_H, y);
        assert(db >= NATIVE_MIXER_FADER_MIN_DB && db <= NATIVE_MIXER_FADER_MAX_DB);
        assert((db - NATIVE_MIXER_FADER_MIN_DB) % NATIVE_MIXER_OVERLAY_GAIN_STEP_DB == 0);
    }

    assert(native_ui_mixer_fader_pct_at(WIN_H, track_top()) == 100);
    assert(native_ui_mixer_fader_pct_at(WIN_H, track_top() + UI_MIXER_FADER_H) == 0);
    assert(ui_mixer_master_pct_clamped(-5) == 0);
    assert(ui_mixer_master_pct_clamped(140) == 100);
}

int main(void) {
    test_panel_bounds_and_corners();
    test_hit_zones();
    test_fader_values();
    printf("ui-mixer-geometry: OK\n");
    return 0;
}
