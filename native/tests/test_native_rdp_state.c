#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "native_app.h"
#include "native_rdp_state.h"

static const RdpState k_all_states[] = {
    RDP_STATE_IDLE,     RDP_STATE_CONNECTING,    RDP_STATE_TLS,           RDP_STATE_CREDSSP,
    RDP_STATE_ACTIVE,   RDP_STATE_NO_AVC420,     RDP_STATE_DECODER_ERROR, RDP_STATE_NETWORK_ERROR,
    RDP_STATE_PROTOCOL_ERROR, RDP_STATE_STOPPED,
};

static App *app_create(bool interactive_ui) {
    App *app = (App *)calloc(1, sizeof(*app));
    assert(app);
    app->interactive_ui = interactive_ui;
    atomic_store(&app->running, true);
    atomic_store(&app->exit_code, 0);
    atomic_store(&app->active_index, 0);
    assert(pthread_mutex_init(&app->redaction_lock, NULL) == 0);
    for (int i = 0; i < NATIVE_SETTINGS_MAX_SESSIONS; i++) {
        app->sessions[i].app = app;
        app->sessions[i].index = i;
    }
    return app;
}

static void app_destroy(App *app) {
    (void)pthread_mutex_destroy(&app->redaction_lock);
    free(app);
}

static void test_state_names(void) {
    assert(strcmp(rdp_state_name(RDP_STATE_IDLE), "Idle") == 0);
    assert(strcmp(rdp_state_name(RDP_STATE_CONNECTING), "Connecting") == 0);
    assert(strcmp(rdp_state_name(RDP_STATE_TLS), "Tls") == 0);
    assert(strcmp(rdp_state_name(RDP_STATE_CREDSSP), "Credssp") == 0);
    assert(strcmp(rdp_state_name(RDP_STATE_ACTIVE), "Active") == 0);
    assert(strcmp(rdp_state_name(RDP_STATE_NO_AVC420), "NoAvc420") == 0);
    assert(strcmp(rdp_state_name(RDP_STATE_DECODER_ERROR), "DecoderError") == 0);
    assert(strcmp(rdp_state_name(RDP_STATE_NETWORK_ERROR), "NetworkError") == 0);
    assert(strcmp(rdp_state_name(RDP_STATE_PROTOCOL_ERROR), "ProtocolError") == 0);
    assert(strcmp(rdp_state_name(RDP_STATE_STOPPED), "Stopped") == 0);
    /* The Rust side owns these values; an unmapped one must stay printable. */
    assert(strcmp(rdp_state_name((RdpState)99), "Unknown") == 0);

    for (size_t i = 0u; i < sizeof(k_all_states) / sizeof(k_all_states[0]); i++) {
        assert(strcmp(rdp_state_name(k_all_states[i]), "Unknown") != 0);
    }
}

static void test_state_classification(void) {
    assert(rdp_state_is_terminal_error(RDP_STATE_NO_AVC420));
    assert(rdp_state_is_terminal_error(RDP_STATE_DECODER_ERROR));
    assert(rdp_state_is_terminal_error(RDP_STATE_NETWORK_ERROR));
    assert(rdp_state_is_terminal_error(RDP_STATE_PROTOCOL_ERROR));
    assert(!rdp_state_is_terminal_error(RDP_STATE_IDLE));
    assert(!rdp_state_is_terminal_error(RDP_STATE_CONNECTING));
    assert(!rdp_state_is_terminal_error(RDP_STATE_TLS));
    assert(!rdp_state_is_terminal_error(RDP_STATE_CREDSSP));
    assert(!rdp_state_is_terminal_error(RDP_STATE_ACTIVE));
    /* Stopped is an orderly end, not an error. */
    assert(!rdp_state_is_terminal_error(RDP_STATE_STOPPED));

    /* Only the two native-pipeline failures survive a reconnect attempt. */
    assert(rdp_state_is_native_runtime_failure(RDP_STATE_NO_AVC420));
    assert(rdp_state_is_native_runtime_failure(RDP_STATE_DECODER_ERROR));
    assert(!rdp_state_is_native_runtime_failure(RDP_STATE_NETWORK_ERROR));
    assert(!rdp_state_is_native_runtime_failure(RDP_STATE_PROTOCOL_ERROR));

    assert(rdp_state_exit_code(RDP_STATE_NO_AVC420) == 10);
    assert(rdp_state_exit_code(RDP_STATE_DECODER_ERROR) == 11);
    assert(rdp_state_exit_code(RDP_STATE_NETWORK_ERROR) == 12);
    assert(rdp_state_exit_code(RDP_STATE_PROTOCOL_ERROR) == 13);
    assert(rdp_state_exit_code(RDP_STATE_STOPPED) == 0);
    assert(rdp_state_exit_code((RdpState)99) == 0);

    for (size_t i = 0u; i < sizeof(k_all_states) / sizeof(k_all_states[0]); i++) {
        RdpState state = k_all_states[i];
        /* The three predicates must stay consistent: a non-zero exit code is
         * exactly the terminal-error set, and runtime failures are a subset. */
        assert(rdp_state_is_terminal_error(state) == (rdp_state_exit_code(state) != 0));
        if (rdp_state_is_native_runtime_failure(state)) {
            assert(rdp_state_is_terminal_error(state));
        }
    }
}

