#include <assert.h>
#include <stdio.h>

#include "native_app.h"
#include "native_rdp_callback_parts.h"
#include "native_rdp_media.h"

static unsigned opens, closes, received;
static bool sink_available = true;

NativeMedia *native_media_open(const char *sdk) {
    (void)sdk;
    return (NativeMedia *)(uintptr_t)1;
}

NativeAudio *native_audio_open(NativeMedia *media, uint32_t codec, uint32_t rate, uint16_t channels) {
    assert(media && codec == RDP_AUDIO_CODEC_PCM_S16LE && rate == 48000 && channels == 2);
    opens++;
    return sink_available ? (NativeAudio *)(uintptr_t)1 : NULL;
}

void native_video_close(NativeVideo *video) {
    assert(video);
    closes++;
}

bool native_audio_pipeline_set_source_format(NativeAudioPipeline *pipeline, int source,
                                             uint32_t rate, uint16_t channels) {
    (void)pipeline;
    assert(source == 0 && rate == 48000 && channels == 2);
    return true;
}

void native_audio_pipeline_close_source(NativeAudioPipeline *pipeline, int source) {
    (void)pipeline;
    (void)source;
}

size_t native_audio_pipeline_push(NativeAudioPipeline *pipeline, int source, const int16_t *samples,
                                  size_t frames, uint32_t timestamp_ms) {
    (void)pipeline;
    (void)samples;
    (void)timestamp_ms;
    assert(source == 0);
    received += frames;
    return 0;
}

const char *native_session_slot_name(int index) {
    assert(index == 0);
    return "red";
}

int main(void) {
    App app = {0};
    NativeSessionSlot *slot = &app.sessions[0];
    slot->app = &app;
    assert(pthread_mutex_init(&app.video_lock, NULL) == 0);
    assert(pthread_mutex_init(&app.audio_lock, NULL) == 0);
    RdpCallbacks callbacks = {.ctx = slot};
    native_rdp_install_audio_callbacks(&callbacks);
    app.video = (NativeVideo *)(uintptr_t)1;

    /* A stalled video callback holds this lock while network audio keeps arriving. */
    pthread_mutex_lock(&app.video_lock);
    callbacks.on_audio_format(slot, RDP_AUDIO_CODEC_PCM_S16LE, 48000, 2);
    const int16_t samples[4] = {0};
    callbacks.on_audio_data(slot, (const uint8_t *)samples, sizeof(samples), 0);
    assert(opens == 0 && received == 2 && atomic_load(&app.audio_open_requested));
    pthread_mutex_unlock(&app.video_lock);

    native_audio_media_tick(&app);
    assert(opens == 1 && closes == 1 && app.audio && !app.video);
    assert(atomic_load(&app.video_refresh_needed) && !atomic_load(&app.audio_open_requested));
    native_audio_media_tick(&app);
    assert(opens == 1);
    callbacks.on_audio_format(slot, RDP_AUDIO_CODEC_PCM_S16LE, 48000, 2);
    native_audio_media_tick(&app);
    assert(opens == 1);

    app.audio = NULL;
    app.video = (NativeVideo *)(uintptr_t)1;
    sink_available = false;
    callbacks.on_audio_format(slot, RDP_AUDIO_CODEC_PCM_S16LE, 48000, 2);
    native_audio_media_tick(&app);
    assert(opens == 2 && !app.audio && app.video && closes == 1 && slot->audio_routed);
    assert(pthread_mutex_destroy(&app.audio_lock) == 0);
    assert(pthread_mutex_destroy(&app.video_lock) == 0);
    puts("PASS native-audio-dispatch");
    return 0;
}
