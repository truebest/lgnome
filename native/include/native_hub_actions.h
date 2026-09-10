#ifndef LGNOME_NATIVE_HUB_ACTIONS_H
#define LGNOME_NATIVE_HUB_ACTIONS_H

/* HUB one-shot request handling (see native_hub_actions*.c). Preconnect-UI
 * builds only. */
#ifdef LGNOME_TARGET_WEBOS

typedef struct App App;
typedef struct NativePreconnectUi NativePreconnectUi;
typedef struct NativeSettings NativeSettings;

void native_hub_actions_tick(App *app, NativePreconnectUi *ui, NativeSettings *settings);

#endif
#endif
