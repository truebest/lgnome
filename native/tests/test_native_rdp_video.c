#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "native_app.h"
#include "native_rdp_callback_parts.h"
#include "native_rdp_media.h"
#include "native_rgba_owner.h"
#include "native_rdp_video.h"
#include "native_rdp_state.h"

/* Exercise the real callbacks, ownership and RGBA canvas. Only the hardware
 * decoder/media boundary is replaced; successful feed is not a display test. */
struct NativeVideo {
    uint16_t width;
    uint16_t height;
    NativeVideoCodec codec;
    unsigned fed_count;
    uint8_t fed_tags[8];
    uint8_t fed_output_flags[8];
};

NativeMedia *native_ensure_media_locked(App *app) {
    return (NativeMedia *)app;
}

void native_open_speculative_audio_locked(App *app) {
    (void)app;
}

NativeVideo *native_video_open(NativeMedia *media, uint16_t width, uint16_t height, uint16_t fps, NativeVideoCodec codec) {
    (void)media;
    (void)fps;
    NativeVideo *video = malloc(sizeof(*video));
    assert(video);
    *video = (NativeVideo){.width = width, .height = height, .codec = codec};
    return video;
}

NativeVideoCodec native_video_codec(const NativeVideo *video) { return video->codec; }

void native_video_close(NativeVideo *video) { free(video); }
uint16_t native_video_width(const NativeVideo *video) { return video->width; }
uint16_t native_video_height(const NativeVideo *video) { return video->height; }

NativeVideoResult native_video_feed(NativeVideo *video, const uint8_t *data, size_t len, uint64_t pts90k, bool decode_only) {
    (void)pts90k;
    assert(video && data && len);
    unsigned index = video->fed_count++ % 8;
    video->fed_tags[index] = data[len - 1];
    video->fed_output_flags[index] = !decode_only;
    if (video->codec == NATIVE_VIDEO_CODEC_H264) {
        for (size_t i = 0; i + 5 <= len; i++) {
            assert(memcmp(data + i, "\0\0\0\1", 4) || (data[i + 4] & 31u) != 14u);
        }
    }
    return NATIVE_VIDEO_OK;
}

static RdpCallbacks callbacks;
static uint32_t now_ms;
static App *demote_on_lock;
int __real_pthread_mutex_lock(pthread_mutex_t *lock);
int __wrap_pthread_mutex_lock(pthread_mutex_t *lock) {
    if (demote_on_lock && lock == &demote_on_lock->video_lock) {
        atomic_store(&demote_on_lock->active_index,1);
        demote_on_lock=NULL;
    }
    return __real_pthread_mutex_lock(lock);
}

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
    callbacks.on_video_au(&app->sessions[index], RDP_VIDEO_CODEC_H264, 0, seed, sizeof(seed), 0);
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

static void test_background_refinement_keeps_h264_classification(void) {
    App app;
    setup(&app);
    NativeSessionSlot *slot = &app.sessions[1];
    assert(native_au_snapshot_arm(&slot->snapshot));
    now_ms = UINT32_MAX - 99u;
    video(&app, 1);
    assert(native_au_snapshot_ready(&slot->snapshot));
    now_ms += 100u;
    bitmap(&app, 1);
    assert(!atomic_load(&slot->video_via_bitmap));
    assert(native_au_snapshot_ready(&slot->snapshot));
    now_ms += 1500u;
    bitmap(&app, 1);
    assert(atomic_load(&slot->video_via_bitmap));
    video(&app, 1);
    atomic_fetch_add(&slot->connect_epoch, 1u);
    bitmap(&app, 1);
    assert(atomic_load(&slot->video_via_bitmap));
    bitmap(&app, 2);
    assert(atomic_load(&app.sessions[2].video_via_bitmap));
    native_au_snapshot_reset(&slot->snapshot);
    teardown(&app);
}

static void test_refinement_overlapping_demotion(void) {
    App app; setup(&app); video(&app,0);
    NativeVideo *decoder=app.video;
    demote_on_lock=&app;
    bitmap(&app,0);
    assert(app.video==decoder && !app.rgba && !atomic_load(&app.sessions[0].video_via_bitmap));
    teardown(&app);
}

static void test_armed_snapshot_blocks_active_refinement(void) {
    App app; setup(&app); video(&app,0);
    NativeVideo *decoder=app.video;
    NativeSessionSlot *slot=&app.sessions[1];
    assert(native_au_snapshot_arm(&slot->snapshot)); video(&app,1);
    atomic_store(&app.active_index,1); bitmap(&app,1);
    assert(app.video==decoder && !app.rgba && !atomic_load(&slot->video_via_bitmap));
    assert(native_au_snapshot_ready(&slot->snapshot));
    native_au_snapshot_reset(&slot->snapshot); teardown(&app);
}

