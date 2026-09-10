#ifndef LGNOME_NATIVE_CAPTURE_GLUE_H
#define LGNOME_NATIVE_CAPTURE_GLUE_H

/* Applies the current capture settings: restarts the shared workers only on a
 * device/format change, then re-publishes each slot's per-profile permission and
 * foreground capture owner. Interactive builds only (plus its host regression test). */

typedef struct App App;
typedef struct NativeCaptureRedirectConfig NativeCaptureRedirectConfig;

void native_reconfigure_capture_redirect(App *app);
void native_capture_record_applied(App *app, const NativeCaptureRedirectConfig *config);
/* Active-slot handoffs are two-phase: synchronously close every outgoing payload
 * gate before publishing active_index, then follow the newly published owner. */
void native_capture_pause_for_active_slot_change(App *app);
void native_capture_follow_active_slot(App *app);

#endif
