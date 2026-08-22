#ifndef GNOMECAST_NATIVE_SWITCH_H
#define GNOMECAST_NATIVE_SWITCH_H

/* Slot-switching state machine (see native_switch*.c). SDL builds only. */
#ifdef HELLOLG_TARGET_WEBOS

#include <stdint.h>

typedef struct App App;
typedef struct NativeSessionSlot NativeSessionSlot;

void native_background_slot(App *app, int index);
void native_complete_session_switch(App *app, int target);
void native_show_hub(App *app);
void native_show_slot_configurator(App *app, int target);
void native_request_session_switch(App *app, int target);
void native_ring_switch(App *app, int direction);
void native_switch_tick(App *app);
/* Keyframe watchdog, in two halves around the resume/refresh requests:
 * baseline BEFORE them, start the window AFTER they are on the wire. */
void native_switch_watchdog_baseline(App *app, NativeSessionSlot *active);
void native_switch_watchdog_start(App *app, uint32_t timeout_ms);

#endif
#endif
