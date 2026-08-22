#include "native_settings.h"

#include <stdio.h>
#include <string.h>

#include "clog.h"

clog_define(g_native_log_settings, cLogLevelInfo, cLogFlags_Default, "config.settings", NULL);

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

void native_settings_defaults(NativeSettings *settings) {
    memset(settings, 0, sizeof(*settings));
    for (int i = 0; i < NATIVE_SETTINGS_MAX_SESSIONS; i++) {
        settings->sessions[i].port = 3389;
        settings->sessions[i].fps = 60;
        settings->sessions[i].duck_mask = 0; /* ducking is opt-in, per channel */
    }
    (void)snprintf(settings->sessions[NATIVE_SESSION_SLOT_GREEN].host,
                   sizeof(settings->sessions[NATIVE_SESSION_SLOT_GREEN].host), "127.0.0.1");
    settings->width = 1920;
    settings->height = 1080;
    settings->wheel_step = 60;
    settings->wheel_scroll_divisor = 1;
    settings->audio_codec = NATIVE_AUDIO_CODEC_AUTO;
    settings->camera_width = 640;
    settings->camera_height = 480;
    settings->camera_fps = 15;
    settings->audio_input_gain_db = 0;
}

void native_settings_warn_deprecated_audio_prebuffer(void) {
    clog_once(cLogLevelWarning,
              "audioPrebufferMs/--audio-prebuffer-ms is deprecated and ignored; adaptive buffering is always enabled");
}
