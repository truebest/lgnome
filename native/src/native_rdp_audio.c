/* Best-effort RDP audio decode and routing into the shared mixer. */

#include "native_rdp_callback_parts.h"

#include <stdatomic.h>

#include "native_app.h"
#include "native_rdp_media.h"

#include "clog.h"

clog_define(g_native_log_rdp_audio, cLogLevelInfo, cLogFlags_Default, "native", NULL);

/* Audio is strictly best-effort: any failure here logs and degrades to silence for that
 * session; neither handler ever stops a session (and must never call rdp_session_stop,
 * which would self-join the rdp-worker thread these callbacks run on). */
static void on_audio_format(void *ctx, uint32_t codec, uint32_t sample_rate, uint16_t channels) {
    NativeSessionSlot *slot = (NativeSessionSlot *)ctx;
    if (!slot) {
        return;
    }
    App *app = slot->app;
    slot->audio_routed = false;
    atomic_store(&slot->audio_codec, 0u);
    atomic_store(&slot->audio_sample_rate, 0u);
    atomic_store(&slot->audio_channels, 0u);
    if (codec != RDP_AUDIO_CODEC_PCM_S16LE && codec != RDP_AUDIO_CODEC_OPUS) {
        if (!slot->audio_incompatible_logged) {
            clog(cLogLevelWarning, "%s session negotiated unsupported codec %u; muting it in the mix",
                 native_session_slot_name(slot->index), (unsigned)codec);
            slot->audio_incompatible_logged = true;
        }
        native_audio_pipeline_close_source(&app->audio_pipeline, slot->index);
        return;
    }
    if (codec == RDP_AUDIO_CODEC_OPUS) {
        /* Fresh stream (or renegotiation): restart decoder state. Safe against
         * on_audio_data — both run on this session's own rdp-worker thread. */
        native_opus_decoder_close(slot->opus_decoder);
        slot->opus_decoder = native_opus_decoder_open(sample_rate, channels);
        if (!slot->opus_decoder) {
            if (!slot->audio_incompatible_logged) {
                clog(cLogLevelWarning, "%s session: no Opus decoder available; muting it in the mix",
                     native_session_slot_name(slot->index));
                slot->audio_incompatible_logged = true;
            }
            native_audio_pipeline_close_source(&app->audio_pipeline, slot->index);
            return;
        }
    } else if (slot->opus_decoder) {
        native_opus_decoder_close(slot->opus_decoder);
        slot->opus_decoder = NULL;
    }

    if (!native_audio_pipeline_set_source_format(&app->audio_pipeline, slot->index, sample_rate, channels)) {
        if (!slot->audio_incompatible_logged) {
            clog(cLogLevelWarning, "%s session has unsupported PCM format %uHz %uch; muting it",
                 native_session_slot_name(slot->index), (unsigned)sample_rate, (unsigned)channels);
            slot->audio_incompatible_logged = true;
        }
        native_opus_decoder_close(slot->opus_decoder);
        slot->opus_decoder = NULL;
        native_audio_pipeline_close_source(&app->audio_pipeline, slot->index);
        return;
    }
    atomic_store(&slot->audio_codec, codec);
    atomic_store(&slot->audio_sample_rate, sample_rate);
    atomic_store(&slot->audio_channels, channels);
    slot->audio_routed = true;
    slot->audio_incompatible_logged = false;
    clog(cLogLevelInfo, "%s session audio joined the mix (%s %uHz %uch)",
         native_session_slot_name(slot->index), codec == RDP_AUDIO_CODEC_OPUS ? "Opus->PCM" : "PCM",
         (unsigned)sample_rate, (unsigned)channels);

    pthread_mutex_lock(&app->video_lock);
    pthread_mutex_lock(&app->audio_lock);
    if (!app->audio) {
        /* First working audio format and the speculative open didn't happen (or failed):
         * bring the shared track up now. */
        NativeMedia *media = native_ensure_media_locked(app);
        if (media) {
            app->audio = native_audio_open(media, RDP_AUDIO_CODEC_PCM_S16LE,
                                           NATIVE_AUDIO_PIPELINE_SAMPLE_RATE,
                                           NATIVE_AUDIO_PIPELINE_CHANNELS);
        }
        if (app->audio) {
            if (app->video) {
                /* First-time open under a live video stream reloads the shared pipeline
                 * (webOS); drop the dead video track so on_video_au reopens it on the next
                 * SPS+PPS+IDR instead of feeding P-frames into a fresh decoder — and ask
                 * the SDL thread to force that keyframe (gnome-remote-desktop never
                 * resends an IDR spontaneously). */
                clog(cLogLevelNotice,
                     "audio open reloaded the media pipeline; reopening video on next keyframe");
                native_video_close(app->video);
                app->video = NULL;
                app->decoder_keyframe_pending = false;
                atomic_store(&app->video_refresh_needed, true);
            }
        } else {
            clog(cLogLevelWarning,
                 "audio sink unavailable (PCM 48000Hz 2ch); continuing with silent video");
        }
    }
    pthread_mutex_unlock(&app->audio_lock);
    pthread_mutex_unlock(&app->video_lock);
}

static void on_audio_data(void *ctx, const uint8_t *data, size_t len, uint32_t ts_ms) {
    NativeSessionSlot *slot = (NativeSessionSlot *)ctx;
    if (!slot || !data || len == 0 || !slot->audio_routed) {
        return;
    }
    App *app = slot->app;
    if (slot->opus_decoder) {
        const int16_t *pcm = NULL;
        int frames = native_opus_decoder_decode(slot->opus_decoder, data, len, &pcm);
        if (frames > 0) {
            (void)native_audio_pipeline_push(&app->audio_pipeline, slot->index, pcm, (size_t)frames, ts_ms);
        }
        return;
    }
    size_t frame_bytes = (size_t)atomic_load(&slot->audio_channels) * sizeof(int16_t);
    size_t frames = frame_bytes ? len / frame_bytes : 0;
    if (frames > 0) {
        (void)native_audio_pipeline_push(&app->audio_pipeline, slot->index,
                                         (const int16_t *)(const void *)data, frames, ts_ms);
    }
}

void native_rdp_install_audio_callbacks(RdpCallbacks *callbacks) {
    callbacks->on_audio_format = on_audio_format;
    callbacks->on_audio_data = on_audio_data;
}
