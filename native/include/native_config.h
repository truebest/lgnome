#ifndef LGNOME_NATIVE_CONFIG_H
#define LGNOME_NATIVE_CONFIG_H

#include <stdbool.h>
#include <stddef.h>

#include "native_settings.h"

/* Default remote desktop size; independent of the local SDL canvas. */
#define NATIVE_RDP_INITIAL_DESKTOP_WIDTH 3840u
#define NATIVE_RDP_INITIAL_DESKTOP_HEIGHT 2160u

#define NATIVE_CONFIG_PATH "native/config.local.json"
#define NATIVE_CONFIG_MAX_FILE (16u * 1024u)

/* Pure settings operations and validation: config diffs, derived capture
 * gates, and effective-config diagnostics. */

bool copy_config_string(char *dest, size_t cap, const char *value, const char *field);

/* Config-diff predicates the connect/save paths use to decide between a live
 * apply and a reconnect. */
bool native_session_endpoint_changed(const NativeSessionConfig *before, const NativeSessionConfig *after);
bool native_session_connection_config_changed(const NativeSessionConfig *before, const NativeSessionConfig *after);

void native_settings_recompute_capture_gates(NativeSettings *settings);
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
