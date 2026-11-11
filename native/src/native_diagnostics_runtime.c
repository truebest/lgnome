#ifdef LGNOME_TARGET_WEBOS
#include "native_diagnostics.h"
#include "native_statistics.h"

#include <stdio.h>
#include "native_app.h"
#include "native_rdp_state.h"
#include "native_time.h"

void native_diagnostics_tick(App *app) {
    NativeDiagnostics *d = &app->diagnostics;
    bool enabled = native_preconnect_ui_statistics_enabled(app->preconnect_ui);
    if (!enabled && !d->enabled) return;
    uint32_t now = native_monotonic_ms();
    int index = atomic_load(&app->active_index);
    NativeSessionSlot *slot = &app->sessions[index];
    unsigned epoch = atomic_load(&slot->connect_epoch);
    bool changed = !d->enabled || !enabled || d->snapshot.slot != index || d->window.before.epoch != epoch;
    if (!changed && d->sampled && now - d->sampled_ms < NATIVE_DIAGNOSTICS_INTERVAL_MS) return;
    /* A saturated decoder can starve trylock sampling indefinitely. */
    pthread_mutex_lock(&app->video_lock);
    now = native_monotonic_ms();
    native_video_set_diagnostics(app->video, enabled);
    if (!enabled) {
        pthread_mutex_unlock(&app->video_lock);
        memset(d, 0, sizeof(*d));
        return;
    }
    NativeDiagnosticsSnapshot *s = &d->snapshot;
    memset(s, 0, sizeof(*s));
    s->at_ms = now;
    RdpState connection_state = atomic_load(&slot->current_state);
    s->connected = connection_state == RDP_STATE_ACTIVE;
    s->connection_status = rdp_state_name(connection_state);
    s->session_uptime_valid = app->session_runtime_active[index];
    if (s->session_uptime_valid) s->session_uptime_s = native_monotonic_s() - app->session_started_s[index];
    s->slot = index;
    snprintf(s->name, sizeof(s->name), "%s", slot->config.name);
    s->width = (uint16_t)atomic_load(&slot->desktop_width);
    s->height = (uint16_t)atomic_load(&slot->desktop_height);
    s->bitmap = atomic_load(&slot->video_via_bitmap);
    s->codec = slot->video_codec;
    s->video_available = slot->video_seen;
    native_diagnostics_counter_epoch(&slot->diagnostics, epoch);
    s->counters = slot->diagnostics;
    s->rates = native_diagnostics_rates(&d->window, index, s->counters, now);
    if (!s->bitmap && app->video_owner_slot == index && app->video_owner_epoch == epoch) {
        s->ndl = native_video_get_diagnostics(app->video, now);
        s->decoder = native_video_get_decoder_stats(app->video);
    }
    if (atomic_load(&slot->reconnecting)) s->status = "Reconnecting";
    else if (atomic_load(&slot->current_state) != RDP_STATE_ACTIVE) s->status = "Connecting";
    else if (slot->snapshot.armed) s->status = "Preparing cache";
    else if (slot->refresh_waiting || app->decoder_keyframe_pending || app->switch_deadline_ticks)
        s->status = "Waiting for keyframe";
    else s->status = "Active";
    pthread_mutex_unlock(&app->video_lock);
    rdp_session_get_network_stats(slot->rdp, &s->network);
    for (int i = 0; i < NATIVE_SETTINGS_MAX_SESSIONS; i++) {
        NativeSessionSlot *source = &app->sessions[i];
        NativeDiagnosticsAudio *audio = &s->audio[i];
        audio->connected = atomic_load(&source->current_state) == RDP_STATE_ACTIVE;
        audio->codec = atomic_load(&source->audio_codec);
        audio->sample_rate = atomic_load(&source->audio_sample_rate);
        audio->channels = (uint16_t)atomic_load(&source->audio_channels);
        if (audio->connected) native_audio_pipeline_get_source_stats(&app->audio_pipeline, i, &audio->source);
    }
    native_diagnostics_sample_system(d, now);
    native_statistics_sample(&d->timeline, s);
    d->enabled = true;
    d->sampled = true;
    d->sampled_ms = now;
}
#endif
