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
static unsigned order[3], count;
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
    native_video_ingest_au(&app.sessions[0], NATIVE_VIDEO_CODEC_H264, delta3, sizeof(delta3), 3);
    return NULL;
}

NativeVideoResult native_video_feed(NativeVideo *v, const uint8_t *p, size_t n, uint64_t t) {
    (void)v;
    (void)t;
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
    assert(native_au_snapshot_append(snapshot, seed, sizeof(seed), true, true, 1));
    assert(native_au_snapshot_append(snapshot, delta2, sizeof(delta2), true, false, 2));
    atomic_store(&app.sessions[0].snapshot_idr_ready, true);
    atomic_store(&app.sessions[0].snapshot_last_au_ms, native_monotonic_ms() - 1000);
    count = 0;
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
    native_video_ingest_au(&app.sessions[0], NATIVE_VIDEO_CODEC_H264, delta3, sizeof(delta3), 3);
    assert(count == 0 && app.sessions[0].snapshot.count == 3u);
    assert(!native_snapshot_replay(&app, 0));
    assert(count == 0 && !app.sessions[0].snapshot.armed);
    assert(pthread_mutex_destroy(&app.video_lock) == 0);
}

int main(void) {
    test_late_tail_waits_for_entire_replay();
    test_tail_before_replay_stays_in_cache_and_invalidates_quiet();
    puts("PASS native-snapshot-replay");
    return 0;
}
