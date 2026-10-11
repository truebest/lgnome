#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "native_json.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

const char *native_json_skip_ws(const char *p) {
    while (p && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) {
        p++;
    }
    return p;
}

static int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool read_hex4(const char **cursor, uint32_t *value) {
    *value = 0;
    for (unsigned i = 0; i < 4; i++) {
        int digit = hex_value(**cursor);
        if (digit < 0) return false;
        *value = (*value << 4) | (unsigned)digit;
        (*cursor)++;
    }
    return true;
}

static const char *read_string(const char *p, char *out, size_t cap) {
    if (*p != '"') return NULL;
    p++;
    size_t written = 0;
    while (*p && *p != '"') {
        unsigned char bytes[4];
        size_t count = 1;
        unsigned char ch = (unsigned char)*p++;
        if (ch < 0x20) return NULL;
        bytes[0] = ch;
        if (ch == '\\') {
            ch = (unsigned char)*p++;
            switch (ch) {
            case '"': case '\\': case '/': bytes[0] = ch; break;
            case 'b': bytes[0] = '\b'; break;
            case 'f': bytes[0] = '\f'; break;
            case 'n': bytes[0] = '\n'; break;
            case 'r': bytes[0] = '\r'; break;
            case 't': bytes[0] = '\t'; break;
            case 'u': {
                uint32_t codepoint;
                if (!read_hex4(&p, &codepoint) || codepoint == 0) return NULL;
                if (codepoint >= 0xd800 && codepoint <= 0xdbff) {
                    uint32_t low;
                    if (*p != '\\' || p[1] != 'u') return NULL;
                    p += 2;
                    if (!read_hex4(&p, &low) || low < 0xdc00 || low > 0xdfff) return NULL;
                    codepoint = 0x10000 + ((codepoint - 0xd800) << 10) + low - 0xdc00;
                } else if (codepoint >= 0xdc00 && codepoint <= 0xdfff) {
                    return NULL;
                }
                if (codepoint < 0x80) {
                    bytes[0] = (unsigned char)codepoint;
                } else if (codepoint < 0x800) {
                    count = 2;
                    bytes[0] = (unsigned char)(0xc0 | (codepoint >> 6));
                    bytes[1] = (unsigned char)(0x80 | (codepoint & 0x3f));
                } else if (codepoint < 0x10000) {
                    count = 3;
                    bytes[0] = (unsigned char)(0xe0 | (codepoint >> 12));
                    bytes[1] = (unsigned char)(0x80 | ((codepoint >> 6) & 0x3f));
                    bytes[2] = (unsigned char)(0x80 | (codepoint & 0x3f));
                } else {
                    count = 4;
                    bytes[0] = (unsigned char)(0xf0 | (codepoint >> 18));
                    bytes[1] = (unsigned char)(0x80 | ((codepoint >> 12) & 0x3f));
                    bytes[2] = (unsigned char)(0x80 | ((codepoint >> 6) & 0x3f));
                    bytes[3] = (unsigned char)(0x80 | (codepoint & 0x3f));
                }
                break;
            }
            default: return NULL;
            }
        }
        if (out) {
            if (written >= cap || count >= cap - written) return NULL;
            memcpy(out + written, bytes, count);
        }
        written += count;
    }
    if (*p != '"') return NULL;
    if (out) {
        if (written >= cap) return NULL;
        out[written] = '\0';
    }
    return p + 1;
}

static bool digit(char c) {
    return c >= '0' && c <= '9';
}

static const char *number_end(const char *p) {
    if (*p == '-') p++;
    if (*p == '0') {
        p++;
    } else {
        if (*p < '1' || *p > '9') return NULL;
        while (digit(*p)) p++;
    }
    if (*p == '.') {
        p++;
        if (!digit(*p)) return NULL;
        while (digit(*p)) p++;
    }
    if (*p == 'e' || *p == 'E') {
        p++;
        if (*p == '+' || *p == '-') p++;
        if (!digit(*p)) return NULL;
        while (digit(*p)) p++;
    }
    return p;
}

