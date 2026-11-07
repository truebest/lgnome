#include <assert.h>
#include <string.h>

#include "native_app.h"
#include "native_config_storage.h"
#include "native_hub_actions_internal.h"
#include "ui_preconnect.h"

static const char pin[] = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
static bool answer_ready, accept_answer, remember_answer, fail_save;
static unsigned saves, confirmations, shown, closed, errors;
static bool accepted;
static NativeSettings saved;

bool native_config_save_persisted(const NativeSettings *settings) {
    saves++;
    saved = *settings;
    return !fail_save;
}
void native_show_hub(App *app) { app->streaming_visible = false; }
void native_preconnect_ui_set_visible(NativePreconnectUi *ui, bool visible) { (void)ui; assert(visible); }
void native_preconnect_ui_show_certificate(NativePreconnectUi *ui, const NativeSessionConfig *session,
                                          const char *sha256, const char *reason) {
    (void)ui; (void)session; (void)reason;
    assert(strcmp(sha256, pin) == 0);
    shown++;
}
void native_preconnect_ui_close_certificate(NativePreconnectUi *ui) { (void)ui; closed++; }
bool native_preconnect_ui_take_certificate(NativePreconnectUi *ui, bool *accept, bool *remember) {
    (void)ui;
    if (!answer_ready) return false;
    answer_ready = false;
    *accept = accept_answer;
    *remember = remember_answer;
    return true;
}
void native_preconnect_ui_certificate_error(NativePreconnectUi *ui, const char *message) {
    (void)ui; (void)message; errors++;
}
void native_preconnect_ui_set_certificate_trust(NativePreconnectUi *ui, int slot, const NativeCertificateTrust *trust) {
    (void)ui; (void)slot; assert(strcmp(trust->sha256, pin) == 0);
}
bool rdp_confirm_certificate(RdpSession *session, const char *sha256, bool accept) {
    NativeSessionSlot *slot = (NativeSessionSlot *)session;
    assert(!slot->certificate_pending && !slot->app->certificate_prompt_active);
    assert(strcmp(sha256, pin) == 0);
    confirmations++;
    accepted = accept;
    return true;
}
static void request(NativeSessionSlot *slot) {
    slot->certificate_pending = true;
    strcpy(slot->certificate_sha256, pin);
    strcpy(slot->certificate_reason, "Test certificate");
}
static void answer(bool accept, bool remember) {
    accept_answer = accept;
    remember_answer = remember;
    answer_ready = true;
}
int main(void) {
    App app = {0};
    NativeSettings settings;
    native_settings_defaults(&settings);
    app.settings = &settings;
    assert(pthread_mutex_init(&app.video_lock, NULL) == 0);
    NativeSessionSlot *slot = &app.sessions[1];
    slot->app = &app;
    slot->index = 1;
    slot->config = settings.sessions[1];
    slot->rdp = (RdpSession *)slot;
    NativeHubActionsContext context = {&app, NULL, &settings};

    request(slot);
    assert(native_hub_actions_certificate(&context) && shown == 1 && confirmations == 0);
    answer(true, false);
    assert(native_hub_actions_certificate(&context));
    assert(accepted && confirmations == 1 && saves == 0 && closed == 1);
    assert(!settings.sessions[1].certificate_trust.sha256[0]);
    assert(strcmp(native_session_trusted_certificate(&slot->config), pin) == 0);

    request(slot);
    assert(native_hub_actions_certificate(&context));
    answer(true, true);
    fail_save = true;
    assert(native_hub_actions_certificate(&context));
    assert(errors == 1 && confirmations == 1 && slot->certificate_pending);
    assert(!settings.sessions[1].certificate_trust.sha256[0]);
    fail_save = false;
    answer(true, true);
    assert(native_hub_actions_certificate(&context));
    assert(confirmations == 2 && saves == 2);
    assert(strcmp(native_session_trusted_certificate(&settings.sessions[1]), pin) == 0);
    assert(strcmp(native_session_trusted_certificate(&saved.sessions[1]), pin) == 0);

    request(slot);
    assert(native_hub_actions_certificate(&context));
    answer(false, true);
    assert(native_hub_actions_certificate(&context));
    assert(!accepted && confirmations == 3 && saves == 2);

    request(slot);
    assert(native_hub_actions_certificate(&context));
    atomic_fetch_add(&slot->connect_epoch, 1);
    slot->certificate_pending = false;
    answer(true, true);
    assert(!native_hub_actions_certificate(&context));
    assert(confirmations == 3 && saves == 2 && !app.certificate_prompt_active);
    pthread_mutex_destroy(&app.video_lock);
    return 0;
}