static void test_active_native_runtime_failure_exits(void) {
    App *app = app_create(true);
    atomic_store(&app->active_index, 1);
    NativeSessionSlot *slot = &app->sessions[1];

    /* Even an interactive build exits on these: reconnecting cannot fix a
     * decoder that will not decode. */
    native_slot_report_terminal(slot, RDP_STATE_NO_AVC420, 10);
    assert(atomic_load(&slot->terminal_state) == (int)RDP_STATE_NO_AVC420);
    assert(!atomic_load(&app->running));
    assert(atomic_load(&app->exit_code) == 10);
    assert(!atomic_load(&slot->session_failed));
    app_destroy(app);

    /* exit_code 0 means "do not overwrite what is already there". */
    app = app_create(false);
    atomic_store(&app->exit_code, 5);
    native_slot_report_terminal(&app->sessions[0], RDP_STATE_DECODER_ERROR, 0);
    assert(!atomic_load(&app->running));
    assert(atomic_load(&app->exit_code) == 5);
    app_destroy(app);
}

static void test_background_slot_never_exits(void) {
    App *app = app_create(false);
    atomic_store(&app->active_index, 0);
    NativeSessionSlot *background = &app->sessions[2];

    /* A background failure is flagged for the SDL thread and nothing else — not
     * even the fail-fast states may take the app down from a background slot. */
    native_slot_report_terminal(background, RDP_STATE_NO_AVC420, 10);
    assert(atomic_load(&background->session_failed));
    assert(atomic_load(&app->running));
    assert(atomic_load(&app->exit_code) == 0);
    assert(atomic_load(&background->terminal_state) == (int)RDP_STATE_NO_AVC420);

    native_slot_report_terminal(&app->sessions[3], RDP_STATE_NETWORK_ERROR, 12);
    assert(atomic_load(&app->sessions[3].session_failed));
    assert(atomic_load(&app->running));
    assert(atomic_load(&app->exit_code) == 0);
    app_destroy(app);
}

static void test_active_slot_follows_ui_mode(void) {
    /* Interactive: the active slot returns to the pre-connect UI. */
    App *app = app_create(true);
    NativeSessionSlot *slot = &app->sessions[0];
    native_slot_report_terminal(slot, RDP_STATE_NETWORK_ERROR, 12);
    assert(atomic_load(&slot->session_failed));
    assert(atomic_load(&app->running));
    assert(atomic_load(&app->exit_code) == 0);
    app_destroy(app);

    /* Headless: the same failure ends the process with the state's exit code. */
    app = app_create(false);
    slot = &app->sessions[0];
    native_slot_report_terminal(slot, RDP_STATE_NETWORK_ERROR, rdp_state_exit_code(RDP_STATE_NETWORK_ERROR));
    assert(!atomic_load(&slot->session_failed));
    assert(!atomic_load(&app->running));
    assert(atomic_load(&app->exit_code) == 12);
    app_destroy(app);

    /* An orderly stop still ends a headless run, without inventing a code. */
    app = app_create(false);
    native_slot_report_terminal(&app->sessions[0], RDP_STATE_STOPPED, 0);
    assert(!atomic_load(&app->running));
    assert(atomic_load(&app->exit_code) == 0);
    app_destroy(app);

    native_slot_report_terminal(NULL, RDP_STATE_NETWORK_ERROR, 12);
}

static void test_redaction(void) {
    App *app = app_create(true);
    const char *line = "connect failed for user@host";

    assert(strcmp(redact_if_sensitive(app, NULL), "") == 0);
    assert(redact_if_sensitive(NULL, line) == line);
    assert(redact_if_sensitive(app, line) == line);

    /* A worker on slot 0 must not leak ANOTHER slot's password: every stored
     * config is scanned, connected or not. */
    (void)snprintf(app->sessions[3].config.password, sizeof(app->sessions[3].config.password), "%s", "hunter2!");
    const char *leaky = "rdp error: authentication failed with hunter2! at host";
    const char *redacted = redact_if_sensitive(app, leaky);
    assert(redacted != leaky);
    assert(strstr(redacted, "hunter2!") == NULL);
    assert(redact_if_sensitive(app, line) == line);

    /* Under four characters the scan would match far too much ordinary log text,
     * so short passwords are deliberately not redacted. */
    App *shortpw = app_create(true);
    (void)snprintf(shortpw->sessions[0].config.password, sizeof(shortpw->sessions[0].config.password), "%s", "abc");
    const char *contains_short = "abc appears in this line";
    assert(redact_if_sensitive(shortpw, contains_short) == contains_short);
    app_destroy(shortpw);

    /* An empty password must never turn every line into a match. */
    App *empty = app_create(true);
    assert(empty->sessions[0].config.password[0] == '\0');
    assert(redact_if_sensitive(empty, leaky) == leaky);
    app_destroy(empty);

    app_destroy(app);
}

int main(void) {
    test_state_names();
    test_state_classification();
    test_active_native_runtime_failure_exits();
    test_background_slot_never_exits();
    test_active_slot_follows_ui_mode();
    test_redaction();
    return 0;
}
