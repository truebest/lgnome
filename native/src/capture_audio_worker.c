#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

/* Shared microphone capture, per-session PCM conversion/packetization, and
 * foreground-gated RDPEAI submission. */

#include "capture_redirect_internal.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "audio_input_alsa.h"
#include "audio_input_pcm.h"
#include "clog.h"
#include "native_time.h"

clog_define(g_native_log_capture_audio_worker, cLogLevelInfo, cLogFlags_Default, "capture.redirect", NULL);

#define AUDIO_DEVICE_RETRY_MS 1000u
#define AUDIO_READ_FRAMES 480u

typedef struct CaptureTarget {
    int slot;
    RdpSession *session;
    unsigned state_generation;
} CaptureTarget;

typedef struct AudioPacketSink {
    NativeCaptureRedirectImpl *impl;
    CaptureTarget target;
    uint64_t generation;
} AudioPacketSink;

typedef struct AudioDesiredStream {
    bool active;
    CaptureTarget target;
    uint64_t generation;
    uint32_t frames_per_packet;
} AudioDesiredStream;

typedef struct AudioStreamState {
    NativeAudioInputPcm pcm;
    AudioPacketSink sink;
    CaptureTarget target;
    uint64_t generation;
    uint32_t frames_per_packet;
    uint32_t input_rate;
    uint16_t input_channels;
    bool active;
} AudioStreamState;

static bool target_matches_locked(const NativeCaptureRedirectImpl *impl, const CaptureTarget *target) {
    if (!target || !capture_slot_index_valid(target->slot) ||
        impl->slot_generation[target->slot] != target->state_generation) {
        return false;
    }
    return impl->slots[target->slot].session == target->session && target->session != NULL;
}

static bool capture_slot_is_foreground_locked(const NativeCaptureRedirectImpl *impl, int index) {
    return impl->foreground_slot == index;
}

static void audio_packet(void *context, const int16_t *samples, size_t frames) {
    AudioPacketSink *sink = (AudioPacketSink *)context;
    NativeCaptureRedirectImpl *impl = sink->impl;
    pthread_mutex_lock(&impl->lock);
    if (target_matches_locked(impl, &sink->target)) {
        NativeCaptureSlot *slot = &impl->slots[sink->target.slot];
        if (capture_slot_is_foreground_locked(impl, sink->target.slot) && slot->audio_streaming &&
            slot->audio_generation == sink->generation) {
            (void)rdp_submit_audio_input_pcm(sink->target.session, sink->generation,
                                             (const uint8_t *)(const void *)samples,
                                             frames * NATIVE_AUDIO_INPUT_WIRE_CHANNELS * sizeof(int16_t));
        }
    }
    pthread_mutex_unlock(&impl->lock);
}

static unsigned audio_collect_desired_locked(const NativeCaptureRedirectImpl *impl,
                                             AudioDesiredStream desired[NATIVE_CAPTURE_REDIRECT_MAX_SLOTS]) {
    unsigned count = 0;
    for (int i = 0; i < NATIVE_CAPTURE_REDIRECT_MAX_SLOTS; i++) {
        const NativeCaptureSlot *slot = &impl->slots[i];
        AudioDesiredStream *stream = &desired[i];
        memset(stream, 0, sizeof(*stream));
        stream->target.slot = -1;
        if (!impl->audio_enabled || !slot->session || !slot->audio_allowed ||
            !capture_slot_is_foreground_locked(impl, i) || !slot->audio_streaming ||
            slot->audio_frames_per_packet == 0u) {
            continue;
        }
        stream->active = true;
        stream->target.slot = i;
        stream->target.session = slot->session;
        stream->target.state_generation = impl->slot_generation[i];
        stream->generation = slot->audio_generation;
        stream->frames_per_packet = slot->audio_frames_per_packet;
        count++;
    }
    return count;
}

static void audio_stream_reset(AudioStreamState *stream) {
    native_audio_input_pcm_destroy(&stream->pcm);
    memset(stream, 0, sizeof(*stream));
    stream->target.slot = -1;
    stream->sink.target.slot = -1;
}

static bool audio_stream_matches(const AudioStreamState *stream, const AudioDesiredStream *desired,
                                 uint32_t input_rate, uint16_t input_channels) {
    return stream->active && stream->pcm.impl && desired->active && stream->target.slot == desired->target.slot &&
           stream->target.session == desired->target.session &&
           stream->target.state_generation == desired->target.state_generation &&
           stream->generation == desired->generation && stream->frames_per_packet == desired->frames_per_packet &&
           stream->input_rate == input_rate && stream->input_channels == input_channels;
}

