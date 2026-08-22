#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

/* Native-H.264 camera discovery, capture, recovery, and bounded fan-out to the
 * foreground RDP session. Protocol demand and credit live in capture_slots.c. */

#include "capture_redirect_internal.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "camera_h264_stream.h"
#include "camera_v4l2.h"
#include "clog.h"
#include "native_time.h"

clog_define(g_native_log_capture_camera_worker, cLogLevelInfo, cLogFlags_Default, "capture.redirect", NULL);

#define CAMERA_READ_TIMEOUT_MS 100
#define CAMERA_TIMEOUT_LIMIT 20u
#define CAMERA_DEVICE_RETRY_MS 1000u
#define CAMERA_ENUM_LIMIT 32u

static bool camera_slot_eligible_locked(const NativeCaptureRedirectImpl *impl, int index) {
    const NativeCaptureSlot *slot = &impl->slots[index];
    return impl->camera_enabled && slot->session && slot->camera_allowed;
}

static bool capture_slot_is_foreground_locked(const NativeCaptureRedirectImpl *impl, int index) {
    return impl->foreground_slot == index;
}

static bool camera_stream_requested_locked(const NativeCaptureRedirectImpl *impl, int index) {
    const NativeCaptureSlot *slot = &impl->slots[index];
    return camera_slot_eligible_locked(impl, index) && capture_slot_is_foreground_locked(impl, index) &&
           slot->camera_streaming && slot->camera_width == impl->camera_width &&
           slot->camera_height == impl->camera_height && slot->camera_fps == impl->camera_fps;
}

static bool camera_any_eligible_locked(const NativeCaptureRedirectImpl *impl) {
    for (int i = 0; i < NATIVE_CAPTURE_REDIRECT_MAX_SLOTS; i++) {
        if (camera_slot_eligible_locked(impl, i)) {
            return true;
        }
    }
    return false;
}

static int camera_stream_target_locked(const NativeCaptureRedirectImpl *impl, uint64_t *generation,
                                       unsigned *state_generation) {
    for (int i = 0; i < NATIVE_CAPTURE_REDIRECT_MAX_SLOTS; i++) {
        if (camera_stream_requested_locked(impl, i)) {
            if (generation) {
                *generation = impl->slots[i].camera_generation;
            }
            if (state_generation) {
                *state_generation = impl->slot_generation[i];
            }
            return i;
        }
    }
    if (generation) {
        *generation = 0u;
    }
    if (state_generation) {
        *state_generation = 0u;
    }
    return -1;
}

void capture_update_camera_availability_locked(NativeCaptureRedirectImpl *impl) {
    /* Issue every old-device removal before any new foreground advertisement.
     * The per-session workers apply these requests asynchronously, so the
     * synchronous payload gates remain the privacy boundary; this ordering
     * minimizes any transient overlap in server-side device publication. */
    for (int i = 0; i < NATIVE_CAPTURE_REDIRECT_MAX_SLOTS; i++) {
        NativeCaptureSlot *slot = &impl->slots[i];
        bool available = impl->camera_physical_available && camera_slot_eligible_locked(impl, i) &&
                         capture_slot_is_foreground_locked(impl, i);
        if (!slot->camera_available_requested || available) {
            continue;
        }
        if (slot->session) {
            rdp_set_camera_available(slot->session, false);
        }
        slot->camera_available_requested = false;
    }
    for (int i = 0; i < NATIVE_CAPTURE_REDIRECT_MAX_SLOTS; i++) {
        NativeCaptureSlot *slot = &impl->slots[i];
        bool available = impl->camera_physical_available && camera_slot_eligible_locked(impl, i) &&
                         capture_slot_is_foreground_locked(impl, i);
        if (slot->camera_available_requested || !available) {
            continue;
        }
        if (slot->session) {
            rdp_set_camera_available(slot->session, true);
        }
        slot->camera_available_requested = true;
    }
}

static void camera_set_physical_available_locked(NativeCaptureRedirectImpl *impl, bool physical_available) {
    /* Once shutdown/reconfigure starts, an old worker iteration must not
     * resurrect the device after the control thread requested its removal. */
    if (physical_available && atomic_load(&impl->stop)) {
        physical_available = false;
    }
    impl->camera_physical_available = physical_available;
    capture_update_camera_availability_locked(impl);
}

void capture_camera_set_physical_available(NativeCaptureRedirectImpl *impl, bool physical_available) {
    pthread_mutex_lock(&impl->lock);
    camera_set_physical_available_locked(impl, physical_available);
    pthread_mutex_unlock(&impl->lock);
}

