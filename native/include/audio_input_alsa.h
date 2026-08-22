#ifndef GNOMECAST_AUDIO_INPUT_ALSA_H
#define GNOMECAST_AUDIO_INPUT_ALSA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct NativeAudioInputDeviceInfo {
    char stable_id[256];
    char name[256];
} NativeAudioInputDeviceInfo;

typedef struct NativeAudioInputCapture NativeAudioInputCapture;

/* Empty stable_id means Auto. Explicit ids never silently fall back to another device. */
/* Returns the number of entries WRITTEN (never more than capacity). */
size_t native_audio_input_enumerate(NativeAudioInputDeviceInfo *devices, size_t capacity);
NativeAudioInputCapture *native_audio_input_capture_open(const char *stable_id, uint32_t sample_rate,
                                                         uint16_t channels);
void native_audio_input_capture_close(NativeAudioInputCapture *capture);
/* Returns captured frames, zero on timeout/recoverable xrun, and -1 on terminal failure. */
long native_audio_input_capture_read(NativeAudioInputCapture *capture, int16_t *samples, size_t frames);
uint32_t native_audio_input_capture_rate(const NativeAudioInputCapture *capture);
uint16_t native_audio_input_capture_channels(const NativeAudioInputCapture *capture);

#ifdef HELLOLG_AUDIO_INPUT_ALSA_TESTING
bool native_audio_input_candidate_name_for_test(const char *stable_id,
                                                const NativeAudioInputDeviceInfo *devices,
                                                size_t device_count, size_t index,
                                                char *name, size_t capacity);
#endif

#endif
