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
