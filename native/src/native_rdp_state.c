#include "native_rdp_state.h"

#include <pthread.h>
#include <stdatomic.h>
#include <string.h>

#include "native_app.h"

#include "clog.h"

clog_define(g_native_log_rdp_state, cLogLevelInfo, cLogFlags_Default, "native", NULL);

/* Marks a slot's session as terminated. The ACTIVE slot returns to the
 * pre-connect UI in interactive builds and exits the process otherwise
 * (always exits on native runtime failures). A BACKGROUND slot failure must
 * never take down the app: it is flagged and the SDL thread cleans it up. */
void native_slot_report_terminal(NativeSessionSlot *slot, RdpState state, int exit_code) {
    if (!slot) {
        return;
    }
    App *app = slot->app;
    atomic_store(&slot->terminal_state, (int)state);
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
    }
    return "Unknown";
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
