#include "native_config_launch.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "native_config.h"
#include "native_json.h"
#include "settings_json.h"

#include "clog.h"

clog_define(g_native_log_config_launch, cLogLevelInfo, "native");

static bool native_config_apply_json_if_present(NativeSettings *settings, const char *json, const char *source,
                                                bool *applied) {
    if (!native_settings_json_has_rdp_key(json)) {
        return true;
    }
    if (!native_settings_apply_json(settings, json, source)) {
        clog(cLogLevelError, "failed to parse %s", source);
        return false;
    }
    clog(cLogLevelInfo, "loaded %s", source);
    *applied = true;
    return true;
}

static bool native_config_apply_launch_json_string(NativeSettings *settings, const char *json, const char *key,
                                                   int arg_index, bool *arg_applied) {
    const char *value = native_json_find_value(json, key);
    if (!value || *native_json_skip_ws(value) != '"') {
        return true;
    }

    char nested[NATIVE_CONFIG_MAX_FILE];
    int result = native_json_read_string(json, key, nested, sizeof(nested));
    if (result < 0) {
        clog(cLogLevelError, "invalid string value for webOS launch field %s", key);
        return false;
    }
    if (result == 0) {
        return true;
    }

    const char *nested_json = native_json_skip_ws(nested);
    if (!nested_json || nested_json[0] != '{') {
        return true;
    }
    char source[96];
    (void)snprintf(source, sizeof(source), "webOS launch argument %d %s JSON", arg_index, key);
    return native_config_apply_json_if_present(settings, nested_json, source, arg_applied);
}

bool native_config_apply_launch_params(NativeSettings *settings, int argc, char **argv) {
    bool saw_launch_json = false;
    bool saw_launch_control = false;
    bool applied = false;
    for (int i = 1; i < argc; i++) {
        const char *params = native_json_skip_ws(argv[i]);
        if (!params || params[0] != '{') {
            continue;
        }

        saw_launch_json = true;
        bool arg_has_launch_control =
            native_json_find_value(params, "cameraPreview") != NULL ||
            native_json_find_value(params, "ignoreSavedConfig") != NULL ||
            native_json_find_value(params, "connectSlot") != NULL;
        saw_launch_control = saw_launch_control || arg_has_launch_control;
        bool arg_applied = false;
        char source[64];
        (void)snprintf(source, sizeof(source), "webOS launch argument %d", i);
        if (!native_config_apply_json_if_present(settings, params, source, &arg_applied) ||
            !native_config_apply_launch_json_string(settings, params, "params", i, &arg_applied) ||
            !native_config_apply_launch_json_string(settings, params, "launchParams", i, &arg_applied)) {
            return false;
        }
        if (!arg_applied && !arg_has_launch_control) {
            clog(cLogLevelWarning, "webOS launch argument %d has no RDP config keys", i);
        }
        applied = applied || arg_applied;
    }
    if (saw_launch_json && !applied && !saw_launch_control) {
        clog(cLogLevelWarning, "webOS launch parameters did not override RDP config");
    }
    return true;
}

static bool native_config_launch_json_bool_enabled(const char *json, const char *name) {
    bool enabled = false;
    if (!json || json[0] != '{') {
        return false;
    }
    if (native_json_read_bool(json, name, &enabled) > 0 && enabled) {
        return true;
    }

    for (const char **key = (const char *[]){"params", "launchParams", NULL}; *key; key++) {
        char nested[NATIVE_CONFIG_MAX_FILE];
        int result = native_json_read_string(json, *key, nested, sizeof(nested));
        if (result <= 0) {
            continue;
        }
        const char *nested_json = native_json_skip_ws(nested);
        if (native_config_launch_json_bool_enabled(nested_json, name)) {
            return true;
        }
    }
    return false;
}

bool native_config_launch_ignores_saved_config(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        const char *params = native_json_skip_ws(argv[i]);
        if (native_config_launch_json_bool_enabled(params, "ignoreSavedConfig")) {
            return true;
        }
    }
    return false;
}

bool native_camera_preview_requested(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--camera-preview") == 0) {
            return true;
        }
        const char *params = native_json_skip_ws(argv[i]);
        if (native_config_launch_json_bool_enabled(params, "cameraPreview")) {
            return true;
        }
    }
    return false;
}

/* Debugging aid: names a saved profile to open at launch instead of waiting on the
 * hub for a colour-button press, which an automated on-device check cannot send. It
 * carries no connection data of its own -- the named profile must already be
 * configured, and the connection then takes the ordinary user-initiated path. */