static const char *value_end(const char *p, unsigned depth) {
    p = native_json_skip_ws(p);
    if (depth > 32) return NULL;
    if (*p == '"') return read_string(p, NULL, 0);
    if (*p == '{' || *p == '[') {
        bool object = *p == '{';
        char close = object ? '}' : ']';
        p = native_json_skip_ws(p + 1);
        if (*p == close) return p + 1;
        for (;;) {
            if (object) {
                p = read_string(p, NULL, 0);
                if (!p) return NULL;
                p = native_json_skip_ws(p);
                if (*p != ':') return NULL;
                p++;
            }
            p = value_end(p, depth + 1);
            if (!p) return NULL;
            p = native_json_skip_ws(p);
            if (*p == close) return p + 1;
            if (*p != ',') return NULL;
            p = native_json_skip_ws(p + 1);
        }
    }
    for (const char **literal = (const char *[]){"true", "false", "null", NULL}; *literal; literal++) {
        size_t n = strlen(*literal);
        if (strncmp(p, *literal, n) == 0) return p + n;
    }
    return number_end(p);
}

bool native_json_validate_object(const char *json) {
    const char *p = native_json_skip_ws(json);
    if (!p || *p != '{') return false;
    p = value_end(p, 0);
    return p && *native_json_skip_ws(p) == '\0';
}

const char *native_json_find_value(const char *json, const char *key) {
    const char *p = native_json_skip_ws(json);
    if (!p || *p != '{' || !key) return NULL;
    p = native_json_skip_ws(p + 1);
    while (*p && *p != '}') {
        char field[64];
        const char *end = read_string(p, field, sizeof(field));
        bool matches = end && strcmp(field, key) == 0;
        if (!end) end = read_string(p, NULL, 0);
        if (!end) return NULL;
        p = native_json_skip_ws(end);
        if (*p != ':') return NULL;
        p = native_json_skip_ws(p + 1);
        if (matches) return p;
        p = value_end(p, 0);
        if (!p) return NULL;
        p = native_json_skip_ws(p);
        if (*p != ',') return NULL;
        p = native_json_skip_ws(p + 1);
    }
    return NULL;
}

static bool scalar_end(const char *p) {
    p = native_json_skip_ws(p);
    return *p == '\0' || *p == ',' || *p == '}' || *p == ']';
}

int native_json_read_string(const char *json, const char *key, char *out, size_t cap) {
    const char *p = native_json_find_value(json, key);
    if (!p) return 0;
    const char *end = read_string(p, out, cap);
    return end && scalar_end(end) ? 1 : -1;
}

static bool integer_end(const char *p, bool allow_negative) {
    if (allow_negative && *p == '-') p++;
    if (!digit(*p) || (*p == '0' && digit(p[1]))) return false;
    while (digit(*p)) p++;
    return scalar_end(p);
}

int native_json_read_u16(const char *json, const char *key, uint16_t min_value,
                         uint16_t max_value, uint16_t *out) {
    const char *p = native_json_find_value(json, key);
    if (!p) return 0;
    if (!integer_end(p, false)) return -1;
    errno = 0;
    unsigned long value = strtoul(p, NULL, 10);
    if (errno != 0 || value < min_value || value > max_value) return -1;
    *out = (uint16_t)value;
    return 1;
}

int native_json_read_i16(const char *json, const char *key, int16_t min_value,
                         int16_t max_value, int16_t *out) {
    const char *p = native_json_find_value(json, key);
    if (!p) return 0;
    if (!integer_end(p, true)) return -1;
    errno = 0;
    long value = strtol(p, NULL, 10);
    if (errno != 0 || value < min_value || value > max_value) return -1;
    *out = (int16_t)value;
    return 1;
}

int native_json_read_bool(const char *json, const char *key, bool *out) {
    const char *p = native_json_find_value(json, key);
    if (!p) return 0;
    if (strncmp(p, "true", 4) == 0 && scalar_end(p + 4)) {
        *out = true;
        return 1;
    }
    if (strncmp(p, "false", 5) == 0 && scalar_end(p + 5)) {
        *out = false;
        return 1;
    }
    return -1;
}
