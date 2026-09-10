#include "native_rdp_state.h"

#include <pthread.h>
#include <stdatomic.h>
#include <string.h>

#include "native_app.h"
#include "ui_session_status.h"

#include "clog.h"

clog_define(g_native_log_rdp_state, cLogLevelInfo, "native");

static void native_slot_report_terminal_reason(NativeSessionSlot *slot, RdpState state,
                                                RdpDisconnectReason reason, int exit_code);

void native_slot_record_state(NativeSessionSlot *slot, RdpState state, RdpDisconnectReason reason) {
    if (!slot || atomic_load(&slot->session_failed) || atomic_load(&slot->terminal_state) != RDP_STATE_IDLE) {
        return;
    }
    atomic_store(&slot->current_state, (int)state);
    if (state == RDP_STATE_RECONNECTING) {
        atomic_store(&slot->reconnecting, true);
    } else if (state == RDP_STATE_ACTIVE) {
        atomic_store(&slot->reconnecting, false);
        atomic_store(&slot->was_active, true);
    }
    if (rdp_state_is_terminal_error(state) || state == RDP_STATE_DISCONNECTED || state == RDP_STATE_STOPPED) {
        if (rdp_state_is_terminal_error(state)) {
            clog(cLogLevelError, "slot %d session failed: %s", slot->index, rdp_state_name(state));
        }
        native_slot_report_terminal_reason(slot, state, reason, rdp_state_exit_code(state));
    }
}

/* Marks a slot's session as terminated. The ACTIVE slot returns to the
 * pre-connect UI in interactive builds and exits the process otherwise
 * (always exits on native runtime failures). A BACKGROUND slot failure must
 * never take down the app: it is flagged and the SDL thread cleans it up. */
static void native_slot_report_terminal_reason(NativeSessionSlot *slot, RdpState state,
                                                RdpDisconnectReason reason, int exit_code) {
    if (!slot) {
        return;
    }
    int expected = RDP_STATE_IDLE;
    if (!atomic_compare_exchange_strong(&slot->terminal_state, &expected, (int)state)) {
        return;
    }
    reason = native_session_terminal_reason(state, reason, atomic_load(&slot->was_active));
    atomic_store(&slot->terminal_reason, (int)reason);
    atomic_store(&slot->reconnecting, false);
    App *app = slot->app;
    bool is_active = native_slot_is_active(slot);
    if (is_active && rdp_state_is_native_runtime_failure(state)) {
        /* Shared-decoder failures signal a platform problem; fail fast so logs
         * land before anything else goes wrong. */
        if (exit_code != 0) {
            atomic_store(&app->exit_code, exit_code);
        }
        atomic_store(&app->running, false);
        return;
    }
    if (!is_active || app->interactive_ui) {
        atomic_store(&slot->session_failed, true);
        return;
    }
    if (exit_code != 0) {
        atomic_store(&app->exit_code, exit_code);
    }
    atomic_store(&app->running, false);
}

void native_slot_report_terminal(NativeSessionSlot *slot, RdpState state, int exit_code) {
    native_slot_report_terminal_reason(slot, state, RDP_DISCONNECT_NONE, exit_code);
}

const char *rdp_state_name(RdpState state) {
    switch (state) {
    case RDP_STATE_IDLE:
        return "Idle";
    case RDP_STATE_CONNECTING:
        return "Connecting";
    case RDP_STATE_TLS:
        return "Tls";
    case RDP_STATE_CREDSSP:
        return "Credssp";
    case RDP_STATE_ACTIVE:
        return "Active";
    case RDP_STATE_NO_AVC420:
        return "NoAvc420";
    case RDP_STATE_DECODER_ERROR:
        return "DecoderError";
    case RDP_STATE_NETWORK_ERROR:
        return "NetworkError";
    case RDP_STATE_PROTOCOL_ERROR:
        return "ProtocolError";
    case RDP_STATE_STOPPED:
        return "Stopped";
    case RDP_STATE_DISCONNECTED:
        return "Disconnected";
    case RDP_STATE_RECONNECTING:
        return "Reconnecting";
    }
    return "Unknown";
}

