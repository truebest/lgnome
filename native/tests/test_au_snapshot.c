#include "au_snapshot.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void fill(uint8_t *buf, size_t len, uint8_t seed) {
    for (size_t i = 0; i < len; i++) {
        buf[i] = (uint8_t)(seed + i);
    }
}

/* Formats buf as one AVC length-prefixed NAL so the cached test bytes remain realistic.
 * AU validity and decoder-seed classification are supplied by the ingest path. */
static void format_nal_au(uint8_t *buf, size_t len, uint8_t nal_type) {
    assert(len >= 5);
    uint32_t nal_len = (uint32_t)(len - 4);
    buf[0] = (uint8_t)(nal_len >> 24);
    buf[1] = (uint8_t)(nal_len >> 16);
    buf[2] = (uint8_t)(nal_len >> 8);
    buf[3] = (uint8_t)nal_len;
    buf[4] = (uint8_t)(0x60u | nal_type);
}

static void test_unarmed_and_basic_append(void) {
    NativeAuSnapshot snap;
    memset(&snap, 0, sizeof(snap));
    assert(!native_au_snapshot_ready(&snap));

    uint8_t idr[16];
    fill(idr, sizeof(idr), 1);
    format_nal_au(idr, sizeof(idr), 5);
    /* Unarmed appends are rejected without effect. */
    assert(!native_au_snapshot_append(&snap, idr, sizeof(idr), true, true, 0));
    assert(snap.count == 0 && snap.buf == NULL);

    assert(native_au_snapshot_arm(&snap));
    assert(snap.armed && snap.buf != NULL && !native_au_snapshot_ready(&snap));

    /* Deltas before the first decoder seed are skipped but do not invalidate. */
    uint8_t delta[8];
    fill(delta, sizeof(delta), 7);
    assert(native_au_snapshot_append(&snap, delta, sizeof(delta), true, false, 100));
    assert(snap.count == 0 && !native_au_snapshot_ready(&snap));

    /* Empty pushes are ignored, valid either way. */
    assert(native_au_snapshot_append(&snap, NULL, 0, false, false, 0));
    assert(snap.count == 0);

    /* A decoder seed starts the snapshot; a delta appends after it. */
    assert(native_au_snapshot_append(&snap, idr, sizeof(idr), true, true, 200));
    assert(native_au_snapshot_ready(&snap));
    assert(native_au_snapshot_append(&snap, delta, sizeof(delta), true, false, 300));
    assert(snap.count == 2 && snap.used == sizeof(idr) + sizeof(delta));
    assert(snap.entries[0].len == sizeof(idr) && snap.entries[0].pts90k == 200);
    assert(snap.entries[1].len == sizeof(delta) && snap.entries[1].pts90k == 300);
    assert(memcmp(snap.buf + snap.entries[0].offset, idr, sizeof(idr)) == 0);
    assert(memcmp(snap.buf + snap.entries[1].offset, delta, sizeof(delta)) == 0);

    native_au_snapshot_reset(&snap);
    assert(snap.buf == NULL && !snap.armed && !native_au_snapshot_ready(&snap));
    /* Double reset is safe. */
    native_au_snapshot_reset(&snap);
}

static void test_decoder_seed_restarts(void) {
    NativeAuSnapshot snap;
    memset(&snap, 0, sizeof(snap));
    assert(native_au_snapshot_arm(&snap));

    uint8_t first[32], tail[4], second[24];
    fill(first, sizeof(first), 10);
    fill(tail, sizeof(tail), 20);
    fill(second, sizeof(second), 30);
    format_nal_au(first, sizeof(first), 5);
    format_nal_au(second, sizeof(second), 5);

    assert(native_au_snapshot_append(&snap, first, sizeof(first), true, true, 1));
    assert(native_au_snapshot_append(&snap, tail, sizeof(tail), true, false, 2));
    assert(native_au_snapshot_append(&snap, tail, sizeof(tail), true, false, 3));
    assert(snap.count == 3);

    /* A newer decoder seed drops everything before it. */
    assert(native_au_snapshot_append(&snap, second, sizeof(second), true, true, 4));
    assert(native_au_snapshot_ready(&snap));
    assert(snap.count == 1 && snap.used == sizeof(second));
    assert(snap.entries[0].offset == 0 && snap.entries[0].pts90k == 4);
    assert(memcmp(snap.buf, second, sizeof(second)) == 0);

    native_au_snapshot_reset(&snap);
}

