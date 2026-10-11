#ifndef LGNOME_NATIVE_CAPTURE_GLUE_H
#define LGNOME_NATIVE_CAPTURE_GLUE_H

#include <stdbool.h>

/* Applies capture settings and permissions; format changes reconnect camera
 * sessions. Interactive builds only (plus host regression tests). */

typedef struct App App;
typedef struct NativeCaptureRedirectConfig NativeCaptureRedirectConfig;
typedef struct NativeSettings NativeSettings;

void native_reconfigure_capture_redirect(App *app);
void native_capture_record_applied(App *app, const NativeCaptureRedirectConfig *config);
/* Active-slot handoffs are two-phase: synchronously close every outgoing payload
 * gate before publishing active_index, then follow the newly published owner. */
void native_capture_pause_for_active_slot_change(App *app);
void native_capture_follow_active_slot(App *app);
bool native_capture_mode_save_is_connecting(const App *app, const NativeSettings *candidate);

#endif
