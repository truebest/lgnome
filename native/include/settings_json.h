#ifndef LGNOME_SETTINGS_JSON_H
#define LGNOME_SETTINGS_JSON_H

#include <stdio.h>

#include "native_settings.h"

/* Writes session-array JSON; also reads legacy flat settings into the green slot. */

/* True when the JSON contains any recognized settings key (legacy flat or "sessions"). */
bool native_settings_json_has_rdp_key(const char *json);

/* Applies a JSON document over *settings (unknown keys ignored, absent keys keep their
 * current values). Auto-detects the "sessions" array format vs the legacy flat object; flat
 * documents apply to the green slot. Returns false (settings untouched) on malformed
 * values; `source` is used for error logging. */
bool native_settings_apply_json(NativeSettings *settings, const char *json, const char *source);

/* Serializes the current session-array JSON to an open stream. Returns false on write error. */
bool native_settings_write_json(const NativeSettings *settings, FILE *file);

#endif
