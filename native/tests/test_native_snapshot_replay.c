#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "native_app.h"
#include "native_switch_internal.h"
#include "native_rdp_video.h"
#include "native_rdp_media.h"
#include "native_rgba_owner.h"
#include "native_rdp_state.h"
#include "h264_annexb.h"
struct NativeVideo { uint16_t w,h; };
static App app;
static pthread_mutex_t schedule = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static bool go, attempted;
static unsigned order[3], count, resumes, suppresses, refreshes, reconnects;
static bool wire_suppressed;
static bool barrier_ready = true;
static int completed_switch;
static uint32_t ticks;
static const uint8_t seed[]={0,0,1,0x67,1,0,0,1,0x68,1,0,0,1,0x65,1};
static const uint8_t delta2[]={0,0,1,0x41,2}, delta3[]={0,0,1,0x41,3};
NativeH264Result __real_native_h264_scan_au(const uint8_t *,size_t,NativeH264Info *);
NativeH264Result __wrap_native_h264_scan_au(const uint8_t *data, size_t len, NativeH264Info *info) {
    if (data && len && data[len - 1u] == 2u) {
        /* The second cached AU must still own the batch lock. */
        assert(pthread_mutex_trylock(&app.video_lock) == EBUSY);
        pthread_mutex_lock(&schedule);
        go = true;
        pthread_cond_broadcast(&changed);
        while (!attempted) {
            pthread_cond_wait(&changed, &schedule);
        }
        pthread_mutex_unlock(&schedule);
    }
    return __real_native_h264_scan_au(data, len, info);
}

static void *late_tail(void *unused) {
    (void)unused;
    pthread_mutex_lock(&schedule);
    while (!go) {
        pthread_cond_wait(&changed, &schedule);
    }
    attempted = true;
    pthread_cond_broadcast(&changed);
    pthread_mutex_unlock(&schedule);
    native_video_ingest_au(&app.sessions[0], NATIVE_VIDEO_CODEC_H264, delta3, sizeof(delta3), 3, false);
    return NULL;
}

NativeVideoResult native_video_feed(NativeVideo *v, const uint8_t *p, size_t n, uint64_t t, bool decode_only) {
    (void)v;
    (void)t;
    (void)decode_only;
    assert(count<3);
    order[count++]=p[n-1];
    return NATIVE_VIDEO_OK;
}

uint16_t native_video_width(const NativeVideo *v) {
    return v->w;
}

uint16_t native_video_height(const NativeVideo *v) {
    return v->h;
}

NativeVideoCodec native_video_codec(const NativeVideo *v) {
    (void)v;
    return NATIVE_VIDEO_CODEC_H264;
}

void native_video_close(NativeVideo *v) {
    (void)v;
}

NativeVideo *native_video_open(NativeMedia *m, uint16_t w, uint16_t h, uint16_t f, NativeVideoCodec codec) {
    (void)m;
    (void)w;
    (void)h;
    (void)f;
    (void)codec;
    abort();
}

NativeMedia *native_ensure_media_locked(App *a) {
    (void)a;
    abort();
}

void native_open_speculative_audio_locked(App *a) {
    (void)a;
    abort();
}

bool native_rgba_owner_matches(const App *a, int i, unsigned e) {
    (void)a;
    (void)i;
    (void)e;
    return false;
}

void native_close_rgba_locked(App *a, bool b) {
    (void)a;
    (void)b;
    abort();
}

void native_slot_report_terminal(NativeSessionSlot *s, RdpState r, int e) {
    (void)s;
    (void)r;
    (void)e;
    abort();
}

int rdp_state_exit_code(RdpState s) {
    (void)s;
    return 11;
}

const char *native_session_slot_name(int i) {
    (void)i;
    return "slot";
}

static void setup(struct NativeVideo *video) {
    memset(&app, 0, sizeof(app));
    assert(pthread_mutex_init(&app.video_lock, NULL) == 0);
    atomic_store(&app.running, true);
    app.video = video;
    app.video_owner_slot = 1;
    app.video_owner_epoch = 1;
    app.sessions[0].app = &app;
    atomic_store(&app.sessions[0].connect_epoch, 1);
    atomic_store(&app.sessions[0].desktop_width, 16);
    atomic_store(&app.sessions[0].desktop_height, 16);
    NativeAuSnapshot *snapshot = &app.sessions[0].snapshot;
    assert(native_au_snapshot_arm(snapshot));
    assert(native_au_snapshot_append(snapshot, seed, sizeof(seed), true, true, 1, false));
    assert(native_au_snapshot_append(snapshot, delta2, sizeof(delta2), true, false, 2, false));
    atomic_store(&app.sessions[0].snapshot_idr_ready, true);
    atomic_store(&app.sessions[0].snapshot_last_au_ms, native_monotonic_ms() - 1000);
    count = resumes = suppresses = refreshes = reconnects = 0;
    wire_suppressed = false;
    completed_switch = -1;
    ticks = 10000;
    app.pending_switch_target = -1;
    go = attempted = false;
}

