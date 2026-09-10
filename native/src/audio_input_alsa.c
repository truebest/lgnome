#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "audio_input_alsa.h"

#include <dlfcn.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "clog.h"

clog_define(g_native_log_audio_input_alsa, cLogLevelInfo, "audio.input.alsa");

typedef struct snd_pcm_t snd_pcm_t;
typedef long snd_pcm_sframes_t;
typedef unsigned long snd_pcm_uframes_t;

enum {
    NATIVE_SND_PCM_STREAM_CAPTURE = 1,
    NATIVE_SND_PCM_NONBLOCK = 1,
    NATIVE_SND_PCM_ACCESS_RW_INTERLEAVED = 3,
    NATIVE_SND_PCM_FORMAT_S16_LE = 2,
    NATIVE_AUDIO_INPUT_MAX_CARDS = 32,
    NATIVE_AUDIO_INPUT_MAX_DEVICES = 32,
    NATIVE_AUDIO_INPUT_AUTO_CANDIDATES = 32
};

typedef int (*snd_pcm_open_fn)(snd_pcm_t **pcm, const char *name, int stream, int mode);
typedef int (*snd_pcm_close_fn)(snd_pcm_t *pcm);
typedef int (*snd_pcm_set_params_fn)(snd_pcm_t *pcm, int format, int access, unsigned channels,
                                     unsigned rate, int soft_resample, unsigned latency);
typedef int (*snd_pcm_start_fn)(snd_pcm_t *pcm);
typedef snd_pcm_sframes_t (*snd_pcm_readi_fn)(snd_pcm_t *pcm, void *buffer,
                                             snd_pcm_uframes_t size);
typedef int (*snd_pcm_recover_fn)(snd_pcm_t *pcm, int err, int silent);
typedef int (*snd_pcm_drop_fn)(snd_pcm_t *pcm);
typedef const char *(*snd_strerror_fn)(int errnum);

typedef struct NativeAlsaApi {
    void *library;
    snd_pcm_open_fn pcm_open;
    snd_pcm_close_fn pcm_close;
    snd_pcm_set_params_fn pcm_set_params;
    snd_pcm_start_fn pcm_start;
    snd_pcm_readi_fn pcm_readi;
    snd_pcm_recover_fn pcm_recover;
    snd_pcm_drop_fn pcm_drop;
    snd_strerror_fn strerror_fn;
} NativeAlsaApi;

struct NativeAudioInputCapture {
    NativeAlsaApi api;
    snd_pcm_t *pcm;
    uint32_t sample_rate;
    uint16_t channels;
};

static bool load_symbol(void *library, const char *name, void *target, size_t target_size) {
    void *symbol = dlsym(library, name);
    if (!symbol || target_size != sizeof(symbol)) {
        return false;
    }
    memcpy(target, &symbol, sizeof(symbol));
    return true;
}

static bool alsa_load(NativeAlsaApi *api) {
    memset(api, 0, sizeof(*api));
    const char *names[] = {"libasound.so.2", "libasound.so"};
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        api->library = dlopen(names[i], RTLD_NOW | RTLD_LOCAL);
        if (api->library) {
            break;
        }
    }
    if (!api->library) {
        clog(cLogLevelWarning, "ALSA library is unavailable; microphone redirection stays silent");
        return false;
    }
#define LOAD_ALSA(member, symbol)                                                                        \
    do {                                                                                                 \
        if (!load_symbol(api->library, symbol, &api->member, sizeof(api->member))) {                     \
            clog(cLogLevelError, "ALSA symbol %s is unavailable", symbol);                              \
            dlclose(api->library);                                                                       \
            memset(api, 0, sizeof(*api));                                                                \
            return false;                                                                                \
        }                                                                                                \
    } while (0)
    LOAD_ALSA(pcm_open, "snd_pcm_open");
    LOAD_ALSA(pcm_close, "snd_pcm_close");
    LOAD_ALSA(pcm_set_params, "snd_pcm_set_params");
    LOAD_ALSA(pcm_start, "snd_pcm_start");
    LOAD_ALSA(pcm_readi, "snd_pcm_readi");
    LOAD_ALSA(pcm_recover, "snd_pcm_recover");
    LOAD_ALSA(pcm_drop, "snd_pcm_drop");
    LOAD_ALSA(strerror_fn, "snd_strerror");
#undef LOAD_ALSA
    return true;
}

