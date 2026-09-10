#ifndef LGNOME_AU_SNAPSHOT_H
#define LGNOME_AU_SNAPSHOT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Compressed seed/delta cache for decoder replay. Caller serializes access with video_lock.
 * See docs/native-runbook.md, Multi-RDP Sessions, for backgrounding and replay policy. */

#define NATIVE_AU_SNAPSHOT_MAX_BYTES (8u * 1024u * 1024u)
#define NATIVE_AU_SNAPSHOT_MAX_AUS 512u

typedef struct NativeAuSnapshotEntry {
    size_t offset; /* into NativeAuSnapshot.buf */
    size_t len;
    uint64_t pts90k;
} NativeAuSnapshotEntry;

typedef struct NativeAuSnapshot {
    uint8_t *buf; /* NATIVE_AU_SNAPSHOT_MAX_BYTES once armed; NULL otherwise */
    size_t used;
    NativeAuSnapshotEntry entries[NATIVE_AU_SNAPSHOT_MAX_AUS];
    unsigned count;
    bool armed;            /* appends are accepted */
    bool has_decoder_seed; /* an AU carrying SPS + PPS before IDR seeded the snapshot */
} NativeAuSnapshot;

/* Empties the snapshot and (re)allocates the byte buffer; appends are accepted until the
 * snapshot is reset or overflows. Returns false when the allocation fails (the snapshot
 * stays disarmed). */
bool native_au_snapshot_arm(NativeAuSnapshot *snap);

/* Drops everything and frees the buffer. Safe on a zeroed or already-reset snapshot. */
void native_au_snapshot_reset(NativeAuSnapshot *snap);

/* Caches one compressed AU. valid_au and decoder_seed are the canonical results from
 * the native dual-framing scan performed before routing. A decoder seed restarts the
 * snapshot; valid deltas/config AUs before the first seed are skipped. A malformed AU
 * after a seed voids the cached chain, while one before the seed is skipped. Returns
 * false only when this call voided the snapshot (malformed AU or capacity overflow). */
bool native_au_snapshot_append(NativeAuSnapshot *snap, const uint8_t *data, size_t len, bool valid_au,
                               bool decoder_seed, uint64_t pts90k);

/* True when a replay would rebuild decodable state: armed and seeded with a restart AU. */
bool native_au_snapshot_ready(const NativeAuSnapshot *snap);

#endif