static int native_config_launch_json_connect_slot(const char *json) {
    if (!json || json[0] != '{') {
        return -1;
    }
    char name[32];
    if (native_json_read_string(json, "connectSlot", name, sizeof(name)) > 0) {
        for (int slot = 0; slot < NATIVE_SETTINGS_MAX_SESSIONS; slot++) {
            if (strcmp(name, native_session_slot_name(slot)) == 0) {
                return slot;
            }
        }
        clog(cLogLevelWarning, "ignoring unknown connectSlot \"%s\"", name);
        return -1;
    }

    for (const char **key = (const char *[]){"params", "launchParams", NULL}; *key;
         key++) {
        char nested[NATIVE_CONFIG_MAX_FILE];
        if (native_json_read_string(json, *key, nested, sizeof(nested)) <= 0) {
            continue;
        }
        int slot = native_config_launch_json_connect_slot(native_json_skip_ws(nested));
        if (slot >= 0) {
            return slot;
        }
    }
    return -1;
}

const char *native_arg_value(int argc, char **argv, const char *name, const char *fallback) {
    size_t n = strlen(name);
    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], name, n) == 0 && argv[i][n] == '=') {
            return argv[i] + n + 1;
        }
        if (strcmp(argv[i], name) == 0 && i + 1 < argc) {
            return argv[i + 1];
        }
    }
    return fallback;
}

bool native_arg_exists(int argc, char **argv, const char *name) {
    return native_arg_value(argc, argv, name, NULL) != NULL;
}

int native_connect_slot_requested(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        int slot = native_config_launch_json_connect_slot(native_json_skip_ws(argv[i]));
        if (slot >= 0) {
            return slot;
        }
    }
    const char *name = native_arg_value(argc, argv, "--connect-slot", NULL);
    if (!name) {
        return -1;
    }
    for (int slot = 0; slot < NATIVE_SETTINGS_MAX_SESSIONS; slot++) {
        if (strcmp(name, native_session_slot_name(slot)) == 0) {
            return slot;
        }
    }
    clog(cLogLevelWarning, "ignoring unknown --connect-slot \"%s\"", name);
    return -1;
}

static bool apply_cli_string(int argc, char **argv, const char *arg_name, char *dest, size_t cap, const char *field) {
    const char *value = native_arg_value(argc, argv, arg_name, NULL);
    if (!value) {
        return true;
    }
    return copy_config_string(dest, cap, value, field);
}

static bool parse_u16_text(const char *text, uint16_t min_value, uint16_t max_value, uint16_t *out) {
    if (!text || !text[0]) {
        return false;
    }
    errno = 0;
    char *end = NULL;
    unsigned long value = strtoul(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value < (unsigned long)min_value ||
        value > (unsigned long)max_value) {
        return false;
    }
    *out = (uint16_t)value;
    return true;
}

static bool apply_cli_u16(int argc, char **argv, const char *arg_name, uint16_t min_value, uint16_t max_value,
                          uint16_t *dest) {
    const char *value = native_arg_value(argc, argv, arg_name, NULL);
    if (!value) {
        return true;
    }
    uint16_t parsed = 0;
    if (!parse_u16_text(value, min_value, max_value, &parsed)) {
        clog(cLogLevelError, "invalid value for %s", arg_name);
        return false;
    }
    *dest = parsed;
    return true;
}

static bool apply_cli_audio_codec(int argc, char **argv, NativeSettings *settings) {
    const char *value = native_arg_value(argc, argv, "--audio-codec", NULL);
    if (!value) {
        return true;
    }
    if (strcmp(value, "auto") == 0 || strcmp(value, "opus") == 0) {
        settings->audio_codec = NATIVE_AUDIO_CODEC_AUTO;
        return true;
    }
    if (strcmp(value, "pcm") == 0) {
        settings->audio_codec = NATIVE_AUDIO_CODEC_PCM;
        return true;
    }
    clog(cLogLevelError, "invalid value for --audio-codec (expected auto or pcm)");
    return false;
}

bool native_config_apply_cli(NativeSettings *settings, int argc, char **argv) {
    /* Flat CLI flags target the GREEN slot; the yellow
     * slot is configured via the settings file, launch JSON ("sessions" array) or UI. */
    NativeSessionConfig *green = &settings->sessions[NATIVE_SESSION_SLOT_GREEN];
    if (!(apply_cli_string(argc, argv, "--host", green->host, sizeof(green->host), "host") &&
          apply_cli_string(argc, argv, "--user", green->username, sizeof(green->username), "username") &&
          apply_cli_string(argc, argv, "--username", green->username, sizeof(green->username), "username") &&
          apply_cli_string(argc, argv, "--password", green->password, sizeof(green->password), "password") &&
          apply_cli_string(argc, argv, "--domain", green->domain, sizeof(green->domain), "domain") &&
          apply_cli_u16(argc, argv, "--port", 1, UINT16_MAX, &green->port) &&
          apply_cli_u16(argc, argv, "--fps", 1, 240, &green->fps) &&
          apply_cli_u16(argc, argv, "--wheel-step", 1, 120, &settings->wheel_step) &&
          apply_cli_u16(argc, argv, "--wheel-scroll-divisor", 1, 120, &settings->wheel_scroll_divisor) &&
          apply_cli_audio_codec(argc, argv, settings))) {
        return false;
    }
    return true;
}
