#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "capture_redirect.h"

#include "native_app.h"
#include "native_capture_glue.h"
#include "native_time.h"

#include <assert.h>
#include <limits.h>
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#include "audio_input_alsa.h"
#include "camera_h264_stream.h"
#include "camera_v4l2.h"
#include "h264_annexb.h"

struct RdpSession {
    int slot;
    atomic_bool capture_active;
    atomic_bool camera_available;
};

struct NativeCameraCapture {
    uint32_t sequence;
    uint32_t seed_delay;
    bool first_frame;
    bool seeded;
};

struct NativeAudioInputCapture {
    int unused;
};

static atomic_uint g_camera_added[NATIVE_CAPTURE_REDIRECT_MAX_SLOTS];
static atomic_uint g_camera_removed[NATIVE_CAPTURE_REDIRECT_MAX_SLOTS];
static atomic_uint g_camera_packets[NATIVE_CAPTURE_REDIRECT_MAX_SLOTS];
static atomic_uint g_camera_errors[NATIVE_CAPTURE_REDIRECT_MAX_SLOTS];
static atomic_uint g_camera_open_count;
static atomic_uint g_camera_close_count;
static atomic_uint g_camera_submit_failures;
static atomic_uint g_camera_acquire_recoveries;
static atomic_uint g_camera_next_seed_delay;
static atomic_bool g_camera_hold_delta;
static atomic_uint g_camera_delta_acquires;
static atomic_bool g_camera_present = ATOMIC_VAR_INIT(true);
static atomic_uint g_audio_packets[NATIVE_CAPTURE_REDIRECT_MAX_SLOTS];
static atomic_uint g_audio_open_count;
static atomic_uint g_audio_close_count;
static struct NativeCameraCapture g_camera_capture;
static struct NativeAudioInputCapture g_audio_capture;
static App *g_handoff_probe_app;
static int g_handoff_old_slot = -1;
static int g_handoff_new_slot = -1;
static unsigned g_handoff_sequence;
static unsigned g_handoff_pause_sequence;
static unsigned g_handoff_activate_sequence;
static atomic_bool g_record_transitions;
static atomic_uint g_transition_sequence;
static atomic_uint g_capture_closed_sequence[NATIVE_CAPTURE_REDIRECT_MAX_SLOTS];
static atomic_uint g_capture_opened_sequence[NATIVE_CAPTURE_REDIRECT_MAX_SLOTS];
static atomic_uint g_camera_added_sequence[NATIVE_CAPTURE_REDIRECT_MAX_SLOTS];
static atomic_uint g_camera_removed_sequence[NATIVE_CAPTURE_REDIRECT_MAX_SLOTS];

/* How many frames the capture stub may still hand out. UINT_MAX lets the worker
 * free-run; a finite budget lets a test grant several credits before the worker
 * can see any of them, which is what makes shared-encode counting deterministic. */
#define CAMERA_FRAMES_UNLIMITED UINT_MAX
static atomic_uint g_camera_frames_allowed = CAMERA_FRAMES_UNLIMITED;

#define CAPTURE_TEST_FAIL_ALLOC (1u << 0)
#define CAPTURE_TEST_FAIL_MUTEX (1u << 1)
#define CAPTURE_TEST_FAIL_CAMERA_THREAD (1u << 2)
#define CAPTURE_TEST_FAIL_AUDIO_THREAD (1u << 3)

void native_capture_redirect_test_set_failures(unsigned failures);
void native_capture_redirect_test_set_camera_physical_available(NativeCaptureRedirect *redirect, bool available);

static void reset_transition_recording(void) {
    atomic_store(&g_transition_sequence, 0u);
    for (int i = 0; i < NATIVE_CAPTURE_REDIRECT_MAX_SLOTS; i++) {
        atomic_store(&g_capture_closed_sequence[i], 0u);
        atomic_store(&g_capture_opened_sequence[i], 0u);
        atomic_store(&g_camera_added_sequence[i], 0u);
        atomic_store(&g_camera_removed_sequence[i], 0u);
    }
}

static void record_transition(atomic_uint *sequence) {
    if (atomic_load(&g_record_transitions)) {
        atomic_store(sequence, atomic_fetch_add(&g_transition_sequence, 1u) + 1u);
    }
}

static void test_sleep_ms(unsigned milliseconds) {
    native_sleep_ms(milliseconds);
}

static bool wait_for_counter(atomic_uint *counter, unsigned minimum) {
    for (unsigned attempt = 0; attempt < 500u; attempt++) {
        if (atomic_load(counter) >= minimum) {
            return true;
        }
        test_sleep_ms(5u);
    }
    return false;
}

static bool wait_for_value(atomic_uint *value, unsigned expected) {
    for (unsigned attempt = 0; attempt < 500u; attempt++) {
        if (atomic_load(value) == expected) {
            return true;
        }
        test_sleep_ms(5u);
    }
    return false;
}

void rdp_set_camera_available(RdpSession *session, bool available) {
    assert(session);
    assert(session->slot >= 0 && session->slot < NATIVE_CAPTURE_REDIRECT_MAX_SLOTS);
    bool previous = atomic_exchange(&session->camera_available, available);
    assert(previous != available);
    atomic_fetch_add(available ? &g_camera_added[session->slot] : &g_camera_removed[session->slot], 1u);
    record_transition(available ? &g_camera_added_sequence[session->slot]
                                : &g_camera_removed_sequence[session->slot]);
}

void rdp_set_capture_active(RdpSession *session, bool active) {
    assert(session);
    assert(session->slot >= 0 && session->slot < NATIVE_CAPTURE_REDIRECT_MAX_SLOTS);
    bool previous = atomic_exchange(&session->capture_active, active);
    if (previous == active) {
        return;
    }
    record_transition(active ? &g_capture_opened_sequence[session->slot]
                             : &g_capture_closed_sequence[session->slot]);
    if (!g_handoff_probe_app) {
        return;
    }
    unsigned sequence = ++g_handoff_sequence;
    if (session->slot == g_handoff_old_slot && !active) {
        assert(atomic_load(&g_handoff_probe_app->active_index) == g_handoff_old_slot);
        g_handoff_pause_sequence = sequence;
    }
    if (session->slot == g_handoff_new_slot && active) {
        assert(atomic_load(&g_handoff_probe_app->active_index) == g_handoff_new_slot);
        g_handoff_activate_sequence = sequence;
    }
}

