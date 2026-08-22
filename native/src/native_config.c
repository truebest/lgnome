#include "native_config.h"

#include <stdio.h>
#include <string.h>

#include "camera_v4l2.h"

#include "clog.h"

clog_define(g_native_log_app_config, cLogLevelInfo, cLogFlags_Default, "native", NULL);

bool copy_config_string(char *dest, size_t cap, const char *value, const char *field) {
    if (!dest || cap == 0) {
        return false;
    }
    if (!value) {
        value = "";
    }
    size_t len = strlen(value);
    if (len >= cap) {
        clog(cLogLevelError, "config field %s is too long", field);
        return false;
    }
    memcpy(dest, value, len + 1);
    return true;
}

bool native_session_endpoint_changed(const NativeSessionConfig *before, const NativeSessionConfig *after) {
    return strcmp(before->host, after->host) != 0 || before->port != after->port;
}

bool native_session_connection_config_changed(const NativeSessionConfig *before,
                                              const NativeSessionConfig *after) {
    return native_session_endpoint_changed(before, after) ||
           strcmp(before->username, after->username) != 0 ||
           strcmp(before->password, after->password) != 0 ||
           strcmp(before->domain, after->domain) != 0 || before->fps != after->fps ||
           before->camera_redirect != after->camera_redirect ||
           before->audio_input_redirect != after->audio_input_redirect;
}

/* The global capture gates are simply "some profile wants it". Recomputing them from
 * the candidate keeps a per-profile toggle effective without importing the camera
 * drawer's unsaved device draft, which the user has not confirmed. */
void native_settings_recompute_capture_gates(NativeSettings *settings) {
    if (!settings) {
        return;
    }
    settings->camera_enabled = false;
    settings->audio_input_enabled = false;
    for (int i = 0; i < NATIVE_SETTINGS_MAX_SESSIONS; i++) {
        settings->camera_enabled |= settings->sessions[i].camera_redirect;
        settings->audio_input_enabled |= settings->sessions[i].audio_input_redirect;
    }
}

void native_config_apply_initial_desktop_hint(NativeSettings *settings) {
    if (!settings) {
        return;
    }
    settings->width = NATIVE_RDP_INITIAL_DESKTOP_WIDTH;
    settings->height = NATIVE_RDP_INITIAL_DESKTOP_HEIGHT;
}

bool native_config_validate_runtime(const NativeSettings *settings) {
    bool ok = true;
    if (settings->width == 0 || settings->height == 0 || settings->wheel_step == 0 ||
        settings->wheel_scroll_divisor == 0) {
        clog(cLogLevelError, "invalid zero value in RDP config");
        ok = false;
    }
    for (int slot = 0; slot < NATIVE_SETTINGS_MAX_SESSIONS; slot++) {
        const NativeSessionConfig *session = &settings->sessions[slot];
        if (session->port == 0 || session->fps == 0) {
            clog(cLogLevelError, "invalid zero value in %s session config", native_session_slot_name(slot));
            ok = false;
        }
    }
    if (settings->camera_width < NATIVE_CAMERA_MIN_WIDTH ||
        settings->camera_width > NATIVE_CAMERA_MAX_WIDTH ||
        settings->camera_height < NATIVE_CAMERA_MIN_HEIGHT ||
        settings->camera_height > NATIVE_CAMERA_MAX_HEIGHT ||
        (settings->camera_width & 1u) != 0u || (settings->camera_height & 1u) != 0u ||
        settings->camera_fps == 0u || settings->camera_fps > NATIVE_CAMERA_MAX_FPS ||
        settings->audio_input_gain_db < -12 || settings->audio_input_gain_db > 18) {
        clog(cLogLevelError, "invalid camera/microphone capture config");
        ok = false;
    }
    return ok;
}

bool native_session_config_validate_connect(const NativeSessionConfig *session, int slot, char *message,
                                            size_t message_cap) {
    bool ok = true;
    const char *slot_name = native_session_slot_name(slot);
    if (!session->host[0]) {
        clog(cLogLevelError, "missing RDP host for the %s session", slot_name);
        ok = false;
    }
    if (!session->username[0]) {
        clog(cLogLevelError, "missing RDP username for the %s session", slot_name);
        ok = false;
    }
    if (!session->password[0]) {
        clog(cLogLevelError, "missing RDP password for the %s session", slot_name);
        ok = false;
    }
    if (session->port == 0 || session->fps == 0) {
        clog(cLogLevelError, "invalid zero value in %s session config", slot_name);
        ok = false;
    }
    if (!ok && message && message_cap > 0) {
        if (!session->host[0]) {
            (void)snprintf(message, message_cap, "Enter an IP address for the %s server.", slot_name);
        } else if (!session->username[0] || !session->password[0]) {
            (void)snprintf(message, message_cap, "Missing username or password for the %s server.", slot_name);
        } else {
            (void)snprintf(message, message_cap, "Invalid RDP config.");
        }
    }
    return ok;
}

bool native_config_validate(const NativeSettings *settings) {
    return native_session_config_validate_connect(&settings->sessions[NATIVE_SESSION_SLOT_GREEN],
                                                  NATIVE_SESSION_SLOT_GREEN, NULL, 0) &&
           native_config_validate_runtime(settings);
}

void native_config_log_effective(const NativeSettings *settings) {
    for (int slot = 0; slot < NATIVE_SETTINGS_MAX_SESSIONS; slot++) {
        const NativeSessionConfig *session = &settings->sessions[slot];
        if (slot != NATIVE_SESSION_SLOT_GREEN && !session->host[0]) {
            continue; /* unconfigured extra slot */
        }
        clog(cLogLevelInfo,
             "effective %s RDP config host=%s port=%u username=%s password=%s domain=%s fps=%u camera=%s microphone=%s",
             native_session_slot_name(slot), session->host, (unsigned)session->port,
             session->username[0] ? "set" : "missing", session->password[0] ? "set" : "missing",
             session->domain[0] ? "set" : "empty", (unsigned)session->fps,
             session->camera_redirect ? "on" : "off", session->audio_input_redirect ? "on" : "off");
    }
    clog(cLogLevelInfo,
         "effective globals desktop=%ux%u wheelStep=%u wheelScrollDivisor=%u audioCodec=%s camera=%s/%ux%u@%u microphone=%s/gain%+d",
         (unsigned)settings->width, (unsigned)settings->height, (unsigned)settings->wheel_step,
         (unsigned)settings->wheel_scroll_divisor,
         settings->audio_codec == NATIVE_AUDIO_CODEC_PCM ? "pcm" : "auto",
         settings->camera_enabled ? "on" : "off", (unsigned)settings->camera_width,
         (unsigned)settings->camera_height, (unsigned)settings->camera_fps,
         settings->audio_input_enabled ? "on" : "off", (int)settings->audio_input_gain_db);
}
