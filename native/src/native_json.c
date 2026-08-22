#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "native_json.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *native_json_skip_ws(const char *p) {
    while (p && *p && isspace((unsigned char)*p)) {
        p++;
    }
    return p;
}

const char *native_json_find_value(const char *json, const char *key) {
    char pattern[64];
    int n = snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    if (n <= 0 || (size_t)n >= sizeof(pattern)) {
        return NULL;
    }

    const char *p = json;
    while ((p = strstr(p, pattern)) != NULL) {
        const char *after_key = native_json_skip_ws(p + (size_t)n);
        if (*after_key == ':') {
            return native_json_skip_ws(after_key + 1);
        }
        p += (size_t)n;
    }
    return NULL;
}

static int hex_value(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

int native_json_read_string(const char *json, const char *key, char *out, size_t cap) {
    const char *p = native_json_find_value(json, key);
    if (!p) {
        return 0;
    }
    if (*p != '"') {
        return -1;
    }
    p++;

    size_t written = 0;
    while (*p && *p != '"') {
        unsigned char ch = (unsigned char)*p++;
        if (ch < 0x20) {
            return -1;
        }
        if (ch == '\\') {
            ch = (unsigned char)*p++;
            switch (ch) {
            case '"':
            case '\\':
            case '/':
                break;
            case 'b':
                ch = '\b';
                break;
            case 'f':
                ch = '\f';
                break;
            case 'n':
                ch = '\n';
                break;
            case 'r':
                ch = '\r';
                break;
            case 't':
                ch = '\t';
                break;
            case 'u': {
                /* Read the four hex digits left to right, stopping at the first non-hex
                 * character. hex_value('\0') is negative, so a string ending inside the
                 * escape (e.g. "\u" or "\u0") short-circuits before p[1..3] are touched —
                 * argv-backed launch-parameter strings have no trailing slack bytes, so an
                 * unconditional p[1..3] read there is out of bounds. */
                int h0 = hex_value(p[0]);
                int h1 = h0 < 0 ? -1 : hex_value(p[1]);
                int h2 = h1 < 0 ? -1 : hex_value(p[2]);
                int h3 = h2 < 0 ? -1 : hex_value(p[3]);
                if (h0 != 0 || h1 != 0 || h2 < 0 || h3 < 0) {
                    return -1;
                }
                ch = (unsigned char)((h2 << 4) | h3);
                p += 4;
                break;
            }
            default:
                return -1;
            }
        }
        if (written + 1 >= cap) {
            return -1;
        }
        out[written++] = (char)ch;
    }

    if (*p != '"') {
        return -1;
    }
    out[written] = '\0';
    return 1;
}

int native_json_read_u16(const char *json, const char *key, uint16_t min_value, uint16_t max_value, uint16_t *out) {
    const char *p = native_json_find_value(json, key);
    if (!p) {
        return 0;
    }

    errno = 0;
    char *end = NULL;
    unsigned long value = strtoul(p, &end, 10);
    if (errno != 0 || end == p || value < (unsigned long)min_value || value > (unsigned long)max_value) {
        return -1;
    }
    *out = (uint16_t)value;
    return 1;
}

int native_json_read_i16(const char *json, const char *key, int16_t min_value,
                         int16_t max_value, int16_t *out) {
    const char *p = native_json_find_value(json, key);
    if (!p) {
        return 0;
    }

    errno = 0;
    char *end = NULL;
    long value = strtol(p, &end, 10);
    if (errno != 0 || end == p || value < (long)min_value ||
        value > (long)max_value) {
        return -1;
    }
    *out = (int16_t)value;
    return 1;
}

int native_json_read_bool(const char *json, const char *key, bool *out) {
    const char *p = native_json_find_value(json, key);
    if (!p) {
        return 0;
    }
    if (strncmp(p, "true", 4) == 0) {
        *out = true;
        return 1;
    }
    if (strncmp(p, "false", 5) == 0) {
        *out = false;
        return 1;
    }
    return -1;
}
