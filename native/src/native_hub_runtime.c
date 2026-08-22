/* Publish every session slot's live connection, desktop, clock, and mixed-audio
 * metadata to the HUB. Terminal ERROR presentation remains owned by the session
 * supervisor and is deliberately not overwritten here. */
#ifdef HELLOLG_TARGET_WEBOS

#include "native_loop_internal.h"

#include <stdatomic.h>
#include <stdint.h>

#include "audio_pipeline.h"
#include "native_app.h"
#include "native_rdp_state.h"
#include "native_settings.h"
#include "native_time.h"

#include "clog.h"

clog_define(g_native_log_hub_runtime, cLogLevelInfo, cLogFlags_Default, "native", NULL);

void native_hub_runtime_publish(App *app, NativePreconnectUi *ui) {
    /* Keep every card live, including sessions that are connected in the background.
     * Terminal failures drained before this phase own the ERROR state. */
    uint64_t runtime_now = native_monotonic_ms64();
    for (int i = 0; i < NATIVE_SETTINGS_MAX_SESSIONS; i++) {
        NativeSessionSlot *slot = &app->sessions[i];
        bool failed = atomic_load(&slot->session_failed);
        int slot_state = atomic_load(&slot->current_state);
        bool session_exists = slot->rdp && !failed;
        if (!session_exists) {
            app->session_started_ms[i] = 0;
            app->session_runtime_active[i] = false;
            native_preconnect_ui_set_slot_runtime(ui, i, 0, 0, 0, false, 0, 0, 0, 0, 0);
        } else if (slot_state == (int)RDP_STATE_ACTIVE || app->session_runtime_active[i]) {
            /* Start only once the user's connection reaches ACTIVE. Hidden snapshot
             * and watchdog reconnects retain the latch and original start stamp. */
            if (!app->session_runtime_active[i]) {
                app->session_started_ms[i] = runtime_now;
                app->session_runtime_active[i] = true;
                clog(cLogLevelDebug, "%s session reached ACTIVE; starting HUB runtime clock",
                     native_session_slot_name(i));
            }
            uint64_t session_minutes64 = (runtime_now - app->session_started_ms[i]) / 60000u;
            uint32_t session_minutes =
                session_minutes64 > UINT32_MAX ? UINT32_MAX : (uint32_t)session_minutes64;
            NativeAudioSourceStats audio_stats;
            bool audio_stream_open =
                native_audio_pipeline_get_source_stats(&app->audio_pipeline, i, &audio_stats) && audio_stats.open;
            uint32_t audio_codec = audio_stream_open ? atomic_load(&slot->audio_codec) : 0u;
            uint32_t audio_sample_rate = audio_stream_open ? atomic_load(&slot->audio_sample_rate) : 0u;
            uint16_t audio_channels =
                audio_stream_open ? (uint16_t)atomic_load(&slot->audio_channels) : 0u;
            int32_t audio_peak_left = 0;
            int32_t audio_peak_right = 0;
            native_audio_pipeline_get_source_peaks(&app->audio_pipeline, i, &audio_peak_left,
                                                   &audio_peak_right);
            native_preconnect_ui_set_slot_runtime(
                ui, i, (uint16_t)atomic_load(&slot->desktop_width),
                (uint16_t)atomic_load(&slot->desktop_height), session_minutes, audio_stream_open, audio_codec,
                audio_sample_rate, audio_channels, audio_peak_left, audio_peak_right);
        } else {
            /* A fresh connection is still negotiating, so no session clock exists
             * yet. Its card state below remains CONNECTING. */
            native_preconnect_ui_set_slot_runtime(ui, i, 0, 0, 0, false, 0, 0, 0, 0, 0);
        }
        if (!slot->rdp || failed) {
            continue;
        }
        native_preconnect_ui_set_slot_state(
            ui, i,
            slot_state == (int)RDP_STATE_ACTIVE ? NATIVE_PRECONNECT_SESSION_CONNECTED
                                                : NATIVE_PRECONNECT_SESSION_CONNECTING,
            slot_state == (int)RDP_STATE_ACTIVE ? NULL : rdp_state_name((RdpState)slot_state));
    }
}

#endif