static void audio_stream_sync(NativeCaptureRedirectImpl *impl, AudioStreamState *stream,
                              const AudioDesiredStream *desired, uint32_t input_rate, uint16_t input_channels) {
    if (audio_stream_matches(stream, desired, input_rate, input_channels)) {
        return;
    }
    audio_stream_reset(stream);
    if (!desired->active) {
        return;
    }
    stream->sink.impl = impl;
    stream->sink.target = desired->target;
    stream->sink.generation = desired->generation;
    if (!native_audio_input_pcm_init(&stream->pcm, input_rate, input_channels, desired->frames_per_packet,
                                     impl->audio_gain_db, audio_packet, &stream->sink)) {
        return;
    }
    stream->active = true;
    stream->target = desired->target;
    stream->generation = desired->generation;
    stream->frames_per_packet = desired->frames_per_packet;
    stream->input_rate = input_rate;
    stream->input_channels = input_channels;
}

void *capture_audio_worker_main(void *context) {
    NativeCaptureRedirectImpl *impl = (NativeCaptureRedirectImpl *)context;
    NativeAudioInputCapture *capture = NULL;
    AudioStreamState streams[NATIVE_CAPTURE_REDIRECT_MAX_SLOTS] = {0};
    AudioDesiredStream desired[NATIVE_CAPTURE_REDIRECT_MAX_SLOTS];
    int16_t input[AUDIO_READ_FRAMES * 2u];
    /* Sized for the worst-case channel count, like `input`: the push consumer
     * reads frames * input_channels samples, and the mono reset on every
     * close path is an invariant this buffer must not depend on. */
    int16_t silence[AUDIO_READ_FRAMES * 2u] = {0};
    uint32_t input_rate = 48000u;
    uint16_t input_channels = 1u;
    uint64_t next_open_attempt_ms = 0;
    unsigned previous_consumer_count = 0;

    for (int i = 0; i < NATIVE_CAPTURE_REDIRECT_MAX_SLOTS; i++) {
        streams[i].target.slot = -1;
        streams[i].sink.target.slot = -1;
    }

    while (!atomic_load(&impl->stop)) {
        pthread_mutex_lock(&impl->lock);
        unsigned consumer_count = audio_collect_desired_locked(impl, desired);
        pthread_mutex_unlock(&impl->lock);

        if (consumer_count != previous_consumer_count) {
            clog(cLogLevelInfo, "microphone redirect consumers=%u", consumer_count);
            previous_consumer_count = consumer_count;
        }
        if (consumer_count == 0u) {
            native_audio_input_capture_close(capture);
            capture = NULL;
            input_rate = 48000u;
            input_channels = 1u;
            next_open_attempt_ms = 0;
            for (int i = 0; i < NATIVE_CAPTURE_REDIRECT_MAX_SLOTS; i++) {
                audio_stream_reset(&streams[i]);
            }
            native_sleep_ms(10u);
            continue;
        }

        uint64_t now_ms = native_monotonic_ms64();
        if (!capture && now_ms >= next_open_attempt_ms) {
            capture = native_audio_input_capture_open(impl->audio_device_id, 48000u, 1u);
            if (!capture) {
                capture = native_audio_input_capture_open(impl->audio_device_id, 48000u, 2u);
            }
            if (capture) {
                input_rate = native_audio_input_capture_rate(capture);
                input_channels = native_audio_input_capture_channels(capture);
            } else {
                input_rate = 48000u;
                input_channels = 1u;
                next_open_attempt_ms = now_ms + AUDIO_DEVICE_RETRY_MS;
            }
        }

        for (int i = 0; i < NATIVE_CAPTURE_REDIRECT_MAX_SLOTS; i++) {
            audio_stream_sync(impl, &streams[i], &desired[i], input_rate, input_channels);
        }

        if (capture) {
            long frames = native_audio_input_capture_read(capture, input, AUDIO_READ_FRAMES);
            if (frames < 0) {
                native_audio_input_capture_close(capture);
                capture = NULL;
                input_rate = 48000u;
                input_channels = 1u;
                next_open_attempt_ms = native_monotonic_ms64() + AUDIO_DEVICE_RETRY_MS;
            } else if (frames > 0) {
                for (int i = 0; i < NATIVE_CAPTURE_REDIRECT_MAX_SLOTS; i++) {
                    if (streams[i].active) {
                        (void)native_audio_input_pcm_push(&streams[i].pcm, input, (size_t)frames);
                    }
                }
            } else {
                /* ALSA is nonblocking so session changes and shutdown cannot hang
                 * behind a stalled USB device. */
                native_sleep_ms(2u);
            }
            continue;
        }

        /* Keep every negotiated RDPEAI stream paced while the microphone is missing
         * or temporarily unavailable, and retry the one shared device in the
         * background. */
        for (int i = 0; i < NATIVE_CAPTURE_REDIRECT_MAX_SLOTS; i++) {
            if (streams[i].active) {
                (void)native_audio_input_pcm_push(&streams[i].pcm, silence, AUDIO_READ_FRAMES);
            }
        }
        native_sleep_ms(10u);
    }

    native_audio_input_capture_close(capture);
    for (int i = 0; i < NATIVE_CAPTURE_REDIRECT_MAX_SLOTS; i++) {
        audio_stream_reset(&streams[i]);
    }
    return NULL;
}