bool rdp_submit_camera_h264(RdpSession *session, uint64_t generation, const uint8_t *data, size_t len,
                            bool is_keyframe) {
    static uint64_t last_generation[NATIVE_CAPTURE_REDIRECT_MAX_SLOTS];
    static unsigned last_open[NATIVE_CAPTURE_REDIRECT_MAX_SLOTS];
    assert(session);
    assert(data);
    assert(len > 0u);
    assert(len <= NATIVE_CAMERA_H264_MAX_AU_BYTES);
    NativeH264Info info;
    assert(native_h264_scan_annexb(data, len, &info) == NATIVE_H264_OK);
    assert(is_keyframe == info.has_idr);
    unsigned current_open = atomic_load(&g_camera_open_count);
    if (last_generation[session->slot] != generation || last_open[session->slot] != current_open) {
        assert(is_keyframe);
        assert(info.can_seed_decoder);
        last_generation[session->slot] = generation;
        last_open[session->slot] = current_open;
    }
    if (!atomic_load(&session->capture_active)) {
        return false;
    }
    unsigned failures = atomic_load(&g_camera_submit_failures);
    if (failures > 0u && atomic_compare_exchange_strong(&g_camera_submit_failures, &failures, failures - 1u)) {
        return false;
    }
    atomic_fetch_add(&g_camera_packets[session->slot], 1u);
    return true;
}

bool rdp_submit_camera_error(RdpSession *session, uint64_t generation) {
    (void)generation;
    assert(session);
    if (!atomic_load(&session->capture_active)) {
        return false;
    }
    atomic_fetch_add(&g_camera_errors[session->slot], 1u);
    return true;
}

bool rdp_submit_audio_input_pcm(RdpSession *session, uint64_t generation, const uint8_t *data, size_t len) {
    (void)generation;
    assert(session);
    assert(data);
    assert(len > 0u);
    if (!atomic_load(&session->capture_active)) {
        return false;
    }
    atomic_fetch_add(&g_audio_packets[session->slot], 1u);
    return true;
}

size_t native_camera_enumerate(NativeCameraDeviceInfo *devices, size_t capacity) {
    if (!atomic_load(&g_camera_present)) {
        return 0u;
    }
    if (devices && capacity > 0u) {
        memset(&devices[0], 0, sizeof(devices[0]));
        (void)strcpy(devices[0].id, "test-camera");
    }
    return 1u;
}

bool native_camera_h264_mode_available(const char *device_id, uint32_t width, uint32_t height, uint32_t fps) {
    (void)device_id;
    return atomic_load(&g_camera_present) && width == 640u && height == 480u && fps == 15u;
}

bool native_camera_capture_open_h264(const char *device_id, uint32_t width, uint32_t height, uint32_t fps,
                                     NativeCameraCapture **out_capture) {
    (void)device_id;
    assert(width == 640u);
    assert(height == 480u);
    assert(fps == 15u);
    assert(out_capture);
    if (!atomic_load(&g_camera_present)) {
        *out_capture = NULL;
        return false;
    }
    atomic_fetch_add(&g_camera_open_count, 1u);
    g_camera_capture.sequence = 0u;
    g_camera_capture.seed_delay = atomic_exchange(&g_camera_next_seed_delay, 0u);
    g_camera_capture.first_frame = true;
    g_camera_capture.seeded = false;
    *out_capture = &g_camera_capture;
    return true;
}

void native_camera_capture_close(NativeCameraCapture *capture) {
    if (capture) {
        assert(capture == &g_camera_capture);
        atomic_fetch_add(&g_camera_close_count, 1u);
    }
}

static const uint8_t g_camera_seed[] = {
    0u, 0u, 0u, 1u, 0x67u, 0x42u, 0u, 0x1fu,
    0u, 0u, 1u, 0x68u, 0xceu, 0x06u,
    0u, 0u, 0u, 1u, 0x65u, 0x88u, 0x84u,
};
static const uint8_t g_camera_delta[] = {0u, 0u, 1u, 0x41u, 0x9au, 0x22u};
static atomic_uint g_camera_frames_held;
static atomic_uint g_camera_keyframe_requests;

NativeCameraAcquireResult native_camera_capture_acquire(NativeCameraCapture *capture, int timeout_ms,
                                                        NativeCameraFrame *frame) {
    assert(capture == &g_camera_capture);
    (void)timeout_ms;
    assert(frame);
    unsigned recoveries = atomic_load(&g_camera_acquire_recoveries);
    if (recoveries > 0u &&
        atomic_compare_exchange_strong(&g_camera_acquire_recoveries, &recoveries, recoveries - 1u)) {
        return NATIVE_CAMERA_ACQUIRE_RECOVER;
    }
    if (!capture->first_frame) {
        atomic_fetch_add(&g_camera_delta_acquires, 1u);
        while (atomic_load(&g_camera_hold_delta)) {
            test_sleep_ms(1u);
        }
    }
    /* Consume one unit of the frame budget, behaving like a capture timeout while
     * the budget is empty so the worker stays on its normal retry path. */
    for (unsigned attempt = 0;; attempt++) {
        unsigned allowed = atomic_load(&g_camera_frames_allowed);
        if (allowed == CAMERA_FRAMES_UNLIMITED) {
            break;
        }
        if (allowed > 0u && atomic_compare_exchange_strong(&g_camera_frames_allowed, &allowed, allowed - 1u)) {
            break;
        }
        if (attempt >= 100u) {
            return NATIVE_CAMERA_ACQUIRE_TIMEOUT;
        }
        test_sleep_ms(1u);
    }
    memset(frame, 0, sizeof(*frame));
    bool seed = !capture->seeded && capture->seed_delay == 0u;
    if (seed) {
        capture->seeded = true;
    } else if (!capture->seeded) {
        capture->seed_delay--;
    }
    frame->data = seed ? g_camera_seed : g_camera_delta;
    frame->len = seed ? sizeof(g_camera_seed) : sizeof(g_camera_delta);
    frame->timestamp_us = 1000u + capture->sequence;
    frame->sequence = capture->sequence++;
    frame->held = true;
    capture->first_frame = false;
    atomic_fetch_add(&g_camera_frames_held, 1u);
    test_sleep_ms(1u);
    return NATIVE_CAMERA_ACQUIRE_FRAME;
}

void native_camera_capture_release(NativeCameraCapture *capture, NativeCameraFrame *frame) {
    assert(capture == &g_camera_capture);
    assert(frame);
    /* Every acquired buffer must be handed back, including on the paths that skip
     * conversion or encoding. */
    assert(frame->held);
    atomic_fetch_sub(&g_camera_frames_held, 1u);
    memset(frame, 0, sizeof(*frame));
}

void native_camera_capture_request_keyframe(NativeCameraCapture *capture) {
    assert(capture == &g_camera_capture);
    atomic_fetch_add(&g_camera_keyframe_requests, 1u);
}

