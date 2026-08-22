#ifndef GNOMECAST_NATIVE_CONFIG_H
#define GNOMECAST_NATIVE_CONFIG_H

#include <stdbool.h>
#include <stddef.h>

#include "native_settings.h"

/* Desktop size requested at connect. This is NOT the local SDL surface: H.264
 * never touches SDL, so nothing about the UI canvas constrains it. A mirroring server
 * overrides the request with its own monitor size anyway, so asking for the panel's 4K
 * only decides whether that correction is a resize or a no-op -- and the TV panel and
 * the servers this targets are both 3840x2160. A headless server, which does honour the
 * request, would get a 4K desktop; drop this to 1920x1080 if that is ever unwanted. */
#define NATIVE_RDP_INITIAL_DESKTOP_WIDTH 3840u
#define NATIVE_RDP_INITIAL_DESKTOP_HEIGHT 2160u

#define NATIVE_CONFIG_PATH "native/config.local.json"
#define NATIVE_CONFIG_MAX_FILE (16u * 1024u)

/* Pure settings operations and validation: config diffs, derived capture
 * gates, the initial desktop-size hint, and effective-config diagnostics. */

bool copy_config_string(char *dest, size_t cap, const char *value, const char *field);

/* Config-diff predicates the connect/save paths use to decide between a live
 * apply and a reconnect. */
bool native_session_endpoint_changed(const NativeSessionConfig *before, const NativeSessionConfig *after);
bool native_session_connection_config_changed(const NativeSessionConfig *before, const NativeSessionConfig *after);

void native_settings_recompute_capture_gates(NativeSettings *settings);
void native_config_apply_initial_desktop_hint(NativeSettings *settings);
/* Full connect-readiness check for one slot; message (cap bytes) gets the
 * user-facing reason on failure. */
bool native_session_config_validate_connect(const NativeSessionConfig *session, int slot, char *message,
                                            size_t cap);
bool native_config_validate(const NativeSettings *settings);
/* Relaxed variant for preconnect-UI builds: incomplete profiles are fine, the
 * form collects the rest. */
bool native_config_validate_runtime(const NativeSettings *settings);
void native_config_log_effective(const NativeSettings *settings);

#endif