static void test_hevc_switch_and_snapshot_codec(void) {
    App app;
    setup(&app);
    video(&app, 0);
    NativeVideo *previous = app.video;
    const uint8_t delta[] = {0, 0, 1, 2, 1};
    const uint8_t seed[] = {0, 0, 1, 64, 1, 0, 0, 1, 66, 1,
                            0, 0, 1, 68, 1, 0, 0, 1, 38, 1};
    NativeSessionSlot *slot = &app.sessions[1];
    atomic_store(&app.active_index, 1);
    callbacks.on_video_au(slot, RDP_VIDEO_CODEC_H265, 0, delta, sizeof(delta), 0);
    assert(app.video == previous && native_video_codec(app.video) == NATIVE_VIDEO_CODEC_H264);
    callbacks.on_video_au(slot, RDP_VIDEO_CODEC_H265, 0, seed, sizeof(seed), 1);
    assert(slot->diagnostics.bytes == sizeof(delta) + sizeof(seed));
    assert(slot->diagnostics.bytes_auxiliary == 0);
    assert(native_video_codec(app.video) == NATIVE_VIDEO_CODEC_H265);
    assert(app.video_owner_slot == 1);
    video(&app, 1);
    assert(native_video_codec(app.video) == NATIVE_VIDEO_CODEC_H264);

    assert(native_au_snapshot_arm(&slot->snapshot));
    video(&app, 1);
    assert(native_au_snapshot_ready(&slot->snapshot));
    callbacks.on_video_au(slot, RDP_VIDEO_CODEC_H265, 0, delta, sizeof(delta), 2);
    assert(!native_au_snapshot_ready(&slot->snapshot));
    assert(!atomic_load(&slot->snapshot_idr_ready));
    callbacks.on_video_au(slot, RDP_VIDEO_CODEC_H265, 0, seed, sizeof(seed), 3);
    assert(native_au_snapshot_ready(&slot->snapshot) && slot->snapshot.count == 1);
    assert(slot->video_codec == NATIVE_VIDEO_CODEC_H265);
    native_au_snapshot_reset(&slot->snapshot);
    teardown(&app);
}

static void test_all_auxiliary_aus_reach_one_decoder_and_snapshot_in_order(void) {
    App app;
    setup(&app);
    NativeSessionSlot *slot = &app.sessions[0];
    const uint8_t delta[] = {0, 0, 1, 0x41, 2};
    const uint8_t seed[] = {0, 0, 0, 2, 0x67, 1, 0, 0, 0, 2, 0x68, 1,
                            0, 0, 0, 2, 0x65, 3};
    const uint8_t idr[] = {0, 0, 1, 0x65, 4};
    const uint8_t main_delta[] = {0, 0, 1, 0x41, 5};
    video(&app, 0);
    NativeVideo *decoder = app.video;
    callbacks.on_video_au(slot, RDP_VIDEO_CODEC_H264, RDP_VIDEO_FLAG_AVC444_AUX,
                          delta, sizeof(delta), 1);
    assert(atomic_load(&slot->video_ok_frames) == 2);
    callbacks.on_video_au(slot, RDP_VIDEO_CODEC_H264, RDP_VIDEO_FLAG_AVC444_AUX,
                          seed, sizeof(seed), 2);
    callbacks.on_video_au(slot, RDP_VIDEO_CODEC_H264, RDP_VIDEO_FLAG_AVC444_AUX,
                          idr, sizeof(idr), 3);
    callbacks.on_video_au(slot, RDP_VIDEO_CODEC_H264, 0, main_delta, sizeof(main_delta), 4);
    const uint8_t expected[] = {1, 2, 3, 4, 5};
    assert(app.video == decoder && atomic_load(&slot->video_ok_frames) == 5);
    assert(decoder->fed_count == sizeof(expected));
    assert(memcmp(decoder->fed_tags, expected, sizeof(expected)) == 0);
    const uint8_t outputs[] = {1, 0, 0, 0, 1};
    assert(!memcmp(decoder->fed_output_flags, outputs, sizeof(outputs)));

    assert(native_au_snapshot_arm(&slot->snapshot));
    video(&app, 0);
    callbacks.on_video_au(slot, RDP_VIDEO_CODEC_H264, RDP_VIDEO_FLAG_AVC444_AUX,
                          seed, sizeof(seed), 4);
    callbacks.on_video_au(slot, RDP_VIDEO_CODEC_H264, RDP_VIDEO_FLAG_AVC444_AUX,
                          delta, sizeof(delta), 5);
    assert(slot->snapshot.count == 2 && native_au_snapshot_ready(&slot->snapshot));
    assert(slot->snapshot.used == sizeof(seed) + sizeof(delta));
    callbacks.on_video_au(slot, RDP_VIDEO_CODEC_H264, 0, delta, sizeof(delta), 6);
    assert(slot->snapshot.count == 3);
    assert(slot->snapshot.entries[0].decode_only && slot->snapshot.entries[1].decode_only);
    assert(!slot->snapshot.entries[2].decode_only);
    NativeAuSnapshot snapshot = slot->snapshot;
    memset(&slot->snapshot, 0, sizeof(slot->snapshot));
    for (unsigned i = 0; i < snapshot.count; i++) {
        NativeAuSnapshotEntry *entry = &snapshot.entries[i];
        native_video_ingest_au_locked(slot, snapshot.buf + entry->offset, entry->len, entry->pts90k, true, entry->decode_only);
    }
    const uint8_t replay_outputs[] = {0, 0, 1};
    assert(!memcmp(decoder->fed_output_flags + 5, replay_outputs, sizeof(replay_outputs)));
    native_au_snapshot_reset(&snapshot);
    native_au_snapshot_reset(&slot->snapshot);
    teardown(&app);
}