static void trim_line(char *text) {
    size_t len = strlen(text);
    while (len > 0u && (text[len - 1u] == '\n' || text[len - 1u] == '\r' || text[len - 1u] == ' ')) {
        text[--len] = '\0';
    }
}

static bool read_text_file(const char *path, char *out, size_t capacity) {
    if (!out || capacity == 0u) {
        return false;
    }
    FILE *file = fopen(path, "r");
    if (!file) {
        return false;
    }
    size_t length = fread(out, 1u, capacity - 1u, file);
    out[length] = '\0';
    bool ok = length > 0u;
    fclose(file);
    if (ok) {
        trim_line(out);
    }
    return ok && out[0] != '\0';
}

size_t native_audio_input_enumerate(NativeAudioInputDeviceInfo *devices, size_t capacity) {
    size_t count = 0;
    for (unsigned card = 0; card < NATIVE_AUDIO_INPUT_MAX_CARDS; card++) {
        char path[128];
        char card_id[128];
        snprintf(path, sizeof(path), "/proc/asound/card%u/id", card);
        if (!read_text_file(path, card_id, sizeof(card_id))) {
            continue;
        }
        for (unsigned device = 0; device < NATIVE_AUDIO_INPUT_MAX_DEVICES; device++) {
            char info[512];
            snprintf(path, sizeof(path), "/proc/asound/card%u/pcm%uc/info", card, device);
            if (!read_text_file(path, info, sizeof(info))) {
                continue;
            }
            char name[256] = {0};
            char *saveptr = NULL;
            char *line = strtok_r(info, "\n", &saveptr);
            while (line) {
                if (strncmp(line, "name: ", 6) == 0) {
                    snprintf(name, sizeof(name), "%s", line + 6);
                    break;
                }
                line = strtok_r(NULL, "\n", &saveptr);
            }
            if (count >= capacity || !devices) {
                continue;
            }
            snprintf(devices[count].stable_id, sizeof(devices[count].stable_id), "alsa:%s:%u",
                     card_id, device);
            snprintf(devices[count].name, sizeof(devices[count].name),
                     "%.176s (%.64s:%u)", name[0] ? name : card_id, card_id,
                     device);
            count++;
        }
    }
    clog(cLogLevelDebug, "enumerated %zu ALSA capture endpoints", count);
    return count;
}

static bool stable_id_to_alsa_name(const char *stable_id, char *name, size_t capacity) {
    if (strncmp(stable_id, "alsa:", 5) != 0) {
        return false;
    }
    const char *card = stable_id + 5;
    const char *separator = strrchr(card, ':');
    if (!separator || separator == card || separator[1] == '\0') {
        return false;
    }
    char *end = NULL;
    errno = 0;
    unsigned long device = strtoul(separator + 1, &end, 10);
    if (errno != 0 || !end || *end != '\0' || device > 255u) {
        return false;
    }
    int card_len = (int)(separator - card);
    int written = snprintf(name, capacity, "hw:CARD=%.*s,DEV=%lu", card_len, card, device);
    return written > 0 && (size_t)written < capacity;
}

static bool audio_input_candidate_name(const char *stable_id,
                                       const NativeAudioInputDeviceInfo *devices,
                                       size_t device_count, size_t index,
                                       char *name, size_t capacity) {
    bool auto_select = !stable_id || !stable_id[0];
    if (!auto_select) {
        return index == 0u && stable_id_to_alsa_name(stable_id, name, capacity);
    }
    if (index == 0u) {
        int written = snprintf(name, capacity, "default");
        return written > 0 && (size_t)written < capacity;
    }
    index--;
    if (!devices || index >= device_count) {
        return false;
    }
    return stable_id_to_alsa_name(devices[index].stable_id, name, capacity);
}

#ifdef LGNOME_AUDIO_INPUT_ALSA_TESTING
bool native_audio_input_candidate_name_for_test(const char *stable_id,
                                                const NativeAudioInputDeviceInfo *devices,
                                                size_t device_count, size_t index,
                                                char *name, size_t capacity) {
    return audio_input_candidate_name(stable_id, devices, device_count, index, name, capacity);
}
#endif

