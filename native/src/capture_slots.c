/* The RDP-facing protocol surface of capture redirect: per-slot permission
 * publishing and the camera/microphone stream callbacks with their
 * generation-matched, credit-driven state. Workers in capture_redirect.c
 * consume this state under the shared lock. */

#include "capture_redirect_internal.h"

#include "audio_input_pcm.h"
#include "rdp_ffi.h"

#include "clog.h"

clog_define(g_native_log_capture_slots, cLogLevelInfo, cLogFlags_Default, "capture.redirect", NULL);

void capture_camera_fail_credit_locked(NativeCaptureRedirectImpl *impl, int slot, uint64_t generation) {
    if (!impl || !capture_slot_index_valid(slot)) {
        return;
    }
    NativeCaptureSlot *state = &impl->slots[slot];
    if (!state->camera_streaming || state->camera_generation != generation || state->camera_credit == 0u) {
        return;
    }
    if (!state->session || !rdp_submit_camera_error(state->session, state->camera_generation)) {
        clog_limited(cLogLevelDebug, 4, 5000,
                     "camera SampleError for slot %d generation %llu could not enter the closing mailbox", slot,
                     (unsigned long long)generation);
    }
    state->camera_credit = 0u;
}

void native_capture_redirect_set_foreground_slot(NativeCaptureRedirect *redirect, int slot) {
    if (!redirect || !redirect->impl || (slot != -1 && !capture_slot_index_valid(slot))) {
        return;
    }
    NativeCaptureRedirectImpl *impl = (NativeCaptureRedirectImpl *)redirect->impl;
    pthread_mutex_lock(&impl->lock);
    if (impl->foreground_slot == slot) {
        pthread_mutex_unlock(&impl->lock);
        return;
    }
    /* Close every old mailbox before publishing/activating the new owner. Capture
     * producers use this same lock, so after each Rust setter returns no further C
     * submission can enter that background session's queue. */
    for (int i = 0; i < NATIVE_CAPTURE_REDIRECT_MAX_SLOTS; i++) {
        if (i != slot && impl->slots[i].session) {
            capture_camera_fail_credit_locked(impl, i, impl->slots[i].camera_generation);
            rdp_set_capture_active(impl->slots[i].session, false);
        }
    }
    impl->foreground_slot = slot;
    /* A worker can be blocked in V4L2 while ownership moves away and back to
     * the same slot. Preserve every transition so it cannot reuse the old
     * encoder reference chain after Rust closed that slot's capture gate. */
    impl->camera_owner_epoch++;
    if (capture_slot_index_valid(slot)) {
        NativeCaptureSlot *foreground = &impl->slots[slot];
        if (foreground->session) {
            rdp_set_capture_active(foreground->session, true);
        }
    }
    /* The old gates are closed and the new gate is open before RDPECAM
     * DeviceRemoved/DeviceAdded is requested for either session. */
    capture_update_camera_availability_locked(impl);
    pthread_mutex_unlock(&impl->lock);
    if (slot >= 0) {
        clog(cLogLevelInfo, "outgoing capture foreground slot=%d", slot);
    } else {
        clog(cLogLevelInfo, "outgoing capture paused");
    }
}

void native_capture_redirect_set_slot(NativeCaptureRedirect *redirect, int slot, RdpSession *session,
                                      bool camera_allowed, bool audio_input_allowed) {
    if (!redirect || !redirect->impl || !capture_slot_index_valid(slot)) {
        return;
    }
    NativeCaptureRedirectImpl *impl = (NativeCaptureRedirectImpl *)redirect->impl;
    pthread_mutex_lock(&impl->lock);
    NativeCaptureSlot *state = &impl->slots[slot];
    bool session_changed = state->session != session;
    if (state->session && session_changed) {
        capture_camera_fail_credit_locked(impl, slot, state->camera_generation);
        rdp_set_capture_active(state->session, false);
        if (state->camera_available_requested) {
            rdp_set_camera_available(state->session, false);
            state->camera_available_requested = false;
        }
    }
    if (session_changed || !camera_allowed) {
        capture_camera_fail_credit_locked(impl, slot, state->camera_generation);
        state->camera_streaming = false;
        state->camera_credit = 0u;
    }
    if (session_changed || !audio_input_allowed) {
        state->audio_streaming = false;
        state->audio_frames_per_packet = 0u;
    }
    state->session = session;
    state->camera_allowed = camera_allowed;
    state->audio_allowed = audio_input_allowed;
    /* This generation identifies the RDP session object, not a publication.
     * Profile saves republish every unchanged slot; advancing it there would
     * make both workers discard healthy live device state. Permission changes
     * already reconcile their individual stream/eligibility state above. */
    if (session_changed) {
        impl->slot_generation[slot]++;
    }
    if (state->session) {
        /* A newly connected current slot starts with Rust capture closed, while a
         * replacement/background slot must stay closed even if its DVCs negotiate. */
        rdp_set_capture_active(state->session, impl->foreground_slot == slot);
    }
    capture_update_camera_availability_locked(impl);
    pthread_mutex_unlock(&impl->lock);
}

