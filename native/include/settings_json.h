#ifndef GNOMECAST_SETTINGS_JSON_H
#define GNOMECAST_SETTINGS_JSON_H

#include <stdio.h>

#include "native_settings.h"

/* Hand-rolled settings JSON (de)serialization, split out of main.c so the
 * multi-session config logic is host-testable. Two persisted formats are understood:
 *
 *   current (written): { "sessions": [ { "slot": "green", "name": ..., "host": ..., "port": n,
 *                    "username": ..., "password": ..., "domain": ..., "fps": n },
 *                    { "slot": "yellow", ... } ],
 *                    "wheelStep": n, "wheelScrollDivisor": n, "audioCodec": "auto",
 *                    "cameraEnabled": bool, "cameraDeviceId": ...,
 *                    "audioInputEnabled": bool, "audioInputDeviceId": ... }
 *   legacy (read): the old flat single-session object (host/port/username/password/domain/
 *                  fps/wheelStep/...) — applied to the green slot.
 *
 * Launch parameters, CLI flags and config.local.json keep the legacy flat shape and target
 * the green slot through the same native_settings_apply_json entry point. */

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
