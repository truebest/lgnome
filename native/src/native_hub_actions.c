/* Ordered one-shot HUB request dispatcher. Cohesive handlers own navigation,
 * profile mutations, and the connect transaction. Preconnect-UI builds only. */
#ifdef HELLOLG_TARGET_WEBOS

#include "native_hub_actions_internal.h"

#include "clog.h"

clog_define(g_native_log_hub_actions, cLogLevelInfo, cLogFlags_Default, "native", NULL);

/* Drain every request class even when an earlier class consumed work: LVGL may queue
 * multiple independent one-shots in one input batch. The order is intentional:
 * BACK/activate, capture/profile mutations, then connect. In particular, navigation
 * cancellation must run before the connect one-shot is taken. */
void native_hub_actions_tick(App *app, NativePreconnectUi *ui, NativeSettings *settings) {
    NativeHubActionsContext context = {
        .app = app,
        .ui = ui,
        .settings = settings,
    };
    bool handled = native_hub_actions_drain_navigation(&context);
    if (native_hub_actions_drain_profiles(&context)) {
        handled = true;
    }
    if (native_hub_actions_drain_connect(&context)) {
        handled = true;
    }
    if (handled) {
        clog(cLogLevelDebug, "processed pending HUB UI actions");
    }
}

#endif