static void test_refresh_rejects_old_frames_before_matching_ack(void) {
    for (int snapshot = 0; snapshot < 2; snapshot++) {
        App app;
        setup(&app);
        NativeSessionSlot *slot = &app.sessions[0];
        if (snapshot) assert(native_au_snapshot_arm(&slot->snapshot));
        slot->refresh_cookie = 7;
        slot->refresh_waiting = true;
        for (uint32_t cookie = 0; cookie < 7; cookie++) {
            callbacks.on_refresh_started(slot, cookie);
            video(&app, 0);
            bitmap(&app, 0);
            assert(slot->refresh_waiting);
            assert(!atomic_load(&slot->snapshot_idr_ready));
            assert(slot->snapshot.count == 0);
            assert(atomic_load(&slot->video_ok_frames) == 0);
        }
        callbacks.on_refresh_started(slot, 7);
        assert(!slot->refresh_waiting);
        video(&app, 0);
        if (snapshot) {
            assert(native_au_snapshot_ready(&slot->snapshot));
            assert(atomic_load(&slot->snapshot_idr_ready));
        } else {
            assert(atomic_load(&slot->video_ok_frames) == 1);
        }
        native_au_snapshot_reset(&slot->snapshot);
        teardown(&app);
    }
}

static void test_diagnostics_separates_auxiliary_and_replay(void) {
    App app;
    setup(&app);
    NativeSessionSlot *slot = &app.sessions[0];
    static const uint8_t seed[] = {0,0,1,0x67,1,0,0,1,0x68,1,0,0,1,0x65,1};
    static const uint8_t delta[] = {0,0,1,0x41,1};
    video(&app, 0);
    callbacks.on_video_au(slot, RDP_VIDEO_CODEC_H264, RDP_VIDEO_FLAG_AVC444_AUX, delta, sizeof(delta), 1);
    assert(slot->diagnostics.received == 2 && slot->diagnostics.accepted == 2);
    assert(slot->diagnostics.received_auxiliary == 1);
    assert(slot->diagnostics.bytes == sizeof(seed) + sizeof(delta));
    assert(slot->diagnostics.bytes_auxiliary == sizeof(delta));
    assert(slot->diagnostics.auxiliary && slot->diagnostics.last_au_ms == now_ms);
    callbacks.on_video_au(slot, RDP_VIDEO_CODEC_H264, RDP_VIDEO_FLAG_AVC444_AUX, seed, sizeof(seed), 2);
    assert(slot->diagnostics.received == 3 && slot->diagnostics.accepted == 3);
    assert(slot->diagnostics.received_auxiliary == 2);
    pthread_mutex_lock(&app.video_lock);
    native_video_ingest_au_locked(slot, seed, sizeof(seed), 3, true, false);
    pthread_mutex_unlock(&app.video_lock);
    assert(slot->diagnostics.received == 3 && slot->diagnostics.accepted == 3 && slot->diagnostics.replayed == 1);
    assert(slot->diagnostics.bytes == 2 * sizeof(seed) + sizeof(delta));
    assert(slot->diagnostics.bytes_auxiliary == sizeof(seed) + sizeof(delta));
    native_slot_record_state(slot, RDP_STATE_RECONNECTING, RDP_DISCONNECT_NONE);
    assert(!slot->diagnostics.received && !slot->diagnostics.accepted && !slot->diagnostics.has_au);
    assert(!slot->diagnostics.received_auxiliary);
    assert(!slot->diagnostics.bytes && !slot->diagnostics.bytes_auxiliary);
    teardown(&app);
}

int main(void) {
    test_diagnostics_separates_auxiliary_and_replay();
    test_refresh_rejects_old_frames_before_matching_ack();
    test_all_auxiliary_aus_reach_one_decoder_and_snapshot_in_order();
    test_refinement_hold_requires_same_slot_and_epoch();
    test_handoffs_do_not_trip_codec_flip_limit();
    test_same_connection_still_limits_codec_flips();
    test_refinement_hold_across_clock_wrap();
    test_background_refinement_keeps_h264_classification();
    test_refinement_overlapping_demotion();
    test_armed_snapshot_blocks_active_refinement();
    test_hevc_switch_and_snapshot_codec();
    return 0;
}