NativeAudioInputCapture *native_audio_input_capture_open(const char *stable_id, uint32_t sample_rate,
                                                         uint16_t channels) {
    (void)stable_id;
    assert(sample_rate == 48000u);
    assert(channels == 1u);
    atomic_fetch_add(&g_audio_open_count, 1u);
    return &g_audio_capture;
}

void native_audio_input_capture_close(NativeAudioInputCapture *capture) {
    if (capture) {
        atomic_fetch_add(&g_audio_close_count, 1u);
    }
}

long native_audio_input_capture_read(NativeAudioInputCapture *capture, int16_t *samples, size_t frames) {
    assert(capture == &g_audio_capture);
    memset(samples, 0, frames * sizeof(*samples));
    test_sleep_ms(1u);
    return (long)frames;
}

uint32_t native_audio_input_capture_rate(const NativeAudioInputCapture *capture) {
    assert(capture == &g_audio_capture);
    return 48000u;
}

uint16_t native_audio_input_capture_channels(const NativeAudioInputCapture *capture) {
    assert(capture == &g_audio_capture);
    return 1u;
}

static NativeCaptureRedirectConfig test_config(void) {
    NativeCaptureRedirectConfig config = {
        .camera_enabled = true,
        .camera_device_id = "test-camera",
        .camera_width = 640,
        .camera_height = 480,
        .camera_fps = 15,
        .audio_input_enabled = true,
        .audio_input_device_id = "test-microphone",
        .audio_input_gain_db = 0,
    };
    return config;
}

static void test_active_slot_handoff_order(void) {
    App app = {0};
    NativeCaptureRedirectConfig config = test_config();
    config.camera_enabled = false;
    config.audio_input_enabled = false;
    struct RdpSession red = {.slot = NATIVE_SESSION_SLOT_RED};
    struct RdpSession blue = {.slot = NATIVE_SESSION_SLOT_BLUE};
    atomic_init(&red.capture_active, false);
    atomic_init(&blue.capture_active, false);
    atomic_init(&red.camera_available, false);
    atomic_init(&blue.camera_available, false);
    atomic_init(&app.active_index, NATIVE_SESSION_SLOT_RED);

    assert(native_capture_redirect_init(&app.capture_redirect, &config));
    native_capture_redirect_set_slot(&app.capture_redirect, red.slot, &red, false, false);
    native_capture_redirect_set_slot(&app.capture_redirect, blue.slot, &blue, false, false);
    native_capture_follow_active_slot(&app);
    assert(atomic_load(&red.capture_active));
    assert(!atomic_load(&blue.capture_active));

    g_handoff_probe_app = &app;
    g_handoff_old_slot = red.slot;
    g_handoff_new_slot = blue.slot;
    g_handoff_sequence = 0u;
    g_handoff_pause_sequence = 0u;
    g_handoff_activate_sequence = 0u;
    native_capture_pause_for_active_slot_change(&app);
    atomic_store(&app.active_index, blue.slot);
    native_capture_follow_active_slot(&app);
    g_handoff_probe_app = NULL;

    assert(atomic_load(&app.active_index) == blue.slot);
    assert(!atomic_load(&red.capture_active));
    assert(atomic_load(&blue.capture_active));
    assert(g_handoff_pause_sequence != 0u);
    assert(g_handoff_activate_sequence > g_handoff_pause_sequence);
    native_capture_redirect_destroy(&app.capture_redirect);
}

static void test_init_failures(void) {
    NativeCaptureRedirectConfig config = test_config();
    static const unsigned failures[] = {
        CAPTURE_TEST_FAIL_ALLOC,
        CAPTURE_TEST_FAIL_MUTEX,
        CAPTURE_TEST_FAIL_CAMERA_THREAD,
        CAPTURE_TEST_FAIL_AUDIO_THREAD,
        CAPTURE_TEST_FAIL_CAMERA_THREAD | CAPTURE_TEST_FAIL_AUDIO_THREAD,
    };

    for (size_t i = 0; i < sizeof(failures) / sizeof(failures[0]); i++) {
        NativeCaptureRedirect redirect = {.impl = (void *)(uintptr_t)1u};
        native_capture_redirect_test_set_failures(failures[i]);
        assert(!native_capture_redirect_init(&redirect, &config));
        if ((failures[i] & (CAPTURE_TEST_FAIL_ALLOC | CAPTURE_TEST_FAIL_MUTEX)) != 0) {
            assert(redirect.impl == NULL);
        } else {
            /* A worker-start failure is best-effort: init publishes the manager so
             * the successful sibling worker can remain usable and destroyable. */
            assert(redirect.impl != NULL);
        }
        native_capture_redirect_destroy(&redirect);
        assert(redirect.impl == NULL);
    }
    native_capture_redirect_test_set_failures(0u);

    /* Reproduce startup allocation failure followed by a successful settings-save
     * retry. The fresh manager starts at foreground=-1; the glue must republish the
     * app's current slot after publishing the live session handles. */
    App app = {0};
    NativeSettings settings = {0};
    struct RdpSession red = {.slot = NATIVE_SESSION_SLOT_RED};
    struct RdpSession blue = {.slot = NATIVE_SESSION_SLOT_BLUE};
    atomic_init(&red.capture_active, false);
    atomic_init(&blue.capture_active, false);
    atomic_init(&red.camera_available, false);
    atomic_init(&blue.camera_available, false);
    NativeCaptureRedirectConfig late_config = test_config();
    settings.camera_enabled = late_config.camera_enabled;
    (void)strcpy(settings.camera_device_id, late_config.camera_device_id);
    settings.camera_width = late_config.camera_width;
    settings.camera_height = late_config.camera_height;
    settings.camera_fps = late_config.camera_fps;
    settings.audio_input_enabled = late_config.audio_input_enabled;
    (void)strcpy(settings.audio_input_device_id, late_config.audio_input_device_id);
    settings.audio_input_gain_db = late_config.audio_input_gain_db;
    settings.sessions[NATIVE_SESSION_SLOT_RED].camera_redirect = true;
    settings.sessions[NATIVE_SESSION_SLOT_RED].audio_input_redirect = true;
    settings.sessions[NATIVE_SESSION_SLOT_BLUE].camera_redirect = true;
    settings.sessions[NATIVE_SESSION_SLOT_BLUE].audio_input_redirect = true;
    app.settings = &settings;
    app.sessions[NATIVE_SESSION_SLOT_RED].rdp = &red;
    app.sessions[NATIVE_SESSION_SLOT_BLUE].rdp = &blue;
    atomic_init(&app.active_index, NATIVE_SESSION_SLOT_BLUE);

    native_capture_redirect_test_set_failures(CAPTURE_TEST_FAIL_ALLOC);
    native_reconfigure_capture_redirect(&app);
    assert(app.capture_redirect.impl == NULL);
    native_capture_redirect_test_set_failures(0u);
    native_reconfigure_capture_redirect(&app);
    assert(app.capture_redirect.impl != NULL);
    assert(app.capture_applied_valid);
    assert(!atomic_load(&red.capture_active));
    assert(atomic_load(&blue.capture_active));
    native_capture_redirect_destroy(&app.capture_redirect);
}

