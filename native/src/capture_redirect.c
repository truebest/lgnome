#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

/* Capture redirect lifecycle and worker orchestration. Camera/audio device I/O
 * lives in their dedicated worker modules; per-slot protocol state lives in
 * capture_slots.c. */

#include "capture_redirect.h"

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "camera_v4l2.h"
#include "capture_redirect_internal.h"
#include "clog.h"

clog_define(g_native_log_capture_redirect, cLogLevelInfo, cLogFlags_Default, "capture.redirect", NULL);

#define CAPTURE_TEST_FAIL_ALLOC (1u << 0)
#define CAPTURE_TEST_FAIL_MUTEX (1u << 1)
#define CAPTURE_TEST_FAIL_CAMERA_THREAD (1u << 2)
#define CAPTURE_TEST_FAIL_AUDIO_THREAD (1u << 3)

#ifdef HELLOLG_CAPTURE_REDIRECT_TESTING
static atomic_uint g_capture_test_failures;

void native_capture_redirect_test_set_failures(unsigned failures) {
    atomic_store_explicit(&g_capture_test_failures, failures, memory_order_relaxed);
}

static bool capture_test_failure_enabled(unsigned failure) {
    return (atomic_load_explicit(&g_capture_test_failures, memory_order_relaxed) & failure) != 0;
}
#endif

static NativeCaptureRedirectImpl *capture_alloc_impl(void) {
#ifdef HELLOLG_CAPTURE_REDIRECT_TESTING
    if (capture_test_failure_enabled(CAPTURE_TEST_FAIL_ALLOC)) {
        return NULL;
    }
#endif
    return (NativeCaptureRedirectImpl *)calloc(1, sizeof(NativeCaptureRedirectImpl));
}

static int capture_mutex_init(pthread_mutex_t *lock) {
#ifdef HELLOLG_CAPTURE_REDIRECT_TESTING
    if (capture_test_failure_enabled(CAPTURE_TEST_FAIL_MUTEX)) {
        return EAGAIN;
    }
#endif
    return pthread_mutex_init(lock, NULL);
}

static int capture_thread_create(pthread_t *thread, void *(*entry)(void *), void *context,
                                 unsigned test_failure) {
#ifdef HELLOLG_CAPTURE_REDIRECT_TESTING
    if (capture_test_failure_enabled(test_failure)) {
        return EAGAIN;
    }
#else
    (void)test_failure;
#endif
    return pthread_create(thread, NULL, entry, context);
}

static bool capture_config_valid(const NativeCaptureRedirectConfig *config) {
    return config && config->camera_width >= NATIVE_CAMERA_MIN_WIDTH &&
           config->camera_width <= NATIVE_CAMERA_MAX_WIDTH && (config->camera_width & 1u) == 0u &&
           config->camera_height >= NATIVE_CAMERA_MIN_HEIGHT && config->camera_height <= NATIVE_CAMERA_MAX_HEIGHT &&
           (config->camera_height & 1u) == 0u && config->camera_fps > 0u &&
           config->camera_fps <= NATIVE_CAMERA_MAX_FPS && config->audio_input_gain_db >= -12 &&
           config->audio_input_gain_db <= 18;
}

static void capture_apply_config_locked(NativeCaptureRedirectImpl *impl, const NativeCaptureRedirectConfig *config) {
    impl->camera_enabled = config->camera_enabled;
    impl->camera_width = config->camera_width;
    impl->camera_height = config->camera_height;
    impl->camera_fps = config->camera_fps;
    impl->audio_enabled = config->audio_input_enabled;
    impl->audio_gain_db = config->audio_input_gain_db;
    snprintf(impl->camera_device_id, sizeof(impl->camera_device_id), "%s",
             config->camera_device_id ? config->camera_device_id : "");
    snprintf(impl->audio_device_id, sizeof(impl->audio_device_id), "%s",
             config->audio_input_device_id ? config->audio_input_device_id : "");
    for (int i = 0; i < NATIVE_CAPTURE_REDIRECT_MAX_SLOTS; i++) {
        impl->slot_generation[i]++;
    }
}

