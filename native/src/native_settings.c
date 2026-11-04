#include "native_settings.h"

#include "native_config.h"

#include <stdio.h>
#include <string.h>

const char *native_session_slot_name(int slot) {
    switch (slot) {
    case NATIVE_SESSION_SLOT_RED:
        return "red";
    case NATIVE_SESSION_SLOT_GREEN:
        return "green";
    case NATIVE_SESSION_SLOT_YELLOW:
        return "yellow";
    case NATIVE_SESSION_SLOT_BLUE:
        return "blue";
    default:
        return "?";
    }
}

void native_session_config_defaults(NativeSessionConfig *session) {
    memset(session, 0, sizeof(*session));
    session->port = 3389;
    session->fps = 60;
    session->desktop_width = NATIVE_RDP_INITIAL_DESKTOP_WIDTH;
    session->desktop_height = NATIVE_RDP_INITIAL_DESKTOP_HEIGHT;
    session->desktop_scale_factor = 100;
}

void native_settings_defaults(NativeSettings *settings) {
    memset(settings, 0, sizeof(*settings));
    for (int i = 0; i < NATIVE_SETTINGS_MAX_SESSIONS; i++) {
        native_session_config_defaults(&settings->sessions[i]);
    }
    (void)snprintf(settings->sessions[NATIVE_SESSION_SLOT_GREEN].host,
                   sizeof(settings->sessions[NATIVE_SESSION_SLOT_GREEN].host), "127.0.0.1");
    settings->wheel_step = 60;
    settings->wheel_scroll_divisor = 1;
    settings->audio_codec = NATIVE_AUDIO_CODEC_AUTO;
    settings->camera_width = 640;
    settings->camera_height = 480;
    settings->camera_fps = 15;
    settings->audio_input_gain_db = 0;
}

bool native_desktop_scale_valid(uint16_t scale) {
    return scale == 100 || scale == 150 || scale == 200;
}

bool native_certificate_fingerprint_valid(const char *sha256) {
    if (!sha256 || strlen(sha256) != 64) return false;
    for (const char *p = sha256; *p; ++p) {
        if (!(*p >= '0' && *p <= '9') && !(*p >= 'a' && *p <= 'f') && !(*p >= 'A' && *p <= 'F')) return false;
    }
    return true;
}

const char *native_session_trusted_certificate(const NativeSessionConfig *session) {
    const NativeCertificateTrust *trust = &session->certificate_trust;
    return trust->port == session->port && strcmp(trust->host, session->host) == 0 &&
                   native_certificate_fingerprint_valid(trust->sha256) ? trust->sha256 : "";
}

bool native_session_trust_certificate(NativeSessionConfig *session, const char *sha256) {
    if (!native_certificate_fingerprint_valid(sha256)) return false;
    memcpy(session->certificate_trust.sha256, sha256, 65);
    memcpy(session->certificate_trust.host, session->host, sizeof(session->host));
    session->certificate_trust.port = session->port;
    return true;
}
