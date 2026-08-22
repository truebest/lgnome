#ifndef GNOMECAST_NATIVE_MIXER_OVERLAY_H
#define GNOMECAST_NATIVE_MIXER_OVERLAY_H

/* Volume-mixer overlay state machine (see native_mixer_overlay.c). SDL only. */
#ifdef HELLOLG_TARGET_WEBOS

#include <SDL.h>
#include <stdbool.h>
#include <stdint.h>

typedef struct App App;

void native_mixer_overlay_touch(App *app);
void native_mixer_overlay_show(App *app);
void native_mixer_overlay_hide(App *app);
void native_mixer_overlay_force_hide(App *app);
void native_mixer_overlay_select(App *app, int slot);
void native_mixer_overlay_set_master_pct(App *app, int pct);
void native_mixer_overlay_set_db(App *app, int slot, int gain_db);
void native_mixer_overlay_toggle_duck(App *app, int slot);
void native_mixer_overlay_toggle_mute(App *app, int slot);
void native_mixer_overlay_toggle_solo(App *app, int slot);
void native_mixer_overlay_activate(App *app);
void native_mixer_overlay_adjust(App *app, int delta_steps);
bool native_mixer_overlay_consumes_evdev_key(uint16_t code);
void native_mixer_overlay_evdev_key(App *app, uint16_t code, bool down);
void native_mixer_overlay_sdl_key(App *app, const SDL_KeyboardEvent *event);
void native_system_volume_rebaseline(App *app);
void native_system_volume_tick(App *app);

#endif
#endif