static bool capture_start_workers(NativeCaptureRedirectImpl *impl) {
    bool ok = true;
    if (impl->camera_enabled && !impl->camera_thread_running) {
        impl->camera_thread_running =
            capture_thread_create(&impl->camera_thread, capture_camera_worker_main, impl,
                                  CAPTURE_TEST_FAIL_CAMERA_THREAD) == 0;
        ok &= impl->camera_thread_running;
    }
    if (impl->audio_enabled && !impl->audio_thread_running) {
        impl->audio_thread_running =
            capture_thread_create(&impl->audio_thread, capture_audio_worker_main, impl,
                                  CAPTURE_TEST_FAIL_AUDIO_THREAD) == 0;
        ok &= impl->audio_thread_running;
    }
    return ok;
}

bool native_capture_redirect_init(NativeCaptureRedirect *redirect, const NativeCaptureRedirectConfig *config) {
    if (!redirect || !capture_config_valid(config)) {
        return false;
    }
    memset(redirect, 0, sizeof(*redirect));
    NativeCaptureRedirectImpl *impl = capture_alloc_impl();
    if (!impl) {
        return false;
    }
    impl->foreground_slot = -1;
    capture_apply_config_locked(impl, config);
    atomic_init(&impl->stop, false);
    if (capture_mutex_init(&impl->lock) != 0) {
        free(impl);
        return false;
    }
    impl->lock_initialized = true;
    redirect->impl = impl;
    bool workers_ready = capture_start_workers(impl);
    clog(cLogLevelNotice, "capture redirection ready (camera=%s microphone=%s)",
         impl->camera_thread_running ? "on" : "off", impl->audio_thread_running ? "on" : "off");
    if (!workers_ready) {
        clog(cLogLevelWarning, "one or more capture workers could not be started");
    }
    return workers_ready;
}

bool native_capture_redirect_reconfigure(NativeCaptureRedirect *redirect, const NativeCaptureRedirectConfig *config) {
    if (!redirect || !redirect->impl || !capture_config_valid(config)) {
        return false;
    }
    NativeCaptureRedirectImpl *impl = (NativeCaptureRedirectImpl *)redirect->impl;
    atomic_store(&impl->stop, true);
    if (impl->camera_thread_running) {
        pthread_join(impl->camera_thread, NULL);
        impl->camera_thread_running = false;
    }
    if (impl->audio_thread_running) {
        pthread_join(impl->audio_thread, NULL);
        impl->audio_thread_running = false;
    }
    /* The camera worker also withdraws availability on exit. Repeat it after
     * both joins so a partial worker-start failure has the same final state. */
    capture_camera_set_physical_available(impl, false);

    pthread_mutex_lock(&impl->lock);
    capture_apply_config_locked(impl, config);
    pthread_mutex_unlock(&impl->lock);
    atomic_store(&impl->stop, false);
    bool workers_ready = capture_start_workers(impl);
    clog(cLogLevelNotice, "capture redirection reconfigured (camera=%s microphone=%s)",
         impl->camera_thread_running ? "on" : "off", impl->audio_thread_running ? "on" : "off");
    if (!workers_ready) {
        clog(cLogLevelWarning, "one or more reconfigured capture workers could not be started");
    }
    return workers_ready;
}

void native_capture_redirect_destroy(NativeCaptureRedirect *redirect) {
    if (!redirect || !redirect->impl) {
        return;
    }
    NativeCaptureRedirectImpl *impl = (NativeCaptureRedirectImpl *)redirect->impl;
    atomic_store(&impl->stop, true);
    pthread_mutex_lock(&impl->lock);
    for (int i = 0; i < NATIVE_CAPTURE_REDIRECT_MAX_SLOTS; i++) {
        if (impl->slots[i].session) {
            capture_camera_fail_credit_locked(impl, i, impl->slots[i].camera_generation);
            rdp_set_capture_active(impl->slots[i].session, false);
        }
        if (impl->slots[i].session && impl->slots[i].camera_available_requested) {
            rdp_set_camera_available(impl->slots[i].session, false);
            impl->slots[i].camera_available_requested = false;
        }
        impl->slots[i].session = NULL;
    }
    pthread_mutex_unlock(&impl->lock);
    if (impl->camera_thread_running) {
        pthread_join(impl->camera_thread, NULL);
    }
    if (impl->audio_thread_running) {
        pthread_join(impl->audio_thread, NULL);
    }
    if (impl->lock_initialized) {
        pthread_mutex_destroy(&impl->lock);
    }
    free(impl);
    redirect->impl = NULL;
}
