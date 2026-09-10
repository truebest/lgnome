#ifndef LGNOME_UI_SESSION_STATUS_H
#define LGNOME_UI_SESSION_STATUS_H

#include <stdbool.h>
#include <stdint.h>
#include "rdp_ffi.h"

typedef enum NativePreconnectSessionState {
    NATIVE_PRECONNECT_SESSION_NOT_SET_UP = 0,
    NATIVE_PRECONNECT_SESSION_OFFLINE,
    NATIVE_PRECONNECT_SESSION_CONNECTING,
    NATIVE_PRECONNECT_SESSION_CONNECTED,
    NATIVE_PRECONNECT_SESSION_ERROR,
    NATIVE_PRECONNECT_SESSION_DISCONNECTED,
    NATIVE_PRECONNECT_SESSION_INTERRUPTED,
    NATIVE_PRECONNECT_SESSION_RECONNECTING,
} NativePreconnectSessionState;

typedef struct NativeSessionStatus {
    const char *badge;
    const char *title;
    const char *detail;
    const char *action;
    uint32_t color;
    bool error;
} NativeSessionStatus;

static inline bool native_ui_session_connecting(NativePreconnectSessionState state) {
    return state == NATIVE_PRECONNECT_SESSION_CONNECTING || state == NATIVE_PRECONNECT_SESSION_RECONNECTING;
}

static inline bool native_ui_session_terminal(NativePreconnectSessionState state) {
    return state == NATIVE_PRECONNECT_SESSION_ERROR || state == NATIVE_PRECONNECT_SESSION_DISCONNECTED ||
           state == NATIVE_PRECONNECT_SESSION_INTERRUPTED;
}

NativeSessionStatus native_session_status(NativePreconnectSessionState state, RdpDisconnectReason reason);
NativePreconnectSessionState native_session_terminal_ui_state(RdpState state, RdpDisconnectReason reason);
RdpDisconnectReason native_session_terminal_reason(RdpState state, RdpDisconnectReason reason, bool was_active);

#endif
