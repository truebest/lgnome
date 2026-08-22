#ifndef GNOMECAST_CAPTURE_REDIRECT_H
#define GNOMECAST_CAPTURE_REDIRECT_H

#include <stdbool.h>
#include <stdint.h>

#include "rdp_ffi.h"

#define NATIVE_CAPTURE_REDIRECT_MAX_SLOTS 4

typedef struct NativeCaptureRedirectConfig {
    bool camera_enabled;
    const char *camera_device_id;
    uint16_t camera_width;
    uint16_t camera_height;
    uint16_t camera_fps;
    bool audio_input_enabled;
    const char *audio_input_device_id;
    int16_t audio_input_gain_db;
} NativeCaptureRedirectConfig;

typedef struct NativeCaptureRedirect {
    void *impl;
} NativeCaptureRedirect;

/* Returns false when the config is invalid (nothing is set up) or when an
 * enabled worker thread could not start (any worker that did start keeps
 * running; a later reconfigure retries). Capture is best-effort: callers
 * proceed with whatever capture did start (possibly none) rather than
 * failing the app. */
bool native_capture_redirect_init(NativeCaptureRedirect *redirect,
                                  const NativeCaptureRedirectConfig *config);
bool native_capture_redirect_reconfigure(NativeCaptureRedirect *redirect,
                                         const NativeCaptureRedirectConfig *config);
void native_capture_redirect_destroy(NativeCaptureRedirect *redirect);

/* Every opted-in connection negotiates capture capability. Only the foreground slot
 * advertises its RDPECAM device and transmits camera/microphone payloads; background
 * RDPEAI remains negotiated but silent. set_slot must be cleared before
 * rdp_session_stop frees the RdpSession handle. */
void native_capture_redirect_set_slot(NativeCaptureRedirect *redirect, int slot, RdpSession *session,
                                      bool camera_allowed, bool audio_input_allowed);

/* Selects the only slot allowed to transmit capture payloads. Passing -1 pauses all
 * outgoing capture and withdraws every RDPECAM device; RDPEAI stays negotiated. */
void native_capture_redirect_set_foreground_slot(NativeCaptureRedirect *redirect, int slot);

/* RDP worker callbacks. These only update demand/credit state; device I/O and
 * native-H.264 stream normalization remain on the redirect manager's dedicated threads. */
void native_capture_redirect_camera_start(NativeCaptureRedirect *redirect, int slot,
                                          uint64_t generation, uint16_t width,
                                          uint16_t height, uint16_t fps);
void native_capture_redirect_camera_stop(NativeCaptureRedirect *redirect, int slot,
                                         uint64_t generation);
void native_capture_redirect_camera_sample_request(NativeCaptureRedirect *redirect, int slot,
                                                   uint64_t generation);
void native_capture_redirect_audio_start(NativeCaptureRedirect *redirect, int slot,
                                         uint64_t generation, uint32_t sample_rate,
                                         uint16_t channels, uint32_t frames_per_packet);
void native_capture_redirect_audio_stop(NativeCaptureRedirect *redirect, int slot,
                                        uint64_t generation);

#endif
