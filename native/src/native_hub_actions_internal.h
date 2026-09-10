#ifndef LGNOME_NATIVE_HUB_ACTIONS_INTERNAL_H
#define LGNOME_NATIVE_HUB_ACTIONS_INTERNAL_H

#ifdef LGNOME_TARGET_WEBOS

#include <stdbool.h>

#include "native_hub_actions.h"

typedef struct NativeHubActionsContext {
    App *app;
    NativePreconnectUi *ui;
    NativeSettings *settings;
} NativeHubActionsContext;

/* Each phase consumes only its own one-shot requests. The public dispatcher calls
 * these in UI priority order; true means at least one request was consumed. */
bool native_hub_actions_drain_navigation(NativeHubActionsContext *context);
bool native_hub_actions_drain_profiles(NativeHubActionsContext *context);
bool native_hub_actions_drain_connect(NativeHubActionsContext *context);

#endif
#endif
