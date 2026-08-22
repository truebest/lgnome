#ifndef GNOMECAST_CAPTURE_REDIRECT_INTERNAL_H
#define GNOMECAST_CAPTURE_REDIRECT_INTERNAL_H

/* Shared state between the capture-redirect lifecycle, camera/audio workers,
 * and the RDP protocol surface. Everything here is guarded by
 * NativeCaptureRedirectImpl.lock. */

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#include "camera_v4l2.h"
#include "capture_redirect.h"
#include "rdp_ffi.h"

typedef struct NativeCaptureSlot {
    RdpSession *session;
    bool camera_allowed;
    bool audio_allowed;
    bool camera_streaming;
    uint64_t camera_generation;
    uint16_t camera_width;
    uint16_t camera_height;
    uint16_t camera_fps;
    unsigned camera_credit;
    bool camera_available_requested;
    bool audio_streaming;
    uint64_t audio_generation;
    uint32_t audio_frames_per_packet;
} NativeCaptureSlot;

typedef struct NativeCaptureRedirectImpl {
    pthread_mutex_t lock;
    bool lock_initialized;
    atomic_bool stop;
    unsigned slot_generation[NATIVE_CAPTURE_REDIRECT_MAX_SLOTS];
    NativeCaptureSlot slots[NATIVE_CAPTURE_REDIRECT_MAX_SLOTS];
    int foreground_slot;
    uint64_t camera_owner_epoch;
    bool camera_physical_available;

    bool camera_enabled;
    char camera_device_id[NATIVE_CAMERA_DEVICE_ID_MAX];
    uint16_t camera_width;
    uint16_t camera_height;
    uint16_t camera_fps;
    bool audio_enabled;
    char audio_device_id[512];
    int16_t audio_gain_db;

    pthread_t camera_thread;
    pthread_t audio_thread;
    bool camera_thread_running;
    bool audio_thread_running;
} NativeCaptureRedirectImpl;

static inline bool capture_slot_index_valid(int slot) {
    return slot >= 0 && slot < NATIVE_CAPTURE_REDIRECT_MAX_SLOTS;
}

/* Reconciles each session's requested RDPECAM availability with the physical
 * device, permission, and foreground owner. The caller holds impl->lock. */
void capture_update_camera_availability_locked(NativeCaptureRedirectImpl *impl);
void capture_camera_fail_credit_locked(NativeCaptureRedirectImpl *impl, int slot,
                                       uint64_t generation);

/* Worker entry points and the camera availability control used by lifecycle
 * reconfiguration. These are private to the capture redirect subsystem. */
void *capture_camera_worker_main(void *context);
void *capture_audio_worker_main(void *context);
void capture_camera_set_physical_available(NativeCaptureRedirectImpl *impl, bool available);

#endif
