#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
/* XSI superset of the above for sigaltstack()/SA_ONSTACK. */
#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700
#endif

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "audio_pipeline.h"
#include "capture_redirect.h"
#include "cursor_sdl.h"
#include "input_sdl.h"
#include "luna_volume.h"
#include "native_app.h"
#include "native_capture_glue.h"
#include "native_config.h"
#include "native_config_launch.h"
#include "native_config_storage.h"
#include "native_lifecycle.h"
#include "native_loop.h"
#include "native_rdp_media.h"
#include "native_rdp_state.h"
#include "native_session.h"
#include "ui_mixer.h"
#include "webos_platform.h"

#include "clog.h"

clog_define(g_native_log_config, cLogLevelInfo, "native");

int main(int argc, char **argv) {
    native_prepare_webos_logging();
    (void)clog_configure_env();
    clog(cLogLevelNotice, "--- launch ---");
    native_install_termination_hooks();
    native_prepare_webos_environment();

    NativeSettings native_settings;
    native_settings_defaults(&native_settings);

    const char *config_path = native_arg_value(argc, argv, "--config", NATIVE_CONFIG_PATH);
    bool config_required = native_arg_exists(argc, argv, "--config");
    if (!native_config_load_file(&native_settings, config_path, config_required) ||
        !native_config_apply_launch_params(&native_settings, argc, argv) ||
        !native_config_apply_cli(&native_settings, argc, argv)) {
        return 2;
    }

    int sdl_result = native_prepare_sdl_runtime();
    if (sdl_result != 0) {
        return sdl_result;
    }

    bool ignore_saved_config = native_config_launch_ignores_saved_config(argc, argv);
    if (!native_config_load_persisted(&native_settings, ignore_saved_config) ||
        (config_required && !native_config_load_file(&native_settings, config_path, config_required)) ||
        !native_config_apply_launch_params(&native_settings, argc, argv) ||
        !native_config_apply_cli(&native_settings, argc, argv)) {
        native_shutdown_sdl_runtime();
        return 2;
    }

    native_config_log_effective(&native_settings);
#ifdef LGNOME_TARGET_WEBOS
    if (!native_config_validate_runtime(&native_settings)) {
        native_shutdown_sdl_runtime();
        return 2;
    }
#else
    if (!native_config_validate(&native_settings)) {
        native_shutdown_sdl_runtime();
        return 2;
    }
#endif

    NativeWebosPlatformInfo webos_platform;
    if (!native_webos_platform_query(&webos_platform)) {
        clog(cLogLevelWarning,
             "webOS sdkVersion unavailable; DirectMedia ABI probe will decide compatibility");
    } else if (webos_platform.sdk_major < 5) {
        clog(cLogLevelError, "unsupported %s sdkVersion=%s; webOS TV 5.0+ is required",
             native_webos_tv_release(webos_platform.sdk_major),
             webos_platform.sdk_version);
        native_shutdown_sdl_runtime();
        return 2;
    } else if (webos_platform.sdk_major > 11) {
        clog(cLogLevelWarning, "unqualified future sdkVersion=%s; continuing with ABI probe",
             webos_platform.sdk_version);
    }

    App app;
    memset(&app, 0, sizeof(app));
    app.webos_platform = webos_platform;
    app.camera_preview = native_camera_preview_requested(argc, argv);
    app.debug_connect_slot = native_connect_slot_requested(argc, argv);
    if (pthread_mutex_init(&app.video_lock, NULL) != 0) {
        native_shutdown_sdl_runtime();
        return 2;
    }
    if (pthread_mutex_init(&app.audio_lock, NULL) != 0) {
        pthread_mutex_destroy(&app.video_lock);
        native_shutdown_sdl_runtime();
        return 2;
    }
    if (pthread_mutex_init(&app.redaction_lock, NULL) != 0) {
        pthread_mutex_destroy(&app.audio_lock);
        pthread_mutex_destroy(&app.video_lock);
        native_shutdown_sdl_runtime();
        return 2;
    }
    atomic_init(&app.active_index, NATIVE_SESSION_SLOT_GREEN);
    app.rgba_owner_slot = -1;
    app.hub_return_rgba_owner_slot = -1;
    app.hub_return_replacement_slot = -1;
    for (int i = 0; i < NATIVE_SETTINGS_MAX_SESSIONS; i++) {
        NativeSessionSlot *slot = &app.sessions[i];
        slot->app = &app;
        slot->index = i;
        slot->config = native_settings.sessions[i];
        native_cursor_init(&slot->cursor);
        atomic_init(&slot->current_state, (int)RDP_STATE_IDLE);
        atomic_init(&slot->terminal_state, (int)RDP_STATE_IDLE);
        atomic_init(&slot->session_failed, false);
        atomic_init(&slot->desktop_width, NATIVE_RDP_INITIAL_DESKTOP_WIDTH);
        atomic_init(&slot->desktop_height, NATIVE_RDP_INITIAL_DESKTOP_HEIGHT);
        atomic_init(&slot->video_ok_frames, 0);
        atomic_init(&slot->audio_codec, 0u);
        atomic_init(&slot->audio_sample_rate, 0u);
        atomic_init(&slot->audio_channels, 0u);
        atomic_init(&slot->connect_epoch, 0u);
        atomic_init(&slot->keyframe_wait_drops, 0u);
        atomic_init(&slot->snapshot_idr_ready, false);
        atomic_init(&slot->snapshot_last_au_ms, 0u);
        atomic_init(&slot->video_via_bitmap, false);
        app.mixer_gain_db[i] = 0;
        app.duck_mask[i] = (uint8_t)native_settings.sessions[i].duck_mask;
    }
    atomic_init(&app.video_refresh_needed, false);
    app.audio_codec = native_settings.audio_codec;
    if (!app.camera_preview) {
        if (!native_audio_pipeline_init(&app.audio_pipeline) ||
            !native_audio_pipeline_pump_start(&app.audio_pipeline,
                                              native_audio_pipeline_feed_cb,
                                              &app)) {
            clog(cLogLevelWarning,
                 "miniaudio pipeline unavailable; continuing with silent video");
            native_audio_pipeline_destroy(&app.audio_pipeline);
        } else {
            for (int i = 0; i < NATIVE_SETTINGS_MAX_SESSIONS; i++) {
                native_audio_pipeline_set_source_gain(
                    &app.audio_pipeline, i,
                    native_ui_mixer_gain_db_to_q15(app.mixer_gain_db[i]));
            }
            clog(cLogLevelInfo,
                 "miniaudio engine running at 48000Hz stereo, 480-frame blocks");
        }
    }
    app.settings = &native_settings;
    if (!app.camera_preview) {
        NativeCaptureRedirectConfig capture_config = {
            .camera_enabled = native_settings.camera_enabled,
            .camera_device_id = native_settings.camera_device_id,
            .camera_width = native_settings.camera_width,
            .camera_height = native_settings.camera_height,
            .camera_fps = native_settings.camera_fps,
            .audio_input_enabled = native_settings.audio_input_enabled,
            .audio_input_device_id = native_settings.audio_input_device_id,
            .audio_input_gain_db = native_settings.audio_input_gain_db,
        };
        if (!native_capture_redirect_init(&app.capture_redirect,
                                          &capture_config)) {
            clog(cLogLevelWarning,
                 "capture redirect init incomplete; camera/microphone redirection limited or disabled");
        } else {
            /* Record it so the first settings save does not restart the workers
             * without a device or format change. */
            native_capture_record_applied(&app, &capture_config);
        }
    }
    app.mixer_mute_mask = 0;
    app.mixer_solo_mask = 0;
    native_duck_retarget(&app);
    const char *snapshot_force = getenv("LGNOME_SNAPSHOT_FORCE");
    app.snapshot_force = snapshot_force && snapshot_force[0] != '\0' && strcmp(snapshot_force, "0") != 0;
    if (app.snapshot_force) {
        clog(cLogLevelInfo, "LGNOME_SNAPSHOT_FORCE: IDR-snapshot backgrounding for every slot");
    }
#ifdef LGNOME_TARGET_WEBOS
    app.indicator_slot = -1;
    app.system_volume_seen = -1;
    app.system_volume_baseline_seq = 0;
    app.pending_switch_target = -1;
    app.hub_return_slot = -1;
    app.hub_connect_target = -1;
    app.wheel_step = native_settings.wheel_step;
    app.wheel_scroll_divisor = native_settings.wheel_scroll_divisor;
    atomic_init(&app.render_width, 0);
    atomic_init(&app.render_height, 0);
#endif
    atomic_init(&app.running, true);
    atomic_init(&app.exit_code, 0);
    native_input_init(&app.input, NULL, NATIVE_RDP_INITIAL_DESKTOP_WIDTH, NATIVE_RDP_INITIAL_DESKTOP_HEIGHT);
    /* System-volume bridge for the MASTER fader; harmless where luna-send-pub does not
     * exist (every call fails, the fader just stays dimmed). */
    if (!app.camera_preview) {
        if (!native_luna_volume_start(&app.luna_volume)) {
            clog(cLogLevelWarning,
                 "volume worker failed to start; the MASTER fader stays unavailable");
        }
        native_luna_volume_refresh(&app.luna_volume);
    }

#ifndef LGNOME_TARGET_WEBOS
    int startup_slot = app.debug_connect_slot >= 0 ? app.debug_connect_slot
                                                   : NATIVE_SESSION_SLOT_GREEN;
    atomic_store(&app.active_index, startup_slot);
    native_duck_retarget(&app);
    app.sessions[startup_slot].config = native_settings.sessions[startup_slot];
    if (!native_slot_connect(&app, startup_slot, false)) {
        int exit_code = atomic_load(&app.exit_code);
        for (int i = 0; i < NATIVE_SETTINGS_MAX_SESSIONS; i++) {
            native_cursor_destroy(&app.sessions[i].cursor);
        }
        native_capture_redirect_destroy(&app.capture_redirect);
        native_audio_pipeline_destroy(&app.audio_pipeline);
        pthread_mutex_destroy(&app.redaction_lock);
        pthread_mutex_destroy(&app.audio_lock);
        pthread_mutex_destroy(&app.video_lock);
        native_shutdown_sdl_runtime();
        return exit_code ? exit_code : 2;
    }
#endif

    int loop_result = native_run_app_loop(&app, &native_settings);
    if (loop_result != 0 && atomic_load(&app.exit_code) == 0) {
        atomic_store(&app.exit_code, loop_result);
    }

    native_stop_all_sessions(&app);
    native_capture_redirect_destroy(&app.capture_redirect);
    native_luna_volume_stop(&app.luna_volume);
    /* The pump feeds through audio_lock; stop it before destroying that lock. */
    native_audio_pipeline_destroy(&app.audio_pipeline);
    for (int i = 0; i < NATIVE_SETTINGS_MAX_SESSIONS; i++) {
        native_cursor_destroy(&app.sessions[i].cursor);
    }
    pthread_mutex_destroy(&app.redaction_lock);
    pthread_mutex_destroy(&app.audio_lock);
    pthread_mutex_destroy(&app.video_lock);
    native_shutdown_sdl_runtime();

    int exit_code = atomic_load(&app.exit_code);
    if (exit_code == 0 && app.decoder_errors > 0) {
        exit_code = rdp_state_exit_code(RDP_STATE_DECODER_ERROR);
    }
    return exit_code;
}