static void test_camera_availability_inputs(void) {
    NativeCaptureRedirect redirect = {0};
    NativeCaptureRedirectConfig config = test_config();
    struct RdpSession green = {.slot = NATIVE_SESSION_SLOT_GREEN};
    struct RdpSession replacement = {.slot = NATIVE_SESSION_SLOT_GREEN};
    atomic_init(&green.capture_active, false);
    atomic_init(&green.camera_available, false);
    atomic_init(&replacement.capture_active, false);
    atomic_init(&replacement.camera_available, false);
    config.audio_input_enabled = false;

    /* Keep physical availability entirely under test control. A failed camera
     * worker is best-effort and deliberately leaves a usable manager behind. */
    native_capture_redirect_test_set_failures(CAPTURE_TEST_FAIL_CAMERA_THREAD);
    assert(!native_capture_redirect_init(&redirect, &config));
    assert(redirect.impl != NULL);
    native_capture_redirect_set_foreground_slot(&redirect, green.slot);
    native_capture_redirect_set_slot(&redirect, green.slot, &green, true, false);
    assert(atomic_load(&green.capture_active));
    assert(!atomic_load(&green.camera_available));

    native_capture_redirect_test_set_camera_physical_available(&redirect, true);
    assert(atomic_load(&green.camera_available));
    assert(atomic_load(&g_camera_added[green.slot]) == 1u);
    native_capture_redirect_test_set_camera_physical_available(&redirect, false);
    assert(!atomic_load(&green.camera_available));
    assert(atomic_load(&g_camera_removed[green.slot]) == 1u);
    native_capture_redirect_test_set_camera_physical_available(&redirect, true);
    assert(atomic_load(&green.camera_available));
    assert(atomic_load(&g_camera_added[green.slot]) == 2u);

    native_capture_redirect_set_slot(&redirect, green.slot, &green, false, false);
    assert(!atomic_load(&green.camera_available));
    assert(atomic_load(&g_camera_removed[green.slot]) == 2u);
    native_capture_redirect_test_set_camera_physical_available(&redirect, false);
    native_capture_redirect_test_set_camera_physical_available(&redirect, true);
    assert(atomic_load(&g_camera_added[green.slot]) == 2u);
    assert(atomic_load(&g_camera_removed[green.slot]) == 2u);
    native_capture_redirect_set_slot(&redirect, green.slot, &green, true, false);
    assert(atomic_load(&green.camera_available));
    assert(atomic_load(&g_camera_added[green.slot]) == 3u);

    reset_transition_recording();
    atomic_store(&g_record_transitions, true);
    native_capture_redirect_set_foreground_slot(&redirect, -1);
    atomic_store(&g_record_transitions, false);
    assert(!atomic_load(&green.capture_active));
    assert(!atomic_load(&green.camera_available));
    assert(atomic_load(&g_capture_closed_sequence[green.slot]) != 0u);
    assert(atomic_load(&g_capture_closed_sequence[green.slot]) <
           atomic_load(&g_camera_removed_sequence[green.slot]));
    assert(atomic_load(&g_camera_removed[green.slot]) == 3u);

    /* Permission and device churn while backgrounded must not publish a camera. */
    native_capture_redirect_set_slot(&redirect, green.slot, &green, false, false);
    native_capture_redirect_test_set_camera_physical_available(&redirect, false);
    native_capture_redirect_set_slot(&redirect, green.slot, &green, true, false);
    native_capture_redirect_test_set_camera_physical_available(&redirect, true);
    assert(atomic_load(&g_camera_added[green.slot]) == 3u);
    assert(atomic_load(&g_camera_removed[green.slot]) == 3u);

    reset_transition_recording();
    atomic_store(&g_record_transitions, true);
    native_capture_redirect_set_foreground_slot(&redirect, green.slot);
    atomic_store(&g_record_transitions, false);
    assert(atomic_load(&green.capture_active));
    assert(atomic_load(&green.camera_available));
    assert(atomic_load(&g_capture_opened_sequence[green.slot]) != 0u);
    assert(atomic_load(&g_capture_opened_sequence[green.slot]) <
           atomic_load(&g_camera_added_sequence[green.slot]));
    assert(atomic_load(&g_camera_added[green.slot]) == 4u);

    /* Replacing a live foreground session retires the old handle before either
     * gate or camera availability is published to the replacement. */
    reset_transition_recording();
    atomic_store(&g_record_transitions, true);
    native_capture_redirect_set_slot(&redirect, green.slot, &replacement, true, false);
    atomic_store(&g_record_transitions, false);
    assert(!atomic_load(&green.capture_active));
    assert(!atomic_load(&green.camera_available));
    assert(atomic_load(&replacement.capture_active));
    assert(atomic_load(&replacement.camera_available));
    assert(atomic_load(&g_capture_closed_sequence[green.slot]) <
           atomic_load(&g_camera_removed_sequence[green.slot]));
    assert(atomic_load(&g_camera_removed_sequence[green.slot]) <
           atomic_load(&g_capture_opened_sequence[green.slot]));
    assert(atomic_load(&g_capture_opened_sequence[green.slot]) <
           atomic_load(&g_camera_added_sequence[green.slot]));
    assert(atomic_load(&g_camera_added[green.slot]) == 5u);
    assert(atomic_load(&g_camera_removed[green.slot]) == 4u);

    reset_transition_recording();
    atomic_store(&g_record_transitions, true);
    native_capture_redirect_destroy(&redirect);
    atomic_store(&g_record_transitions, false);
    assert(!atomic_load(&replacement.capture_active));
    assert(!atomic_load(&replacement.camera_available));
    assert(atomic_load(&g_capture_closed_sequence[green.slot]) != 0u);
    assert(atomic_load(&g_capture_closed_sequence[green.slot]) <
           atomic_load(&g_camera_removed_sequence[green.slot]));
    assert(atomic_load(&g_camera_removed[green.slot]) == 5u);
    native_capture_redirect_test_set_failures(0u);
}