RdpDisconnectReason native_session_terminal_reason(RdpState state, RdpDisconnectReason reason, bool was_active) {
    if (reason != RDP_DISCONNECT_NONE) {
        return reason;
    }
    switch (state) {
    case RDP_STATE_NETWORK_ERROR:
        return was_active ? RDP_DISCONNECT_CONNECTION_LOST : RDP_DISCONNECT_CONNECTION_FAILED;
    case RDP_STATE_PROTOCOL_ERROR:
        return RDP_DISCONNECT_PROTOCOL_ERROR;
    case RDP_STATE_NO_AVC420:
    case RDP_STATE_DECODER_ERROR:
        return RDP_DISCONNECT_GRAPHICS_ERROR;
    default:
        return RDP_DISCONNECT_PEER_DISCONNECTED;
    }
}

NativePreconnectSessionState native_session_terminal_ui_state(RdpState state, RdpDisconnectReason reason) {
    if (state == RDP_STATE_NETWORK_ERROR || reason == RDP_DISCONNECT_SESSION_REPLACED ||
        reason == RDP_DISCONNECT_IDLE_TIMEOUT || reason == RDP_DISCONNECT_SESSION_TIMEOUT) {
        return NATIVE_PRECONNECT_SESSION_INTERRUPTED;
    }
    return rdp_state_is_terminal_error(state) ? NATIVE_PRECONNECT_SESSION_ERROR : NATIVE_PRECONNECT_SESSION_DISCONNECTED;
}

NativeSessionStatus native_session_status(NativePreconnectSessionState state, RdpDisconnectReason reason) {
    NativeSessionStatus status = {"OFFLINE", "Offline", "", "Connect", 0x8a909b, false};
    switch (state) {
    case NATIVE_PRECONNECT_SESSION_NOT_SET_UP:
        return (NativeSessionStatus){"NOT SET UP", "Not set up", "", "Set up", 0x8a909b, false};
    case NATIVE_PRECONNECT_SESSION_CONNECTING:
        return (NativeSessionStatus){"CONNECTING", "Connecting...", "", "Connecting...", 0xe8c15a, false};
    case NATIVE_PRECONNECT_SESSION_RECONNECTING:
        return (NativeSessionStatus){"RETRYING", "Reconnecting...", "", "Reconnecting...", 0xe8c15a, false};
    case NATIVE_PRECONNECT_SESSION_CONNECTED:
        return (NativeSessionStatus){"CONNECTED", "Connected", "", "Resume", 0x43d47e, false};
    case NATIVE_PRECONNECT_SESSION_ERROR:
        status = (NativeSessionStatus){"ERROR", "Connection error", "Connection failed.", "Retry", 0xe35d55, true};
        break;
    case NATIVE_PRECONNECT_SESSION_INTERRUPTED:
        status = (NativeSessionStatus){"LOST", "Connection lost", "Connection lost; reason unknown.", "Retry", 0xe8c15a, false};
        break;
    case NATIVE_PRECONNECT_SESSION_DISCONNECTED:
        status = (NativeSessionStatus){"CLOSED", "Disconnected", "The session was closed.", "Connect", 0x8a909b, false};
        break;
    default:
        return status;
    }
    switch (reason) {
    case RDP_DISCONNECT_SERVER_SHUTDOWN:
        status.badge = "SHUTDOWN";
        status.title = "Server shutdown";
        status.detail = "Last disconnect: the server reported it was shutting down.";
        break;
    case RDP_DISCONNECT_SERVER_REBOOT:
        status.badge = "REBOOT";
        status.title = "Server restart";
        status.detail = "Last disconnect: the server reported it was restarting.";
        break;
    case RDP_DISCONNECT_USER_LOGOFF:
    case RDP_DISCONNECT_ADMIN_LOGOFF:
        status.badge = "SIGNED OUT";
        status.title = "Signed out";
        status.detail = reason == RDP_DISCONNECT_ADMIN_LOGOFF ? "An administrator signed you out." : "Signed out on the server.";
        break;
    case RDP_DISCONNECT_USER_DISCONNECT:
        status.detail = "The session was disconnected by a user action.";
        break;
    case RDP_DISCONNECT_ADMIN_DISCONNECT:
        status.detail = "An administrator disconnected the session.";
        break;
    case RDP_DISCONNECT_SESSION_REPLACED:
        status.badge = "REPLACED";
        status.title = "Session replaced";
        status.action = "Connect";
        status.detail = "Another connection replaced this session.";
        break;
    case RDP_DISCONNECT_IDLE_TIMEOUT:
    case RDP_DISCONNECT_SESSION_TIMEOUT:
        status.badge = "TIMED OUT";
        status.title = "Session limit reached";
        status.action = "Connect";
        status.detail = reason == RDP_DISCONNECT_IDLE_TIMEOUT ? "The server's idle session limit was reached." : "The server's session time limit was reached.";
        break;
    case RDP_DISCONNECT_CONNECTION_FAILED:
        status.badge = "FAILED";
        status.title = "Connection failed";
        status.detail = "Could not connect to the server.";
        break;
    case RDP_DISCONNECT_PROTOCOL_ERROR:
        status.title = "Protocol error";
        status.detail = "The RDP session ended because of a protocol error.";
        break;
    case RDP_DISCONNECT_GRAPHICS_ERROR:
        status.title = "Graphics error";
        status.detail = "The graphics pipeline could not display the session.";
        break;
    case RDP_DISCONNECT_ACCESS_DENIED:
        status.title = "Connection denied";
        status.detail = "The server denied access. Check your credentials and permissions.";
        break;
    case RDP_DISCONNECT_SERVER_ERROR:
        status.title = "Server error";
        status.detail = "The server reported a session failure.";
        break;
    case RDP_DISCONNECT_LICENSE_ERROR:
        status.title = "License error";
        status.detail = "The server reported a Remote Desktop licensing error.";
        break;
    case RDP_DISCONNECT_BROKER_ERROR:
        status.title = "Connection broker error";
        status.detail = "The server could not route the connection to a desktop.";
        break;
    default:
        break;
    }
    return status;
}