#ifdef HELLOLG_CAPTURE_REDIRECT_TESTING
void native_capture_redirect_test_set_camera_physical_available(NativeCaptureRedirect *redirect, bool available) {
    if (!redirect || !redirect->impl) {
        return;
    }
    capture_camera_set_physical_available((NativeCaptureRedirectImpl *)redirect->impl, available);
}
#endif

static void camera_close_resources(NativeCameraCapture **capture, NativeCameraH264Stream **stream) {
    native_camera_capture_close(*capture);
    native_camera_h264_stream_destroy(*stream);
    *capture = NULL;
    *stream = NULL;
}

static bool camera_device_present(const NativeCaptureRedirectImpl *impl) {
    NativeCameraDeviceInfo devices[CAMERA_ENUM_LIMIT];
    size_t count = native_camera_enumerate(devices, CAMERA_ENUM_LIMIT);
    bool automatic = impl->camera_device_id[0] == '\0';
    bool found = false;
    for (size_t i = 0; i < count; i++) {
        if (automatic || strcmp(devices[i].id, impl->camera_device_id) == 0) {
            found = true;
            break;
        }
    }
    return found && native_camera_h264_mode_available(impl->camera_device_id, impl->camera_width, impl->camera_height,
                                                      impl->camera_fps);
}

static bool camera_open_local(NativeCaptureRedirectImpl *impl, NativeCameraCapture **capture,
                              NativeCameraH264Stream **stream) {
    if (!native_camera_capture_open_h264(impl->camera_device_id, impl->camera_width, impl->camera_height,
                                         impl->camera_fps, capture)) {
        return false;
    }
    *stream = native_camera_h264_stream_create();
    if (!*stream) {
        native_camera_capture_close(*capture);
        *capture = NULL;
        return false;
    }
    return true;
}

static void camera_fail_credit(NativeCaptureRedirectImpl *impl, int slot, uint64_t generation) {
    if (!capture_slot_index_valid(slot)) {
        return;
    }
    pthread_mutex_lock(&impl->lock);
    capture_camera_fail_credit_locked(impl, slot, generation);
    pthread_mutex_unlock(&impl->lock);
}

static bool camera_submit_queued(NativeCaptureRedirectImpl *impl, int slot_index, uint64_t generation,
                                 unsigned state_generation, uint64_t owner_epoch, NativeCameraH264Stream *stream) {
    bool submission_failed = false;
    pthread_mutex_lock(&impl->lock);
    NativeCaptureSlot *slot = &impl->slots[slot_index];
    NativeCameraH264Packet packet;
    if (impl->camera_owner_epoch == owner_epoch && impl->slot_generation[slot_index] == state_generation &&
        camera_stream_requested_locked(impl, slot_index) && slot->camera_generation == generation &&
        slot->camera_credit > 0u &&
        native_camera_h264_stream_peek(stream, &packet)) {
        if (rdp_submit_camera_h264(slot->session, slot->camera_generation, packet.data, packet.len, packet.is_idr)) {
            slot->camera_credit--;
            native_camera_h264_stream_pop(stream);
        } else {
            capture_camera_fail_credit_locked(impl, slot_index, generation);
            submission_failed = true;
        }
    }
    pthread_mutex_unlock(&impl->lock);
    return !submission_failed;
}