void native_capture_redirect_camera_start(NativeCaptureRedirect *redirect, int slot, uint64_t generation,
                                          uint16_t width, uint16_t height, uint16_t fps) {
    if (!redirect || !redirect->impl || !capture_slot_index_valid(slot)) {
        return;
    }
    NativeCaptureRedirectImpl *impl = (NativeCaptureRedirectImpl *)redirect->impl;
    pthread_mutex_lock(&impl->lock);
    NativeCaptureSlot *state = &impl->slots[slot];
    capture_camera_fail_credit_locked(impl, slot, state->camera_generation);
    if (!state->camera_allowed) {
        clog_limited(cLogLevelWarning, 4, 5000, "camera start for slot %d without permission; ignoring until set_slot allows it", slot);
    }
    state->camera_streaming = true;
    state->camera_generation = generation;
    state->camera_width = width;
    state->camera_height = height;
    state->camera_fps = fps;
    state->camera_credit = 0u;
    pthread_mutex_unlock(&impl->lock);
}

void native_capture_redirect_camera_stop(NativeCaptureRedirect *redirect, int slot, uint64_t generation) {
    if (!redirect || !redirect->impl || !capture_slot_index_valid(slot)) {
        return;
    }
    NativeCaptureRedirectImpl *impl = (NativeCaptureRedirectImpl *)redirect->impl;
    pthread_mutex_lock(&impl->lock);
    NativeCaptureSlot *state = &impl->slots[slot];
    if (state->camera_generation == generation) {
        state->camera_streaming = false;
        state->camera_credit = 0u;
    }
    pthread_mutex_unlock(&impl->lock);
}

void native_capture_redirect_camera_sample_request(NativeCaptureRedirect *redirect, int slot, uint64_t generation) {
    if (!redirect || !redirect->impl || !capture_slot_index_valid(slot)) {
        return;
    }
    NativeCaptureRedirectImpl *impl = (NativeCaptureRedirectImpl *)redirect->impl;
    pthread_mutex_lock(&impl->lock);
    NativeCaptureSlot *state = &impl->slots[slot];
    if (state->camera_streaming && state->camera_generation == generation && state->camera_credit < 1u) {
        state->camera_credit++;
    }
    pthread_mutex_unlock(&impl->lock);
}

void native_capture_redirect_audio_start(NativeCaptureRedirect *redirect, int slot, uint64_t generation,
                                         uint32_t sample_rate, uint16_t channels, uint32_t frames_per_packet) {
    if (!redirect || !redirect->impl || !capture_slot_index_valid(slot) || sample_rate != NATIVE_AUDIO_INPUT_WIRE_RATE ||
        channels != NATIVE_AUDIO_INPUT_WIRE_CHANNELS || frames_per_packet == 0u || frames_per_packet > 4096u) {
        return;
    }
    NativeCaptureRedirectImpl *impl = (NativeCaptureRedirectImpl *)redirect->impl;
    pthread_mutex_lock(&impl->lock);
    NativeCaptureSlot *state = &impl->slots[slot];
    state->audio_streaming = true;
    state->audio_generation = generation;
    state->audio_frames_per_packet = frames_per_packet;
    pthread_mutex_unlock(&impl->lock);
}

void native_capture_redirect_audio_stop(NativeCaptureRedirect *redirect, int slot, uint64_t generation) {
    if (!redirect || !redirect->impl || !capture_slot_index_valid(slot)) {
        return;
    }
    NativeCaptureRedirectImpl *impl = (NativeCaptureRedirectImpl *)redirect->impl;
    pthread_mutex_lock(&impl->lock);
    NativeCaptureSlot *state = &impl->slots[slot];
    if (state->audio_generation == generation) {
        state->audio_streaming = false;
        state->audio_frames_per_packet = 0u;
    }
    pthread_mutex_unlock(&impl->lock);
}