bool rdp_state_is_terminal_error(RdpState state) {
    return state == RDP_STATE_NO_AVC420 || state == RDP_STATE_DECODER_ERROR || state == RDP_STATE_NETWORK_ERROR ||
           state == RDP_STATE_PROTOCOL_ERROR;
}

bool rdp_state_is_native_runtime_failure(RdpState state) {
    return state == RDP_STATE_NO_AVC420 || state == RDP_STATE_DECODER_ERROR;
}

int rdp_state_exit_code(RdpState state) {
    switch (state) {
    case RDP_STATE_NO_AVC420:
        return 10;
    case RDP_STATE_DECODER_ERROR:
        return 11;
    case RDP_STATE_NETWORK_ERROR:
        return 12;
    case RDP_STATE_PROTOCOL_ERROR:
        return 13;
    default:
        return 0;
    }
}

const char *redact_if_sensitive(App *app, const char *line) {
    if (!line) {
        return "";
    }
    if (!app) {
        return line;
    }
    /* Runs on rdp-worker threads while the SDL thread may be rewriting ANOTHER slot's
     * config on a (re)Connect, so the scan takes redaction_lock (as does that write).
     * Every slot with a stored password is scanned, connected or not: checking slot->rdp
     * here would just race on that pointer, and redacting a disconnected slot's password
     * is at worst extra-safe. */
    const char *result = line;
    pthread_mutex_lock(&app->redaction_lock);
    for (int i = 0; i < NATIVE_SETTINGS_MAX_SESSIONS; i++) {
        const char *password = app->sessions[i].config.password;
        if (strlen(password) >= 4 && strstr(line, password)) {
            result = "[redacted sensitive detail]";
            clog(cLogLevelDebug, "redacted a sensitive detail from a worker log line");
            break;
        }
    }
    pthread_mutex_unlock(&app->redaction_lock);
    return result;
}