static void test_entry_overflow_invalidates(void) {
    NativeAuSnapshot snap;
    memset(&snap, 0, sizeof(snap));
    assert(native_au_snapshot_arm(&snap));

    uint8_t idr[8];
    fill(idr, sizeof(idr), 3);
    format_nal_au(idr, sizeof(idr), 5);
    uint8_t au[4];
    fill(au, sizeof(au), 5);
    assert(native_au_snapshot_append(&snap, idr, sizeof(idr), true, true, 0));
    for (unsigned i = 1; i < NATIVE_AU_SNAPSHOT_MAX_AUS; i++) {
        assert(native_au_snapshot_append(&snap, au, sizeof(au), true, false, i));
    }
    assert(snap.count == NATIVE_AU_SNAPSHOT_MAX_AUS);

    /* One entry past the cap voids the snapshot: a partial chain is undecodable. */
    assert(!native_au_snapshot_append(&snap, au, sizeof(au), true, false, 999));
    assert(!snap.armed && !native_au_snapshot_ready(&snap) && snap.buf == NULL);
}

static void test_byte_overflow_invalidates(void) {
    NativeAuSnapshot snap;
    memset(&snap, 0, sizeof(snap));
    uint8_t *big = (uint8_t *)malloc(NATIVE_AU_SNAPSHOT_MAX_BYTES);
    assert(big);
    memset(big, 0xAB, NATIVE_AU_SNAPSHOT_MAX_BYTES);

    /* A keyframe of exactly the cap fits... */
    assert(native_au_snapshot_arm(&snap));
    format_nal_au(big, NATIVE_AU_SNAPSHOT_MAX_BYTES, 5);
    assert(native_au_snapshot_append(&snap, big, NATIVE_AU_SNAPSHOT_MAX_BYTES, true, true, 0));
    assert(native_au_snapshot_ready(&snap) && snap.used == NATIVE_AU_SNAPSHOT_MAX_BYTES);
    /* ...and the next byte voids the snapshot. */
    uint8_t one = 0xCD;
    assert(!native_au_snapshot_append(&snap, &one, 1, true, false, 1));
    assert(!snap.armed && snap.buf == NULL);

    /* A delta that would cross the cap voids the snapshot too. */
    assert(native_au_snapshot_arm(&snap));
    format_nal_au(big, NATIVE_AU_SNAPSHOT_MAX_BYTES - 1, 5);
    assert(native_au_snapshot_append(&snap, big, NATIVE_AU_SNAPSHOT_MAX_BYTES - 1, true, true, 0));
    assert(!native_au_snapshot_append(&snap, big, 2, true, false, 1));
    assert(!snap.armed && snap.buf == NULL);

    native_au_snapshot_reset(&snap);
    free(big);
}

/* The ingest path supplies canonical validity and decoder-seed verdicts. Only a
 * decoder seed may start the cache; byte framing stays outside this module. */
static void test_seed_and_invalid_verdicts(void) {
    NativeAuSnapshot snap;
    memset(&snap, 0, sizeof(snap));
    assert(native_au_snapshot_arm(&snap));

    /* Standalone SPS: skipped, not a seed. */
    uint8_t sps[12];
    fill(sps, sizeof(sps), 40);
    format_nal_au(sps, sizeof(sps), 7);
    assert(native_au_snapshot_append(&snap, sps, sizeof(sps), true, false, 1));
    assert(snap.count == 0 && !native_au_snapshot_ready(&snap));

    /* Unparseable bytes likewise cannot seed when the scanner reported no IDR. */
    uint8_t garbage[16];
    fill(garbage, sizeof(garbage), 0xF0);
    assert(native_au_snapshot_append(&snap, garbage, sizeof(garbage), false, false, 2));
    assert(snap.count == 0 && !native_au_snapshot_ready(&snap));

    /* An AU classified as carrying SPS + PPS before IDR seeds normally. */
    uint8_t bundle[12 + 12 + 20];
    fill(bundle, sizeof(bundle), 50);
    format_nal_au(bundle, 12, 7);                       /* SPS */
    format_nal_au(bundle + 12, 12, 8);                  /* PPS */
    format_nal_au(bundle + 24, sizeof(bundle) - 24, 5); /* IDR */
    assert(native_au_snapshot_append(&snap, bundle, sizeof(bundle), true, true, 3));
    assert(snap.count == 1 && native_au_snapshot_ready(&snap));

    /* A config-only AU after the seed joins the chain instead of restarting it. */
    assert(native_au_snapshot_append(&snap, sps, sizeof(sps), true, false, 4));
    assert(snap.count == 2 && native_au_snapshot_ready(&snap));
    assert(snap.entries[0].pts90k == 3 && snap.entries[1].pts90k == 4);

    /* A malformed AU after the seed breaks the cached delta chain and voids it. */
    assert(!native_au_snapshot_append(&snap, garbage, sizeof(garbage), false, false, 5));
    assert(!snap.armed && snap.buf == NULL && !native_au_snapshot_ready(&snap));
    native_au_snapshot_reset(&snap);
}

int main(void) {
    test_unarmed_and_basic_append();
    test_decoder_seed_restarts();
    test_entry_overflow_invalidates();
    test_byte_overflow_invalidates();
    test_seed_and_invalid_verdicts();
    printf("au-snapshot: OK\n");
    return 0;
}
