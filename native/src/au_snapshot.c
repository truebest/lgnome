#include "au_snapshot.h"

#include <stdlib.h>
#include <string.h>

#include "clog.h"

clog_define(g_native_log_video, cLogLevelInfo, "video.snapshot");

bool native_au_snapshot_arm(NativeAuSnapshot *snap) {
    if (!snap) {
        return false;
    }
    native_au_snapshot_reset(snap);
    snap->buf = (uint8_t *)malloc(NATIVE_AU_SNAPSHOT_MAX_BYTES);
    if (!snap->buf) {
        clog(cLogLevelWarning, "failed to allocate %zu-byte AU snapshot",
             (size_t)NATIVE_AU_SNAPSHOT_MAX_BYTES);
        return false;
    }
    snap->armed = true;
    return true;
}

void native_au_snapshot_reset(NativeAuSnapshot *snap) {
    if (!snap) {
        return;
    }
    free(snap->buf);
    snap->buf = NULL;
    snap->used = 0;
    snap->count = 0;
    snap->armed = false;
    snap->has_decoder_seed = false;
}

bool native_au_snapshot_append(NativeAuSnapshot *snap, const uint8_t *data, size_t len, bool valid_au,
                               bool decoder_seed, uint64_t pts90k) {
    if (!snap || !snap->armed) {
        return false;
    }
    if (!valid_au || !data || len == 0) {
        if (!snap->has_decoder_seed) {
            return true;
        }
        native_au_snapshot_reset(snap);
        return false;
    }
    if (decoder_seed) {
        /* Only the newest decoder-seed group matters; older content is obsolete. */
        snap->used = 0;
        snap->count = 0;
        snap->has_decoder_seed = true;
    } else if (!snap->has_decoder_seed) {
        /* A delta before the first restart point is undecodable alone, so skip it.
         * Once seeded, later config-only AUs append as ordinary chain members. */
        return true;
    }
    if (len > NATIVE_AU_SNAPSHOT_MAX_BYTES - snap->used || snap->count >= NATIVE_AU_SNAPSHOT_MAX_AUS) {
        /* A partial delta chain is undecodable, so overflow voids the whole snapshot. */
        native_au_snapshot_reset(snap);
        return false;
    }
    memcpy(snap->buf + snap->used, data, len);
    snap->entries[snap->count].offset = snap->used;
    snap->entries[snap->count].len = len;
    snap->entries[snap->count].pts90k = pts90k;
    snap->used += len;
    snap->count++;
    return true;
}

bool native_au_snapshot_ready(const NativeAuSnapshot *snap) {
    return snap && snap->armed && snap->has_decoder_seed && snap->count > 0;
}
