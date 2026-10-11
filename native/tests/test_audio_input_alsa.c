#include "audio_input_alsa.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

struct snd_pcm_t {
    long result;
    unsigned recovery_calls;
};

static long read_frames(struct snd_pcm_t *pcm, void *buffer, unsigned long frames) {
    (void)buffer;
    (void)frames;
    return pcm->result;
}

static int recover_frames(struct snd_pcm_t *pcm, int error, int silent) {
    (void)error;
    (void)silent;
    pcm->recovery_calls++;
    return 0;
}

static const char *read_error(int error) {
    (void)error;
    return "test error";
}

static void test_suspend_reopens_instead_of_entering_resume_loop(void) {
    struct snd_pcm_t pcm = {.result = -ESTRPIPE};
    NativeAudioInputCapture *capture = native_audio_input_capture_create_for_test(
        &pcm, read_frames, recover_frames, read_error);
    assert(capture);
    int16_t samples[2];
    assert(native_audio_input_capture_read(capture, samples, 1u) == -1);
    assert(pcm.recovery_calls == 0u);
    pcm.result = -EPIPE;
    assert(native_audio_input_capture_read(capture, samples, 1u) == 0);
    assert(pcm.recovery_calls == 1u);
    pcm.result = -EAGAIN;
    assert(native_audio_input_capture_read(capture, samples, 1u) == 0);
    assert(pcm.recovery_calls == 1u);
    native_audio_input_capture_close(capture);
}

static void test_explicit_device_is_strict(void) {
    NativeAudioInputDeviceInfo devices[] = {
        {.stable_id = "alsa:ignored:0"},
    };
    char name[128];

    assert(native_audio_input_candidate_name_for_test(
        "alsa:B300:0", devices, 1u, 0u, name, sizeof(name)));
    assert(strcmp(name, "hw:CARD=B300,DEV=0") == 0);
    assert(!native_audio_input_candidate_name_for_test(
        "alsa:B300:0", devices, 1u, 1u, name, sizeof(name)));
    assert(!native_audio_input_candidate_name_for_test(
        "invalid", devices, 1u, 0u, name, sizeof(name)));
}

static void test_auto_falls_back_to_enumerated_devices(void) {
    NativeAudioInputDeviceInfo devices[] = {
        {.stable_id = "alsa:B300:0"},
        {.stable_id = "alsa:USB_Headset:2"},
    };
    char name[128];

    assert(native_audio_input_candidate_name_for_test(
        "", devices, 2u, 0u, name, sizeof(name)));
    assert(strcmp(name, "default") == 0);
    assert(native_audio_input_candidate_name_for_test(
        "", devices, 2u, 1u, name, sizeof(name)));
    assert(strcmp(name, "hw:CARD=B300,DEV=0") == 0);
    assert(native_audio_input_candidate_name_for_test(
        NULL, devices, 2u, 2u, name, sizeof(name)));
    assert(strcmp(name, "hw:CARD=USB_Headset,DEV=2") == 0);
    assert(!native_audio_input_candidate_name_for_test(
        "", devices, 2u, 3u, name, sizeof(name)));
}

int main(void) {
    test_suspend_reopens_instead_of_entering_resume_loop();
    test_explicit_device_is_strict();
    test_auto_falls_back_to_enumerated_devices();
    puts("PASS audio-input-alsa");
    return 0;
}
