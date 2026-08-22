#include "audio_input_alsa.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

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
    test_explicit_device_is_strict();
    test_auto_falls_back_to_enumerated_devices();
    puts("PASS audio-input-alsa");
    return 0;
}