void *capture_camera_worker_main(void *context) {
    NativeCaptureRedirectImpl *impl = (NativeCaptureRedirectImpl *)context;
    NativeCameraCapture *capture = NULL;
    NativeCameraH264Stream *stream = NULL;
    bool physical_available = false;
    uint64_t next_device_probe_ms = 0u;
    unsigned timeouts = 0u;
    unsigned previous_profile_count = 0u;
    int active_slot = -1;
    uint64_t active_generation = 0u;
    unsigned active_state_generation = 0u;
    uint64_t active_owner_epoch = 0u;

    while (!atomic_load(&impl->stop)) {
        pthread_mutex_lock(&impl->lock);
        bool have_eligible = camera_any_eligible_locked(impl);
        uint64_t target_generation = 0u;
        unsigned target_state_generation = 0u;
        int target_slot = camera_stream_target_locked(impl, &target_generation, &target_state_generation);
        uint64_t target_owner_epoch = impl->camera_owner_epoch;
        unsigned profile_count = 0u;
        for (int i = 0; i < NATIVE_CAPTURE_REDIRECT_MAX_SLOTS; i++) {
            profile_count += camera_slot_eligible_locked(impl, i) ? 1u : 0u;
        }
        bool target_changed = target_slot != active_slot || target_generation != active_generation ||
                              target_state_generation != active_state_generation ||
                              target_owner_epoch != active_owner_epoch;
        if (target_changed && capture_slot_index_valid(active_slot)) {
            capture_camera_fail_credit_locked(impl, active_slot, active_generation);
        }
        camera_set_physical_available_locked(impl, physical_available);
        pthread_mutex_unlock(&impl->lock);

        if (profile_count != previous_profile_count) {
            clog(cLogLevelInfo, "camera redirect consumers=%u", profile_count);
            previous_profile_count = profile_count;
        }
        if (target_changed) {
            camera_close_resources(&capture, &stream);
            active_slot = target_slot;
            active_generation = target_generation;
            active_state_generation = target_state_generation;
            active_owner_epoch = target_owner_epoch;
            timeouts = 0u;
        }
        if (!have_eligible) {
            camera_close_resources(&capture, &stream);
            active_slot = -1;
            active_generation = 0u;
            active_state_generation = target_state_generation;
            active_owner_epoch = target_owner_epoch;
            if (physical_available) {
                physical_available = false;
                capture_camera_set_physical_available(impl, false);
            }
            next_device_probe_ms = 0u;
            timeouts = 0u;
            native_sleep_ms(25u);
            continue;
        }

        uint64_t now_ms = native_monotonic_ms64();
        if ((!physical_available || !capture) && now_ms >= next_device_probe_ms) {
            bool present = camera_device_present(impl);
            if (present != physical_available) {
                physical_available = present;
                capture_camera_set_physical_available(impl, physical_available);
            }
            next_device_probe_ms = now_ms + CAMERA_DEVICE_RETRY_MS;
        }
        if (!physical_available) {
            camera_close_resources(&capture, &stream);
            native_sleep_ms(25u);
            continue;
        }
        if (!capture_slot_index_valid(active_slot)) {
            camera_close_resources(&capture, &stream);
            native_sleep_ms(25u);
            continue;
        }

        if (!capture && !camera_open_local(impl, &capture, &stream)) {
            camera_fail_credit(impl, active_slot, active_generation);
            physical_available = false;
            capture_camera_set_physical_available(impl, false);
            camera_close_resources(&capture, &stream);
            next_device_probe_ms = native_monotonic_ms64() + CAMERA_DEVICE_RETRY_MS;
            native_sleep_ms(25u);
            continue;
        }
        if (!camera_submit_queued(impl, active_slot, active_generation, active_state_generation, active_owner_epoch,
                                  stream)) {
            camera_close_resources(&capture, &stream);
            next_device_probe_ms = 0u;
            continue;
        }

        NativeCameraFrame frame;
        NativeCameraAcquireResult acquire_result = native_camera_capture_acquire(capture, CAMERA_READ_TIMEOUT_MS,
                                                                                 &frame);
        if (acquire_result == NATIVE_CAMERA_ACQUIRE_TIMEOUT) {
            if (++timeouts >= CAMERA_TIMEOUT_LIMIT) {
                clog(cLogLevelWarning, "native-H.264 camera stopped producing frames; reopening capture");
                camera_fail_credit(impl, active_slot, active_generation);
                camera_close_resources(&capture, &stream);
                next_device_probe_ms = 0u;
                timeouts = 0u;
            }
            continue;
        }
        if (acquire_result != NATIVE_CAMERA_ACQUIRE_FRAME) {
            camera_fail_credit(impl, active_slot, active_generation);
            camera_close_resources(&capture, &stream);
            next_device_probe_ms = 0u;
            timeouts = 0u;
            continue;
        }
        timeouts = 0u;

        NativeCameraH264IngestResult ingest_result = native_camera_h264_stream_ingest(stream, &frame);
        native_camera_capture_release(capture, &frame);
        if (ingest_result == NATIVE_CAMERA_H264_INGEST_RECOVER) {
            camera_fail_credit(impl, active_slot, active_generation);
            camera_close_resources(&capture, &stream);
            next_device_probe_ms = 0u;
            continue;
        }
        if (ingest_result == NATIVE_CAMERA_H264_INGEST_SAMPLE_ERROR) {
            camera_fail_credit(impl, active_slot, active_generation);
            /* The stream is gated on a decoder seed: either capture started
             * mid-GOP or a dropped-buffer gap broke the dependent chain. Asking
             * for an IDR now shortens the gap to about a frame. The stream only
             * reports this once per wait bound, so the request is self-limiting. */
            native_camera_capture_request_keyframe(capture);
        }

        if (!camera_submit_queued(impl, active_slot, active_generation, active_state_generation, active_owner_epoch,
                                  stream)) {
            camera_close_resources(&capture, &stream);
            next_device_probe_ms = 0u;
        }
    }

    pthread_mutex_lock(&impl->lock);
    capture_camera_fail_credit_locked(impl, active_slot, active_generation);
    camera_set_physical_available_locked(impl, false);
    pthread_mutex_unlock(&impl->lock);
    camera_close_resources(&capture, &stream);
    return NULL;
}
