/* Background stream recovery and IDR-snapshot preparation for slot switching.
 * SDL thread only; see native_switch.c for the coordinator. */
#ifdef LGNOME_TARGET_WEBOS

#include "native_switch_internal.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "native_app.h"
#include "native_rdp_state.h"
#include "native_rdp_video.h"
#include "native_session.h"

#include "clog.h"

clog_define(g_native_log_switch_recovery, cLogLevelInfo, "native");

/* Suppress background graphics, or reconnect off-screen to cache an IDR if refresh is ineffective. */

void native_background_slot(App *app, int index) {
    NativeSessionSlot *slot = &app->sessions[index];
    if (!slot->rdp || slot->suppressed) {
        return;
    }
    bool snapshot_wanted = (slot->refresh_ineffective || app->snapshot_force) &&
                           atomic_load(&slot->current_state) == (int)RDP_STATE_ACTIVE &&
                           !NATIVE_SWITCH_TEST_NO_RECONNECT;
    if (snapshot_wanted) {
        clog(cLogLevelNotice, "reconnecting the %s session in the background to cache a fresh IDR",
             native_session_slot_name(index));
        /* The snapshot is armed inside the connect, between the old worker's teardown
         * and the new worker's start, so the connect IDR cannot race an unarmed cache. */
        if (!native_slot_connect(app, index, true)) {
            /* Startup failure: route it through the standard failure drain (per-slot
             * status line, quiet background teardown) instead of leaving a dead handle. */
            native_slot_report_terminal(slot, RDP_STATE_NETWORK_ERROR,
                                        rdp_state_exit_code(RDP_STATE_NETWORK_ERROR));
            return;
        }
        pthread_mutex_lock(&app->video_lock);
        bool armed = slot->snapshot.armed;
        pthread_mutex_unlock(&app->video_lock);
        if (armed) {
            slot->snapshot_pending = true; /* the switch tick suppresses once the IDR lands */
            slot->snapshot_deadline_ticks = 0;
        } else {
            /* No memory for the cache: background it plainly; switching back takes the
             * usual refresh-or-reconnect path. */
            clog(cLogLevelWarning, "%s AU snapshot allocation failed; backgrounding without a cache",
                 native_session_slot_name(index));
            rdp_set_suppress_output(slot->rdp, false);
            slot->suppressed = true;
        }
        return;
    }
    /* Works for a still-CONNECTING slot too: the Rust worker buffers control commands
     * and replays them once the session goes active, so a slot left mid-handshake still
     * comes up backgrounded (suppressed) instead of streaming video nobody displays. */
    rdp_set_suppress_output(slot->rdp, false);
    slot->suppressed = true;
}

/* SDL thread: replay only while target is active, suppressed and quiet.
 * Returns true if the decoder accepted the replay; resume output afterwards. */
bool native_snapshot_replay(App *app, int target) {
    NativeSessionSlot *slot = &app->sessions[target];
    if (!atomic_load(&slot->snapshot_idr_ready)) {
        return false;
    }
    NativeAuSnapshot snap;
    pthread_mutex_lock(&app->video_lock);
    snap = slot->snapshot; /* take ownership of the buffer */
    memset(&slot->snapshot, 0, sizeof(slot->snapshot));
    pthread_mutex_unlock(&app->video_lock);
    atomic_store(&slot->snapshot_idr_ready, false);
    if (!native_au_snapshot_ready(&snap)) {
        free(snap.buf);
        return false;
    }
    uint32_t since_last_au = native_monotonic_ms() - atomic_load(&slot->snapshot_last_au_ms);
    if (since_last_au < NATIVE_SNAPSHOT_QUIET_MS) {
        clog(cLogLevelDebug, "%s streamed %ums ago, may still have AUs in flight; skipping replay",
             native_session_slot_name(target), (unsigned)since_last_au);
        free(snap.buf);
        return false;
    }
    clog(cLogLevelDebug, "replaying %u cached %s AUs (%zu bytes) to re-enter the delta chain",
         snap.count, native_session_slot_name(target), snap.used);
    unsigned baseline = atomic_load(&slot->video_ok_frames);
    for (unsigned i = 0; i < snap.count; i++) {
        const NativeAuSnapshotEntry *entry = &snap.entries[i];
        native_video_ingest_au(slot, snap.buf + entry->offset, entry->len, entry->pts90k);
        if (!atomic_load(&app->running) || atomic_load(&slot->session_failed)) {
            break; /* a terminal feed error stopped the slot (or app); the drain owns it */
        }
    }
    free(snap.buf);
    if (atomic_load(&slot->video_ok_frames) == baseline) {
        clog(cLogLevelWarning, "%s snapshot replay fed no frames; falling back",
             native_session_slot_name(target));
        return false;
    }
    return true;
}

/* Prepare the target snapshot in the background; switch after the cache becomes ready and quiet. */
void native_prepare_pending_switch(App *app, int target) {
    NativeSessionSlot *slot = &app->sessions[target];
    if (slot->snapshot_pending) {
        return; /* a hidden reconnect is already filling the cache */
    }
    bool reconnect = (slot->refresh_ineffective || app->snapshot_force) && !NATIVE_SWITCH_TEST_NO_RECONNECT;
    bool armed;
    if (reconnect) {
        clog(cLogLevelNotice,
             "reconnecting the %s session in the background for the deferred switch",
             native_session_slot_name(target));
        /* One reconnect per deferred switch: a stream that produces no AU even on a
         * fresh connection (bitmap path) must not loop reconnects forever. */
        slot->snapshot_retry_used = true;
        /* Arming happens inside the connect (old worker stopped, new not yet started),
         * so the connect IDR cannot race an unarmed cache. */
        if (!native_slot_connect(app, target, true)) {
            /* Same failure routing as every connect path; the tick cancels the switch. */
            native_slot_report_terminal(slot, RDP_STATE_NETWORK_ERROR,
                                        rdp_state_exit_code(RDP_STATE_NETWORK_ERROR));
            return;
        }
        pthread_mutex_lock(&app->video_lock);
        armed = slot->snapshot.armed;
        pthread_mutex_unlock(&app->video_lock);
    } else {
        /* No reconnect: the suppressed connection emits nothing until the resume below,
         * which is sent only after the cache is armed. */
        atomic_store(&slot->snapshot_idr_ready, false);
        pthread_mutex_lock(&app->video_lock);
        armed = native_au_snapshot_arm(&slot->snapshot);
        pthread_mutex_unlock(&app->video_lock);
    }
    if (!armed) {
        /* No memory for the cache: fall back to the immediate old-style switch (its
         * splash covers the reload) rather than leaving the press unanswered. */
        clog(cLogLevelWarning, "%s AU snapshot allocation failed; switching immediately",
             native_session_slot_name(target));
        app->pending_switch_target = -1;
        native_complete_session_switch(app, target);
        return;
    }
    slot->snapshot_pending = true; /* the tick suppresses once the IDR lands */
    slot->snapshot_deadline_ticks = 0;
    slot->suppressed = false;
    if (!reconnect) {
        rdp_set_suppress_output(slot->rdp, true);
        rdp_request_refresh(slot->rdp);
    }
}

#endif