static void test_rapid_foreground_round_trip_reopens_camera(void) {
    NativeCaptureRedirect redirect = {0};
    NativeCaptureRedirectConfig config = test_config();
    struct RdpSession green = {.slot = NATIVE_SESSION_SLOT_GREEN};
    struct RdpSession yellow = {.slot = NATIVE_SESSION_SLOT_YELLOW};
    atomic_init(&green.capture_active, false);
    atomic_init(&green.camera_available, false);
    atomic_init(&yellow.capture_active, false);
    atomic_init(&yellow.camera_available, false);
    config.audio_input_enabled = false;

    unsigned green_packets = atomic_load(&g_camera_packets[green.slot]);
    unsigned green_errors = atomic_load(&g_camera_errors[green.slot]);
    unsigned delta_acquires = atomic_load(&g_camera_delta_acquires);
    atomic_store(&g_camera_frames_allowed, 0u);
    atomic_store(&g_camera_hold_delta, true);

    assert(native_capture_redirect_init(&redirect, &config));
    native_capture_redirect_set_foreground_slot(&redirect, green.slot);
    native_capture_redirect_set_slot(&redirect, green.slot, &green, true, false);
    native_capture_redirect_set_slot(&redirect, yellow.slot, &yellow, true, false);
    native_capture_redirect_camera_start(&redirect, green.slot, 70u, 640u, 480u, 15u);
    native_capture_redirect_camera_sample_request(&redirect, green.slot, 70u);
    atomic_store(&g_camera_frames_allowed, 1u);
    assert(wait_for_counter(&g_camera_packets[green.slot], green_packets + 1u));
    assert(wait_for_counter(&g_camera_delta_acquires, delta_acquires + 1u));

    /* The worker is inside acquire with the old redirection epoch. Moving away
     * and back before it wakes must still invalidate the retained FIFO and V4L2
     * reference chain, and the pending post-handoff credit must receive an error. */
    unsigned camera_opens = atomic_load(&g_camera_open_count);
    native_capture_redirect_set_foreground_slot(&redirect, yellow.slot);
    native_capture_redirect_set_foreground_slot(&redirect, green.slot);
    native_capture_redirect_camera_sample_request(&redirect, green.slot, 70u);
    atomic_store(&g_camera_frames_allowed, 2u);
    atomic_store(&g_camera_hold_delta, false);
    assert(wait_for_counter(&g_camera_errors[green.slot], green_errors + 1u));
    assert(wait_for_counter(&g_camera_open_count, camera_opens + 1u));

    native_capture_redirect_camera_sample_request(&redirect, green.slot, 70u);
    assert(wait_for_counter(&g_camera_packets[green.slot], green_packets + 2u));
    assert(atomic_load(&green.capture_active));
    assert(!atomic_load(&yellow.capture_active));

    native_capture_redirect_camera_stop(&redirect, green.slot, 70u);
    atomic_store(&g_camera_frames_allowed, CAMERA_FRAMES_UNLIMITED);
    atomic_store(&g_camera_hold_delta, false);
    native_capture_redirect_destroy(&redirect);
}

static void test_session_replacement_reopens_camera(void) {
    NativeCaptureRedirect redirect = {0};
    NativeCaptureRedirectConfig config = test_config();
    struct RdpSession original = {.slot = NATIVE_SESSION_SLOT_YELLOW};
    struct RdpSession replacement = {.slot = NATIVE_SESSION_SLOT_YELLOW};
    atomic_init(&original.capture_active, false);
    atomic_init(&original.camera_available, false);
    atomic_init(&replacement.capture_active, false);
    atomic_init(&replacement.camera_available, false);
    config.audio_input_enabled = false;

    unsigned packets = atomic_load(&g_camera_packets[original.slot]);
    unsigned errors = atomic_load(&g_camera_errors[original.slot]);
    unsigned delta_acquires = atomic_load(&g_camera_delta_acquires);
    atomic_store(&g_camera_frames_allowed, 0u);
    atomic_store(&g_camera_hold_delta, true);

    assert(native_capture_redirect_init(&redirect, &config));
    native_capture_redirect_set_foreground_slot(&redirect, original.slot);
    native_capture_redirect_set_slot(&redirect, original.slot, &original, true, false);
    native_capture_redirect_camera_start(&redirect, original.slot, 1u, 640u, 480u, 15u);
    native_capture_redirect_camera_sample_request(&redirect, original.slot, 1u);
    atomic_store(&g_camera_frames_allowed, 1u);
    assert(wait_for_counter(&g_camera_packets[original.slot], packets + 1u));
    assert(wait_for_counter(&g_camera_delta_acquires, delta_acquires + 1u));

    /* A new Rust worker can reuse generation 1. Replace the session while the
     * camera worker holds a dependent AU from the original slot generation. */
    unsigned camera_opens = atomic_load(&g_camera_open_count);
    native_capture_redirect_set_slot(&redirect, original.slot, &replacement, true, false);
    native_capture_redirect_camera_start(&redirect, replacement.slot, 1u, 640u, 480u, 15u);
    native_capture_redirect_camera_sample_request(&redirect, replacement.slot, 1u);
    atomic_store(&g_camera_frames_allowed, 2u);
    atomic_store(&g_camera_hold_delta, false);
    assert(wait_for_counter(&g_camera_errors[replacement.slot], errors + 1u));
    assert(wait_for_counter(&g_camera_open_count, camera_opens + 1u));

    native_capture_redirect_camera_sample_request(&redirect, replacement.slot, 1u);
    assert(wait_for_counter(&g_camera_packets[replacement.slot], packets + 2u));
    assert(!atomic_load(&original.capture_active));
    assert(!atomic_load(&original.camera_available));
    assert(atomic_load(&replacement.capture_active));
    assert(atomic_load(&replacement.camera_available));

    native_capture_redirect_camera_stop(&redirect, replacement.slot, 1u);
    atomic_store(&g_camera_frames_allowed, CAMERA_FRAMES_UNLIMITED);
    atomic_store(&g_camera_hold_delta, false);
    native_capture_redirect_destroy(&redirect);
}

