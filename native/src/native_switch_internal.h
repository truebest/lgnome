#ifndef LGNOME_NATIVE_SWITCH_INTERNAL_H
#define LGNOME_NATIVE_SWITCH_INTERNAL_H

#ifdef LGNOME_TARGET_WEBOS

#include <stdbool.h>

#include "native_switch.h"

#ifndef NATIVE_SWITCH_TEST_NO_RECONNECT
#define NATIVE_SWITCH_TEST_NO_RECONNECT 0
#endif

/* Private phases shared by the switch coordinator, handoff, and navigation modules. */
bool native_snapshot_replay(App *app, int target);
void native_prepare_pending_switch(App *app, int target);

#endif
#endif