static void test_late_tail_waits_for_entire_replay(void) {
    struct NativeVideo video = {16, 16};
    setup(&video);
    pthread_t worker;
    assert(pthread_create(&worker, NULL, late_tail, NULL) == 0);
    assert(native_snapshot_replay(&app, 0));
    assert(pthread_join(worker, NULL) == 0);
    assert(count == 3 && order[0] == 1 && order[1] == 2 && order[2] == 3);
    assert(!app.sessions[0].snapshot.armed);
    assert(pthread_mutex_destroy(&app.video_lock) == 0);
}

static void test_tail_before_replay_stays_in_cache_and_invalidates_quiet(void) {
    struct NativeVideo video = {16, 16};
    setup(&video);
    native_video_ingest_au(&app.sessions[0], NATIVE_VIDEO_CODEC_H264, delta3, sizeof(delta3), 3, false);
    assert(count == 0 && app.sessions[0].snapshot.count == 3u);
    assert(!native_snapshot_replay(&app, 0));
    assert(count == 0 && !app.sessions[0].snapshot.armed);
    assert(pthread_mutex_destroy(&app.video_lock) == 0);
}

uint32_t SDL_GetTicks(void) { return ticks; }

void rdp_set_suppress_output(RdpSession *session, bool allow_display) {
    assert(session == app.sessions[0].rdp);
    wire_suppressed = !allow_display;
    if (allow_display) resumes++;
    else suppresses++;
}

void rdp_request_refresh(RdpSession *session, uint32_t cookie) {
    assert(cookie == app.sessions[0].refresh_cookie);
    assert(app.sessions[0].refresh_waiting);
    assert(session == app.sessions[0].rdp && !wire_suppressed);
    assert(app.sessions[0].snapshot.armed);
    refreshes++;
}

bool native_slot_connect(App *a, int index, bool arm_snapshot) {
    assert(a == &app && index == 0 && arm_snapshot);
    reconnects++;
    a->sessions[index].refresh_waiting = false;
    return native_au_snapshot_arm(&a->sessions[index].snapshot);
}

bool native_complete_session_switch(App *a, int target) {
    assert(a == &app);
    completed_switch = target;
    a->pending_switch_target = -1;
    a->pending_switch_graphics = false;
    return true;
}

static void test_background_prefetch_preserves_the_first_idr_replay_path(void) {
    for (int hub_presuppressed = 0; hub_presuppressed < 2; hub_presuppressed++) {
        struct NativeVideo video = {16, 16};
        setup(&video);
        NativeSessionSlot *slot = &app.sessions[0];
        slot->rdp = (RdpSession *)slot;
        atomic_store(&slot->current_state, RDP_STATE_ACTIVE);
        atomic_store(&app.active_index, 1);
        wire_suppressed = hub_presuppressed != 0;
        native_background_slot(&app, 0);
        assert(slot->snapshot_pending && !slot->suppressed);
        assert(slot->snapshot.armed && slot->snapshot.count == 0);
        assert(!atomic_load(&slot->snapshot_idr_ready));
        assert(resumes == 1 && refreshes == 1 && !reconnects && !suppresses);
        native_background_slot(&app, 0);
        assert(resumes == 1 && refreshes == 1);

        native_video_ingest_au(slot, NATIVE_VIDEO_CODEC_H264, seed, sizeof(seed), 0, false);
        native_switch_tick(&app);
        assert(slot->snapshot_pending && !slot->suppressed);
        assert(slot->snapshot.count == 0 && !atomic_load(&slot->snapshot_idr_ready));
        slot->refresh_waiting = false; /* worker acknowledgement */
        native_video_ingest_au(slot, NATIVE_VIDEO_CODEC_H264, seed, sizeof(seed), 1, false);
        native_switch_tick(&app);
        assert(!slot->snapshot_pending && slot->suppressed);
        assert(suppresses == 1 && count == 0);
        native_video_ingest_au(slot, NATIVE_VIDEO_CODEC_H264, seed, sizeof(seed), 2, false);
        assert(slot->snapshot.count == 1);
        app.pending_switch_target = 0;
        app.pending_switch_deadline_ticks = ticks + 8000;
        native_switch_tick(&app);
        assert(completed_switch == -1);
        atomic_store(&slot->snapshot_last_au_ms, native_monotonic_ms() - 1000);
        native_switch_tick(&app);
        assert(completed_switch == 0 && refreshes == 1 && resumes == 1);
        atomic_store(&app.active_index, 0);
        assert(native_snapshot_replay(&app, 0));
        assert(count == 1 && !slot->snapshot.armed);
        assert(pthread_mutex_destroy(&app.video_lock) == 0);
    }
}