static NativeAudioInputCapture *audio_input_capture_open_device(const char *device_name,
                                                                uint32_t sample_rate,
                                                                uint16_t channels) {
    NativeAudioInputCapture *capture =
        (NativeAudioInputCapture *)calloc(1, sizeof(NativeAudioInputCapture));
    if (!capture || !alsa_load(&capture->api)) {
        free(capture);
        return NULL;
    }
    int result = capture->api.pcm_open(&capture->pcm, device_name,
                                       NATIVE_SND_PCM_STREAM_CAPTURE,
                                       NATIVE_SND_PCM_NONBLOCK);
    if (result < 0) {
        clog(cLogLevelWarning, "cannot open ALSA capture %s: %s", device_name,
             capture->api.strerror_fn(result));
        native_audio_input_capture_close(capture);
        return NULL;
    }
    result = capture->api.pcm_set_params(capture->pcm, NATIVE_SND_PCM_FORMAT_S16_LE,
                                         NATIVE_SND_PCM_ACCESS_RW_INTERLEAVED, channels, sample_rate,
                                         1, 50000u);
    if (result < 0) {
        clog(cLogLevelWarning, "cannot configure ALSA capture %s at %u Hz/%u ch: %s", device_name,
             sample_rate, channels, capture->api.strerror_fn(result));
        native_audio_input_capture_close(capture);
        return NULL;
    }
    result = capture->api.pcm_start(capture->pcm);
    if (result < 0) {
        clog(cLogLevelWarning, "cannot start ALSA capture %s: %s",
             device_name, capture->api.strerror_fn(result));
        native_audio_input_capture_close(capture);
        return NULL;
    }
    capture->sample_rate = sample_rate;
    capture->channels = channels;
    clog(cLogLevelNotice, "opened ALSA capture %s at %u Hz/%u ch", device_name, sample_rate, channels);
    return capture;
}

NativeAudioInputCapture *native_audio_input_capture_open(const char *stable_id, uint32_t sample_rate,
                                                         uint16_t channels) {
    if (sample_rate < 8000u || sample_rate > 192000u || (channels != 1u && channels != 2u)) {
        return NULL;
    }

    bool auto_select = !stable_id || !stable_id[0];
    NativeAudioInputDeviceInfo devices[NATIVE_AUDIO_INPUT_AUTO_CANDIDATES];
    size_t device_count = auto_select
                              ? native_audio_input_enumerate(devices, NATIVE_AUDIO_INPUT_AUTO_CANDIDATES)
                              : 0u;
    size_t candidate_count = auto_select ? device_count + 1u : 1u;
    for (size_t i = 0; i < candidate_count; i++) {
        char device_name[384];
        if (!audio_input_candidate_name(stable_id, devices, device_count, i,
                                        device_name, sizeof(device_name))) {
            if (!auto_select) {
                clog(cLogLevelError, "invalid ALSA stable device id");
            }
            continue;
        }
        NativeAudioInputCapture *capture =
            audio_input_capture_open_device(device_name, sample_rate, channels);
        if (capture) {
            return capture;
        }
    }
    return NULL;
}

void native_audio_input_capture_close(NativeAudioInputCapture *capture) {
    if (!capture) {
        return;
    }
    if (capture->pcm && capture->api.pcm_drop) {
        capture->api.pcm_drop(capture->pcm);
    }
    if (capture->pcm && capture->api.pcm_close) {
        capture->api.pcm_close(capture->pcm);
    }
    if (capture->api.library) {
        dlclose(capture->api.library);
    }
    free(capture);
}

long native_audio_input_capture_read(NativeAudioInputCapture *capture, int16_t *samples, size_t frames) {
    if (!capture || !capture->pcm || !samples || frames == 0u) {
        return -1;
    }
    snd_pcm_sframes_t result = capture->api.pcm_readi(capture->pcm, samples, frames);
    if (result >= 0) {
        return (long)result;
    }
    if (result == -EAGAIN) {
        return 0;
    }
    int recovered = capture->api.pcm_recover(capture->pcm, (int)result, 1);
    if (recovered >= 0) {
        clog_limited(cLogLevelWarning, 2, 1000u, "recovered ALSA microphone xrun (%ld)", (long)result);
        return 0;
    }
    clog_limited(cLogLevelError, 2, 1000u, "terminal ALSA microphone read failure: %s",
                 capture->api.strerror_fn(recovered));
    return -1;
}

uint32_t native_audio_input_capture_rate(const NativeAudioInputCapture *capture) {
    return capture ? capture->sample_rate : 0u;
}

uint16_t native_audio_input_capture_channels(const NativeAudioInputCapture *capture) {
    return capture ? capture->channels : 0u;
}
