#ifndef LGNOME_NATIVE_RDP_CALLBACKS_H
#define LGNOME_NATIVE_RDP_CALLBACKS_H

#include "rdp_ffi.h"

/* Worker-callback module (see native_rdp_callbacks.c). */

typedef struct NativeSessionSlot NativeSessionSlot;

/* The full callback table for one slot; ctx is the slot itself. */
RdpCallbacks native_rdp_callbacks(NativeSessionSlot *slot);

#endif