static void test_background_prefetch_keeps_other_background_paths(void) {
    for (int scenario = 0; scenario < 4; scenario++) {
        struct NativeVideo video = {16, 16};
        setup(&video);
        NativeSessionSlot *slot = &app.sessions[0];
        native_au_snapshot_reset(&slot->snapshot);
        slot->rdp = (RdpSession *)slot;
        atomic_store(&slot->current_state, scenario == 0 ? RDP_STATE_CONNECTING : RDP_STATE_ACTIVE);
        atomic_store(&slot->video_via_bitmap, scenario == 1);
        slot->refresh_ineffective = scenario == 2;
        slot->suppressed = scenario == 3;
        native_background_slot(&app, 0);
        assert(refreshes == 0 && resumes == 0);
        if (scenario == 2) {
            assert(reconnects == 1 && slot->snapshot_pending && slot->snapshot.armed);
        } else {
            assert(!reconnects && !slot->snapshot_pending && !slot->snapshot.armed);
            assert(slot->suppressed && suppresses == (scenario == 3 ? 0u : 1u));
        }
        native_au_snapshot_reset(&slot->snapshot);
        assert(pthread_mutex_destroy(&app.video_lock) == 0);
    }
}

static void test_background_prefetch_timeout_remains_bounded(void) {
    struct NativeVideo video = {16, 16};
    setup(&video);
    NativeSessionSlot *slot = &app.sessions[0];
    slot->rdp = (RdpSession *)slot;
    atomic_store(&slot->current_state, RDP_STATE_ACTIVE);
    atomic_store(&app.active_index, 1);
    native_background_slot(&app, 0);
    native_switch_tick(&app);
    ticks += NATIVE_SWITCH_KEYFRAME_TIMEOUT_MS + 1;
    native_switch_tick(&app);
    assert(slot->suppressed && !slot->snapshot_pending && !slot->snapshot.armed);
    assert(suppresses == 1 && refreshes == 1 && !reconnects);
    assert(pthread_mutex_destroy(&app.video_lock) == 0);
}

static void test_active_watchdog_rejects_pre_refresh_frame(void) {
    struct NativeVideo video = {16, 16};
    setup(&video);
    NativeSessionSlot *slot = &app.sessions[0];
    native_au_snapshot_reset(&slot->snapshot);
    slot->rdp = (RdpSession *)slot;
    atomic_store(&slot->current_state, RDP_STATE_ACTIVE);
    atomic_store(&app.active_index, 0);
    native_switch_watchdog_baseline(&app, slot);
    native_switch_watchdog_start(&app, 2000);
    native_video_ingest_au(slot, NATIVE_VIDEO_CODEC_H264, seed, sizeof(seed), 0, false);
    native_switch_tick(&app);
    assert(count == 1 && app.switch_deadline_ticks != 0);
    slot->refresh_waiting = false;
    native_video_ingest_au(slot, NATIVE_VIDEO_CODEC_H264, seed, sizeof(seed), 1, false);
    native_switch_tick(&app);
    assert(count == 2 && app.switch_deadline_ticks == 0);
    assert(pthread_mutex_destroy(&app.video_lock) == 0);
}

static void test_replay_timeout_preserves_snapshot(void) {
    struct NativeVideo video = {16, 16};
    setup(&video);
    bool armed = app.sessions[0].snapshot.armed;
    uint8_t *buffer = app.sessions[0].snapshot.buf;
    barrier_ready = false;
    assert(!native_snapshot_replay(&app, 0));
    assert(app.sessions[0].snapshot.armed == armed);
    assert(app.sessions[0].snapshot.buf == buffer);
    assert(count == 0);
    barrier_ready = true;
    native_au_snapshot_reset(&app.sessions[0].snapshot);
    assert(pthread_mutex_destroy(&app.video_lock) == 0);
}

int main(void) {
    test_replay_timeout_preserves_snapshot();
    test_active_watchdog_rejects_pre_refresh_frame();
    test_background_prefetch_preserves_the_first_idr_replay_path();
    test_background_prefetch_keeps_other_background_paths();
    test_background_prefetch_timeout_remains_bounded();
    test_late_tail_waits_for_entire_replay();
    test_tail_before_replay_stays_in_cache_and_invalidates_quiet();
    puts("PASS native-snapshot-replay");
    return 0;
}

bool rdp_graphics_barrier(RdpSession *session, uint32_t timeout_ms) {
    (void)session;
    assert(timeout_ms > 0);
    assert(pthread_mutex_trylock(&app.video_lock) == 0);
    pthread_mutex_unlock(&app.video_lock);
    return barrier_ready;
}