static void test_delayed_seed_fails_credit_without_reopen(void) {
    NativeCaptureRedirect redirect = {0};
    NativeCaptureRedirectConfig config = test_config();
    struct RdpSession green = {.slot = NATIVE_SESSION_SLOT_GREEN};
    atomic_init(&green.capture_active, false);
    atomic_init(&green.camera_available, false);
    config.audio_input_enabled = false;

    unsigned packets = atomic_load(&g_camera_packets[green.slot]);
    unsigned errors = atomic_load(&g_camera_errors[green.slot]);
    unsigned camera_opens = atomic_load(&g_camera_open_count);
    unsigned camera_closes = atomic_load(&g_camera_close_count);
    atomic_store(&g_camera_frames_allowed, 0u);
    atomic_store(&g_camera_next_seed_delay, 30u);

    assert(native_capture_redirect_init(&redirect, &config));
    native_capture_redirect_set_foreground_slot(&redirect, green.slot);
    native_capture_redirect_set_slot(&redirect, green.slot, &green, true, false);
    native_capture_redirect_camera_start(&redirect, green.slot, 90u, 640u, 480u, 15u);
    native_capture_redirect_camera_sample_request(&redirect, green.slot, 90u);
    atomic_store(&g_camera_frames_allowed, NATIVE_CAMERA_H264_WAIT_AUS);
    assert(wait_for_counter(&g_camera_errors[green.slot], errors + 1u));
    assert(atomic_load(&g_camera_open_count) == camera_opens + 1u);
    assert(atomic_load(&g_camera_close_count) == camera_closes);
    assert(atomic_load(&g_camera_packets[green.slot]) == packets);
    assert(atomic_load(&g_camera_next_seed_delay) == 0u);

    /* Let the original capture run through the rest of a 30-picture GOP with no
     * credit. The cached stream state must survive each later wait bound. */
    atomic_store(&g_camera_frames_allowed, 30u - NATIVE_CAMERA_H264_WAIT_AUS);
    assert(wait_for_value(&g_camera_frames_allowed, 0u));
    assert(atomic_load(&g_camera_open_count) == camera_opens + 1u);
    assert(atomic_load(&g_camera_close_count) == camera_closes);

    native_capture_redirect_camera_sample_request(&redirect, green.slot, 90u);
    atomic_store(&g_camera_frames_allowed, 1u);
    assert(wait_for_counter(&g_camera_packets[green.slot], packets + 1u));
    assert(atomic_load(&green.capture_active));
    assert(atomic_load(&g_camera_open_count) == camera_opens + 1u);
    assert(atomic_load(&g_camera_close_count) == camera_closes);

    native_capture_redirect_camera_stop(&redirect, green.slot, 90u);
    atomic_store(&g_camera_frames_allowed, CAMERA_FRAMES_UNLIMITED);
    atomic_store(&g_camera_next_seed_delay, 0u);
    native_capture_redirect_destroy(&redirect);
}

