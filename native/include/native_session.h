#ifndef GNOMECAST_NATIVE_SESSION_H
#define GNOMECAST_NATIVE_SESSION_H

#include <stdbool.h>

/* Session-slot lifecycle (see native_session.c). SDL/main thread only. */

typedef struct App App;

void native_duck_retarget(App *app);
bool native_any_slot_connected(const App *app);
/* Tears down the shared presentation pipeline; sessions are unaffected. */
void native_stop_media(App *app);
void native_publish_effective_solo_mask(App *app);
void native_stop_slot(App *app, int index);
void native_stop_all_sessions(App *app);
/* (Re)connects one slot from its stored config; arm_snapshot pre-arms the
 * IDR-snapshot cache for background fills. */
bool native_slot_connect(App *app, int index, bool arm_snapshot);

#endif
