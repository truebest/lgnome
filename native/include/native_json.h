#ifndef LGNOME_NATIVE_JSON_H
#define LGNOME_NATIVE_JSON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Minimal field scanner for the flat, trusted-ish JSON this app actually
 * meets: the persisted settings file, webOS launch parameters, and Luna
 * service replies. It scans for `"key" :` and reads one scalar; it does not
 * validate documents, handle nesting, or allocate. Deliberately log-free:
 * callers own the tri-state result and the error report.
 *
 * native_json_read_* return 1 = read, 0 = key absent, -1 = present but
 * invalid (out of range, wrong type, bad escape). */

const char *native_json_skip_ws(const char *p);

/* Pointer to the first non-space byte of the value for `key`, or NULL. */
const char *native_json_find_value(const char *json, const char *key);

int native_json_read_string(const char *json, const char *key, char *out, size_t cap);
int native_json_read_u16(const char *json, const char *key, uint16_t min_value, uint16_t max_value, uint16_t *out);
int native_json_read_i16(const char *json, const char *key, int16_t min_value, int16_t max_value, int16_t *out);
int native_json_read_bool(const char *json, const char *key, bool *out);

#endif