int main(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], "--init-failure") == 0) {
        test_init_failures();
        return 0;
    }

    test_active_slot_handoff_order();
    test_camera_availability_inputs();
    test_rapid_foreground_round_trip_reopens_camera();
    test_session_replacement_reopens_camera();
    test_delayed_seed_fails_credit_without_reopen();

    NativeCaptureRedirect redirect = {0};
    NativeCaptureRedirectConfig config = test_config();
    struct RdpSession red = {.slot = 0};
    struct RdpSession blue = {.slot = 3};
    atomic_init(&red.capture_active, false);
    atomic_init(&blue.capture_active, false);
    atomic_init(&red.camera_available, false);
    atomic_init(&blue.camera_available, false);

    unsigned initial_camera_opens = atomic_load(&g_camera_open_count);
    assert(native_capture_redirect_init(&redirect, &config));
    native_capture_redirect_set_foreground_slot(&redirect, red.slot);
    native_capture_redirect_set_slot(&redirect, red.slot, &red, true, true);
    native_capture_redirect_set_slot(&redirect, blue.slot, &blue, true, true);
    assert(atomic_load(&red.capture_active));
    assert(!atomic_load(&blue.capture_active));
    assert(wait_for_counter(&g_camera_added[red.slot], 1u));
    assert(atomic_load(&red.camera_available));
    assert(!atomic_load(&blue.camera_available));
    assert(atomic_load(&g_camera_added[blue.slot]) == 0u);

    /* Only the foreground session is offered the device and may consume camera
     * credits or put microphone PCM on the wire. */
    atomic_store(&g_camera_frames_allowed, 0u);
    native_capture_redirect_camera_start(&redirect, red.slot, 30u, 640u, 480u, 15u);
    native_capture_redirect_camera_sample_request(&redirect, red.slot, 30u);
    atomic_store(&g_camera_frames_allowed, 1u);
    assert(wait_for_counter(&g_camera_packets[red.slot], 1u));
    assert(atomic_load(&g_camera_open_count) == initial_camera_opens + 1u);

    native_capture_redirect_audio_start(&redirect, red.slot, 10u, 44100u, 2u, 441u);
    native_capture_redirect_audio_start(&redirect, blue.slot, 20u, 44100u, 2u, 441u);
    assert(wait_for_counter(&g_audio_packets[red.slot], 3u));
    test_sleep_ms(30u);
    assert(atomic_load(&g_audio_packets[blue.slot]) == 0u);
    assert(atomic_load(&g_audio_open_count) == 1u);

    /* A name-only profile save republishes every slot with the same session and
     * permissions. It must preserve the native camera reference chain and the
     * outstanding RDPECAM credit instead of reopening V4L2. */
    unsigned unchanged_packets = atomic_load(&g_camera_packets[red.slot]);
    unsigned unchanged_errors = atomic_load(&g_camera_errors[red.slot]);
    unsigned unchanged_opens = atomic_load(&g_camera_open_count);
    unsigned unchanged_closes = atomic_load(&g_camera_close_count);
    native_capture_redirect_camera_sample_request(&redirect, red.slot, 30u);
    native_capture_redirect_set_slot(&redirect, red.slot, &red, true, true);
    native_capture_redirect_set_slot(&redirect, blue.slot, &blue, true, true);
    atomic_store(&g_camera_frames_allowed, 1u);
    assert(wait_for_counter(&g_camera_packets[red.slot], unchanged_packets + 1u));
    assert(atomic_load(&g_camera_errors[red.slot]) == unchanged_errors);
    assert(atomic_load(&g_camera_open_count) == unchanged_opens);
    assert(atomic_load(&g_camera_close_count) == unchanged_closes);

    /* A direct handoff closes the old capture gate, opens the new one, then removes
     * and adds the respective RDPECAM devices. The Rust stop callback remains the
     * authority that retires the old stream state. */
    unsigned red_camera_errors = atomic_load(&g_camera_errors[red.slot]);
    native_capture_redirect_camera_sample_request(&redirect, red.slot, 30u);
    reset_transition_recording();
    atomic_store(&g_record_transitions, true);
    native_capture_redirect_set_foreground_slot(&redirect, blue.slot);
    atomic_store(&g_record_transitions, false);
    unsigned red_camera_packets = atomic_load(&g_camera_packets[red.slot]);
    unsigned red_audio_packets = atomic_load(&g_audio_packets[red.slot]);
    assert(!atomic_load(&red.capture_active));
    assert(atomic_load(&blue.capture_active));
    assert(!atomic_load(&red.camera_available));
    assert(atomic_load(&blue.camera_available));
    assert(atomic_load(&g_capture_closed_sequence[red.slot]) != 0u);
    assert(atomic_load(&g_capture_opened_sequence[blue.slot]) != 0u);
    assert(atomic_load(&g_capture_closed_sequence[red.slot]) <
           atomic_load(&g_camera_removed_sequence[red.slot]));
    assert(atomic_load(&g_capture_opened_sequence[blue.slot]) <
           atomic_load(&g_camera_added_sequence[blue.slot]));
    assert(atomic_load(&g_camera_removed[red.slot]) == 1u);
    assert(atomic_load(&g_camera_added[blue.slot]) == 1u);
    assert(atomic_load(&g_camera_errors[red.slot]) == red_camera_errors + 1u);
    native_capture_redirect_camera_stop(&redirect, red.slot, 30u);

    native_capture_redirect_camera_start(&redirect, blue.slot, 40u, 640u, 480u, 15u);
    native_capture_redirect_camera_sample_request(&redirect, blue.slot, 40u);
    atomic_store(&g_camera_frames_allowed, 2u);
    assert(wait_for_counter(&g_camera_packets[blue.slot], 1u));
    assert(wait_for_counter(&g_audio_packets[blue.slot], 3u));
    test_sleep_ms(30u);
    assert(atomic_load(&g_camera_packets[red.slot]) == red_camera_packets);
    assert(atomic_load(&g_audio_packets[red.slot]) == red_audio_packets);
    assert(atomic_load(&g_camera_open_count) >= 2u);
    assert(atomic_load(&g_audio_open_count) == 1u);
    assert(atomic_load(&g_audio_close_count) == 0u);
    assert(atomic_load(&g_camera_removed[blue.slot]) == 0u);

    /* Pausing removes the current device after closing its gate. No camera stream
     * or pending sample survives until the server starts the re-added red device. */
    reset_transition_recording();
    atomic_store(&g_record_transitions, true);
    native_capture_redirect_set_foreground_slot(&redirect, -1);
    atomic_store(&g_record_transitions, false);
    native_capture_redirect_camera_stop(&redirect, blue.slot, 40u);
    unsigned blue_camera_packets = atomic_load(&g_camera_packets[blue.slot]);
    unsigned blue_audio_packets = atomic_load(&g_audio_packets[blue.slot]);
    assert(!atomic_load(&red.capture_active));
    assert(!atomic_load(&blue.capture_active));
    assert(!atomic_load(&red.camera_available));
    assert(!atomic_load(&blue.camera_available));
    assert(atomic_load(&g_capture_closed_sequence[blue.slot]) != 0u);
    assert(atomic_load(&g_capture_closed_sequence[blue.slot]) <
           atomic_load(&g_camera_removed_sequence[blue.slot]));
    test_sleep_ms(30u);
    assert(atomic_load(&g_camera_packets[blue.slot]) == blue_camera_packets);
    assert(atomic_load(&g_audio_packets[blue.slot]) == blue_audio_packets);
    assert(wait_for_counter(&g_camera_close_count, 1u));
    assert(wait_for_counter(&g_audio_close_count, 1u));
    assert(atomic_load(&g_camera_removed[red.slot]) == 1u);
    assert(atomic_load(&g_camera_removed[blue.slot]) == 1u);

    unsigned red_resume_audio = atomic_load(&g_audio_packets[red.slot]);
    unsigned red_resume_open_count = atomic_load(&g_camera_open_count);
    reset_transition_recording();
    atomic_store(&g_record_transitions, true);
    native_capture_redirect_set_foreground_slot(&redirect, red.slot);
    atomic_store(&g_record_transitions, false);
    assert(atomic_load(&red.capture_active));
    assert(!atomic_load(&blue.capture_active));
    assert(atomic_load(&red.camera_available));
    assert(!atomic_load(&blue.camera_available));
    assert(atomic_load(&g_capture_opened_sequence[red.slot]) != 0u);
    assert(atomic_load(&g_capture_opened_sequence[red.slot]) <
           atomic_load(&g_camera_added_sequence[red.slot]));
    assert(atomic_load(&g_camera_added[red.slot]) == 2u);

    native_capture_redirect_camera_start(&redirect, red.slot, 50u, 640u, 480u, 15u);
    native_capture_redirect_camera_sample_request(&redirect, red.slot, 50u);
    atomic_store(&g_camera_frames_allowed, 2u);
    assert(wait_for_counter(&g_camera_packets[red.slot], red_camera_packets + 1u));
    assert(wait_for_counter(&g_audio_packets[red.slot], red_resume_audio + 3u));
    assert(atomic_load(&g_camera_open_count) > red_resume_open_count);
    assert(atomic_load(&g_audio_open_count) >= 2u);

    /* A refused camera submission completes the one pending credit with an
     * error, discards the dependent FIFO, and reopens at a fresh decoder seed.
     * The RDP capture gate and independent microphone stream stay active. */
    unsigned red_failure_packets = atomic_load(&g_camera_packets[red.slot]);
    red_camera_errors = atomic_load(&g_camera_errors[red.slot]);
    unsigned red_failure_audio = atomic_load(&g_audio_packets[red.slot]);
    atomic_store(&g_camera_submit_failures, 1u);
    native_capture_redirect_camera_sample_request(&redirect, red.slot, 50u);
    atomic_store(&g_camera_frames_allowed, 1u);
    assert(wait_for_counter(&g_camera_errors[red.slot], red_camera_errors + 1u));
    assert(atomic_load(&g_camera_packets[red.slot]) == red_failure_packets);
    assert(atomic_load(&red.capture_active));
    assert(wait_for_counter(&g_audio_packets[red.slot], red_failure_audio + 3u));

    native_capture_redirect_camera_sample_request(&redirect, red.slot, 50u);
    atomic_store(&g_camera_frames_allowed, 1u);
    assert(wait_for_counter(&g_camera_packets[red.slot], red_failure_packets + 1u));

    /* Unplug/replug removes and republishes only RDPECAM. A pending sample is
     * failed, while microphone packets and the RDP session continue. */
    unsigned unplug_removes = atomic_load(&g_camera_removed[red.slot]);
    unsigned replug_adds = atomic_load(&g_camera_added[red.slot]);
    red_camera_errors = atomic_load(&g_camera_errors[red.slot]);
    unsigned unplug_audio = atomic_load(&g_audio_packets[red.slot]);
    native_capture_redirect_camera_sample_request(&redirect, red.slot, 50u);
    atomic_store(&g_camera_present, false);
    atomic_store(&g_camera_acquire_recoveries, 1u);
    assert(wait_for_counter(&g_camera_errors[red.slot], red_camera_errors + 1u));
    assert(wait_for_counter(&g_camera_removed[red.slot], unplug_removes + 1u));
    assert(!atomic_load(&red.camera_available));
    assert(atomic_load(&red.capture_active));
    assert(wait_for_counter(&g_audio_packets[red.slot], unplug_audio + 3u));

    native_capture_redirect_camera_stop(&redirect, red.slot, 50u);
    atomic_store(&g_camera_present, true);
    assert(wait_for_counter(&g_camera_added[red.slot], replug_adds + 1u));
    native_capture_redirect_camera_start(&redirect, red.slot, 60u, 640u, 480u, 15u);
    native_capture_redirect_camera_sample_request(&redirect, red.slot, 60u);
    atomic_store(&g_camera_frames_allowed, 2u);
    assert(wait_for_counter(&g_camera_packets[red.slot], red_failure_packets + 2u));

    /* Foreground permission changes remove/re-add the device; identical churn on a
     * background slot is silent. Capture-active remains an independent microphone
     * gate and therefore stays open for the foreground session. */
    unsigned permission_red_removes = atomic_load(&g_camera_removed[red.slot]);
    unsigned permission_red_adds = atomic_load(&g_camera_added[red.slot]);
    native_capture_redirect_set_slot(&redirect, red.slot, &red, false, true);
    assert(atomic_load(&red.capture_active));
    assert(!atomic_load(&red.camera_available));
    assert(atomic_load(&g_camera_removed[red.slot]) == permission_red_removes + 1u);
    native_capture_redirect_camera_stop(&redirect, red.slot, 60u);
    native_capture_redirect_set_slot(&redirect, red.slot, &red, true, true);
    assert(atomic_load(&red.camera_available));
    assert(atomic_load(&g_camera_added[red.slot]) == permission_red_adds + 1u);

    unsigned blue_adds = atomic_load(&g_camera_added[blue.slot]);
    unsigned blue_removes = atomic_load(&g_camera_removed[blue.slot]);
    native_capture_redirect_set_slot(&redirect, blue.slot, &blue, false, true);
    native_capture_redirect_set_slot(&redirect, blue.slot, &blue, true, true);
    assert(!atomic_load(&blue.camera_available));
    assert(atomic_load(&g_camera_added[blue.slot]) == blue_adds);
    assert(atomic_load(&g_camera_removed[blue.slot]) == blue_removes);

    /* Rapid ownership churn keeps one foreground request and issues each old
     * removal before the corresponding new advertisement. The independent RDP
     * workers apply those requests asynchronously; TV acceptance checks the
     * resulting PipeWire nodes for transient overlap. */
    unsigned red_adds = atomic_load(&g_camera_added[red.slot]);
    unsigned red_removes = atomic_load(&g_camera_removed[red.slot]);
    blue_adds = atomic_load(&g_camera_added[blue.slot]);
    blue_removes = atomic_load(&g_camera_removed[blue.slot]);
    for (unsigned i = 0; i < 4u; i++) {
        reset_transition_recording();
        atomic_store(&g_record_transitions, true);
        native_capture_redirect_set_foreground_slot(&redirect, blue.slot);
        atomic_store(&g_record_transitions, false);
        assert(!atomic_load(&red.camera_available));
        assert(atomic_load(&blue.camera_available));
        assert(atomic_load(&g_camera_removed_sequence[red.slot]) != 0u);
        assert(atomic_load(&g_camera_added_sequence[blue.slot]) != 0u);
        assert(atomic_load(&g_camera_removed_sequence[red.slot]) <
               atomic_load(&g_camera_added_sequence[blue.slot]));

        reset_transition_recording();
        atomic_store(&g_record_transitions, true);
        native_capture_redirect_set_foreground_slot(&redirect, red.slot);
        atomic_store(&g_record_transitions, false);
        assert(atomic_load(&red.camera_available));
        assert(!atomic_load(&blue.camera_available));
        assert(atomic_load(&g_camera_removed_sequence[blue.slot]) != 0u);
        assert(atomic_load(&g_camera_added_sequence[red.slot]) != 0u);
        assert(atomic_load(&g_camera_removed_sequence[blue.slot]) <
               atomic_load(&g_camera_added_sequence[red.slot]));
    }
    assert(atomic_load(&g_camera_added[red.slot]) == red_adds + 4u);
    assert(atomic_load(&g_camera_removed[red.slot]) == red_removes + 4u);
    assert(atomic_load(&g_camera_added[blue.slot]) == blue_adds + 4u);
    assert(atomic_load(&g_camera_removed[blue.slot]) == blue_removes + 4u);

    /* Reconfigure issues one old-device removal before the replacement worker's
     * advertisement. The desired-state worker may coalesce a sufficiently fast
     * restart, which is valid because the payload gate remains authoritative. */
    red_adds = atomic_load(&g_camera_added[red.slot]);
    red_removes = atomic_load(&g_camera_removed[red.slot]);
    reset_transition_recording();
    atomic_store(&g_record_transitions, true);
    assert(native_capture_redirect_reconfigure(&redirect, &config));
    assert(wait_for_counter(&g_camera_added[red.slot], red_adds + 1u));
    atomic_store(&g_record_transitions, false);
    assert(atomic_load(&g_camera_removed[red.slot]) == red_removes + 1u);
    assert(atomic_load(&g_camera_added[red.slot]) == red_adds + 1u);
    assert(atomic_load(&g_camera_removed_sequence[red.slot]) != 0u);
    assert(atomic_load(&g_camera_added_sequence[red.slot]) != 0u);
    assert(atomic_load(&g_camera_removed_sequence[red.slot]) <
           atomic_load(&g_camera_added_sequence[red.slot]));

    red_removes = atomic_load(&g_camera_removed[red.slot]);
    blue_removes = atomic_load(&g_camera_removed[blue.slot]);
    native_capture_redirect_set_slot(&redirect, red.slot, NULL, false, false);
    assert(!atomic_load(&red.capture_active));
    assert(!atomic_load(&red.camera_available));
    assert(atomic_load(&g_camera_removed[red.slot]) == red_removes + 1u);
    assert(atomic_load(&g_camera_removed[blue.slot]) == blue_removes);

    native_capture_redirect_destroy(&redirect);
    assert(atomic_load(&g_camera_removed[blue.slot]) == blue_removes);
    assert(atomic_load(&g_camera_close_count) == atomic_load(&g_camera_open_count));
    assert(atomic_load(&g_audio_close_count) == atomic_load(&g_audio_open_count));
    /* No borrowed capture buffer may outlive the worker, whichever path it took. */
    assert(atomic_load(&g_camera_frames_held) == 0u);
    return 0;
}
