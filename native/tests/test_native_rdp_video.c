#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "native_app.h"
#include "native_rdp_callback_parts.h"
#include "native_rdp_media.h"
#include "native_rgba_owner.h"

/* Exercise the real callbacks, ownership and RGBA canvas. Only the hardware
 * decoder/media boundary is replaced; successful feed is not a display test. */
struct NativeVideo {
    uint16_t width;
    uint16_t height;
};

NativeMedia *native_ensure_media_locked(App *app) {
    return (NativeMedia *)app;
}

void native_open_speculative_audio_locked(App *app) {
    (void)app;
}

NativeVideo *native_video_open(NativeMedia *media, uint16_t width, uint16_t height, uint16_t fps) {
    (void)media;
    (void)fps;
    NativeVideo *video = malloc(sizeof(*video));
    assert(video);
    *video = (NativeVideo){width, height};
    return video;
}

void native_video_close(NativeVideo *video) { free(video); }
uint16_t native_video_width(const NativeVideo *video) { return video->width; }
uint16_t native_video_height(const NativeVideo *video) { return video->height; }

NativeVideoResult native_video_feed(NativeVideo *video, const uint8_t *data, size_t len, uint64_t pts90k) {
    (void)pts90k;
    assert(video && data && len);
    return NATIVE_VIDEO_OK;
}

static RdpCallbacks callbacks;
static uint32_t now_ms;

int clock_gettime(clockid_t clock_id, struct timespec *time) {
    (void)clock_id;
    time->tv_sec = (time_t)(now_ms / 1000u);
    time->tv_nsec = (long)(now_ms % 1000u) * 1000000L;
    return 0;
}

static void setup(App *app) {
    now_ms = 10000u;
    memset(app, 0, sizeof(*app));
    assert(pthread_mutex_init(&app->video_lock, NULL) == 0);
    atomic_init(&app->active_index, 0);
    atomic_init(&app->running, true);
    for (int i = 0; i < NATIVE_SETTINGS_MAX_SESSIONS; i++) {
        NativeSessionSlot *slot = &app->sessions[i];
        slot->app = app;
        slot->index = i;
        slot->config.fps = 60;
        atomic_init(&slot->connect_epoch, 1);
        atomic_init(&slot->desktop_width, 16);
        atomic_init(&slot->desktop_height, 16);
    }
    native_rdp_install_video_callbacks(&callbacks);
}

static void teardown(App *app) {
    native_video_close(app->video);
    native_close_rgba_locked(app, false);
    assert(pthread_mutex_destroy(&app->video_lock) == 0);
}

static void video(App *app, int index) {
    static const uint8_t seed[] = {0, 0, 1, 0x67, 1, 0, 0, 1, 0x68, 1, 0, 0, 1, 0x65, 1};
    callbacks.on_video_au(&app->sessions[index], seed, sizeof(seed), 0);
}

static void bitmap(App *app, int index) {
    static const uint8_t pixel[] = {17, 33, 65, 255};
    callbacks.on_bitmap_update(&app->sessions[index], 0, 0, 0, 1, 1, 4, pixel, sizeof(pixel));
}

static void test_refinement_hold_requires_same_slot_and_epoch(void) {
    App app;
    setup(&app);
    video(&app, 0);
    NativeVideo *decoder = app.video;
    bitmap(&app, 0);
    assert(app.video == decoder && !app.rgba);
    assert(!atomic_load(&app.sessions[0].video_via_bitmap));
    assert(atomic_load(&app.sessions[0].video_ok_frames) == 1);

    /* A new connection in the same slot must receive its first bitmap even
     * though the old epoch fed the retained decoder just now. */
    atomic_fetch_add(&app.sessions[0].connect_epoch, 1);
    bitmap(&app, 0);
    assert(!app.video && native_rgba_surface_has_frame(app.rgba));
    assert(atomic_load(&app.sessions[0].video_via_bitmap));
    assert(atomic_load(&app.sessions[0].video_ok_frames) == 2);
    assert(app.sessions[0].graphics_path_switches == 0);
    teardown(&app);

    setup(&app);
    video(&app, 0);
    atomic_store(&app.active_index, 1);
    bitmap(&app, 1);
    assert(!app.video && native_rgba_surface_has_frame(app.rgba));
    assert(native_rgba_owner_matches(&app, 1, 1));
    assert(atomic_load(&app.sessions[1].video_via_bitmap));
    assert(atomic_load(&app.sessions[1].video_ok_frames) == 1);
    assert(app.sessions[1].graphics_path_switches == 0);
    teardown(&app);
}

static void test_handoffs_do_not_trip_codec_flip_limit(void) {
    App app;
    setup(&app);
    for (int i = 0; i < 12; i++) {
        /* Retain the opposite path to exercise both foreign-owner checks. */
        atomic_store(&app.active_index, 0);
        video(&app, 0);
        atomic_store(&app.active_index, 1);
        /* Test a handoff outside the hold interval too. */
        app.video_last_feed_ms = now_ms - 2000u;
        bitmap(&app, 1);
        assert(native_rgba_surface_has_frame(app.rgba));
        assert(atomic_load(&app.running));
        assert(app.sessions[0].graphics_path_switches == 0);
        assert(app.sessions[1].graphics_path_switches == 0);
    }
    teardown(&app);
}

static void test_same_connection_still_limits_codec_flips(void) {
    App app;
    setup(&app);
    video(&app, 0);
    for (int i = 1; i <= 5; i++) {
        if (i & 1) {
            app.video_last_feed_ms = now_ms - 2000u;
            bitmap(&app, 0);
        } else {
            video(&app, 0);
        }
        assert(app.sessions[0].graphics_path_switches == i);
        assert(atomic_load(&app.running) == (i < 5));
    }
    assert(atomic_load(&app.sessions[0].terminal_state) == RDP_STATE_DECODER_ERROR);
    teardown(&app);
}

static void test_refinement_hold_across_clock_wrap(void) {
    App app;
    setup(&app);
    now_ms = UINT32_MAX - 99u;
    video(&app, 0);
    now_ms += 100u;
    assert(now_ms == 0);
    bitmap(&app, 0);
    assert(app.video && !app.rgba);
    /* A feed at exactly zero must also keep the H.264 plane. */
    video(&app, 0);
    now_ms = 1499u;
    bitmap(&app, 0);
    assert(app.video && !app.rgba);
    now_ms = 1500u;
    bitmap(&app, 0);
    assert(!app.video && native_rgba_surface_has_frame(app.rgba));
    assert(app.sessions[0].graphics_path_switches == 1);
    teardown(&app);
}

int main(void) {
    test_refinement_hold_requires_same_slot_and_epoch();
    test_handoffs_do_not_trip_codec_flip_limit();
    test_same_connection_still_limits_codec_flips();
    test_refinement_hold_across_clock_wrap();
    return 0;
}
