/* Atomic ownership handoff between live RDP slots. The ordering here is the
 * privacy/input/video boundary of the switch state machine; SDL thread only. */
#ifdef HELLOLG_TARGET_WEBOS

#include "native_switch_internal.h"

#include <stdatomic.h>

#include "native_app.h"
#include "native_capture_glue.h"
#include "native_input_route.h"
#include "native_mixer_overlay.h"
#include "native_pointer.h"
#include "native_presentation.h"
#include "native_rdp_state.h"
#include "native_rgba_owner.h"
#include "native_session.h"

#include "clog.h"

clog_define(g_native_log_switch_handoff, cLogLevelInfo, cLogFlags_Default, "native", NULL);

/* Finalizes a switch to `target`, which must already be connected. Retargets input,
 * cursor, and the shared video path while the mixed audio track stays attached, then
 * asks the servers to pause/resume their graphics. */
void native_complete_session_switch(App *app, int target) {
    int old_index = atomic_load(&app->active_index);
    NativeSessionSlot *target_slot = &app->sessions[target];
    /* A cache-allocation fallback can intentionally reach here while a fresh worker is
     * still CONNECTING and before snapshot_pending is set, so this boundary is looser
     * than native_slot_is_live_stream_target; a terminal failure is never switchable. */
    if (!target_slot->rdp || atomic_load(&target_slot->session_failed)) {
        return;
    }
    if (old_index == target) {
        native_preconnect_ui_select_slot(app->preconnect_ui, target);
        native_show_session_indicator(app, target);
        return;
    }
    clog(cLogLevelNotice, "switching video %s -> %s", native_session_slot_name(old_index),
         native_session_slot_name(target));
    /* Any switch that actually happens supersedes a deferred one still in flight. */
    app->pending_switch_target = -1;
    /* Reachable with the overlay up via the auto-switch on a died session. */
    native_mixer_overlay_hide(app);

    /* Releases still owed to the OLD server must go out before input is retargeted,
     * or its desktop keeps a stuck drag / auto-repeating key. */
    native_flush_held_inputs(app);
    native_input_set_active(&app->input, false);

    /* Route worker callbacks to the new slot; the old session's in-flight AUs stop
     * feeding the shared decoder immediately (active check in on_video_au). The video
     * track itself is NOT closed here: the old picture stays up and on_video_au swaps
     * the decoder in one pipeline reload once the new stream's keyframe is in hand —
     * no black gap, one audio interruption instead of two. */
    /* Snapshot the watchdog baseline BEFORE the target becomes active: its worker can
     * decode the resume keyframe at any point after the switch below, and on a static
     * desktop that keyframe is the only frame coming — baselining after it would make
     * the watchdog reconnect a successful switch and mislearn refresh_ineffective. */
    unsigned switch_baseline = atomic_load(&target_slot->video_ok_frames);

    /* The privacy boundary precedes the ownership publication: close and drain the
     * old slot's Rust payload gate before active_index can describe it as background. */
    native_capture_pause_for_active_slot_change(app);
    pthread_mutex_lock(&app->video_lock);
    /* The active flip and the stale-recovery clear live INSIDE video_lock: on_video_au
     * publishes video_refresh_needed under the same lock after re-checking the active
     * slot, so an outgoing worker mid-feed cannot interleave with the switch and leave
     * a stale request behind. A pending request belongs to the OUTGOING owner and goes
     * stale with the ownership change: draining it after the switch would send a
     * spurious refresh to the fresh target and arm its watchdog into a needless
     * reconnect on a static desktop. The old slot needs no recovery either way —
     * backgrounding reconnects it (snapshot) or suppresses it (resume asks for its own
     * keyframe on return). */
    unsigned target_epoch = atomic_load(&target_slot->connect_epoch);
    if (app->hub_return_rgba && app->hub_return_rgba_owner_slot == target) {
        if (!native_promote_hub_return_rgba_locked(app, target, target_epoch)) {
            /* The return slot reconnected while hidden. Its frozen pixels remain a
             * read-only fallback; the new generation gets a fresh mutable canvas. */
            native_close_rgba_locked(app, false);
            native_arm_hub_return_replacement_locked(
                app, target, target_epoch, atomic_load(&target_slot->video_ok_frames));
        }
    } else {
        native_close_rgba_locked(app, false);
        if (app->hub_return_rgba) {
            /* Choosing any other live stream commits the HUB exit; its original BACK
             * destination is no longer part of this handoff. */
            native_close_hub_return_rgba_locked(app);
        }
    }
    atomic_store(&app->active_index, target);
    atomic_store(&app->video_refresh_needed, false);
    native_present_invalidate(app);
    pthread_mutex_unlock(&app->video_lock);
    native_preconnect_ui_select_slot(app->preconnect_ui, target);
    native_duck_retarget(app);

    /* Input now drives the new session; the per-tick derive block re-arms the active
     * flag and re-syncs NumLock. */
    native_input_set_session(&app->input, target_slot->rdp);
    native_input_set_desktop_size(&app->input, (uint16_t)atomic_load(&target_slot->desktop_width),
                                  (uint16_t)atomic_load(&target_slot->desktop_height));
    app->input_locks_synced = false;
    native_request_pointer_window_size_update(app);
    native_cursor_reassert(&target_slot->cursor);

    /* Background the old server (graphics off, rdpsnd audio keeps feeding the mix; a
     * refresh-ineffective one reconnects hidden to cache its IDR) and wake the new one.
     * A previously suppressed session resumes with a delta frame the reloaded hardware
     * decoder cannot use, so bring the keyframe with us: a cached IDR snapshot replays
     * instantly, a refresh-capable server gets a keyframe request, and a refresh-
     * ineffective one without a snapshot reconnects for its connect IDR. */
    native_background_slot(app, old_index);
    if (target_slot->snapshot_pending) {
        /* The target's own hidden reconnect is still in flight: let the live connection
         * feed the decoder directly — its first frame IS the connect IDR (make-before-
         * break). In the ms-wide window where that IDR already landed in the cache
         * instead, the keyframe watchdog below reconnects — no worse than before. */
        target_slot->snapshot_pending = false;
        pthread_mutex_lock(&app->video_lock);
        native_au_snapshot_reset(&target_slot->snapshot);
        pthread_mutex_unlock(&app->video_lock);
        atomic_store(&target_slot->snapshot_idr_ready, false);
    } else if (target_slot->suppressed) {
        target_slot->suppressed = false;
        if (native_snapshot_replay(app, target)) {
            /* Decoder state rebuilt from the cached connect IDR: resume plainly — the
             * server's next delta continues that exact chain. No refresh, no reconnect. */
            rdp_set_suppress_output(target_slot->rdp, true);
        } else if (atomic_load(&target_slot->session_failed)) {
            /* The replay attempt flagged the slot (terminal feed error): the failure
             * drain routes it; nothing to resume here. */
        } else if (target_slot->refresh_ineffective && !NATIVE_SWITCH_TEST_NO_RECONNECT) {
            /* This server never delivers a keyframe on request (no Display Control
             * channel, Refresh Rect ignored — learned from an earlier watchdog timeout).
             * Skip the doomed wait and reconnect right away: a fresh connection always
             * starts with an IDR. */
            clog(cLogLevelWarning,
                 "%s server yields no keyframe on request; reconnecting immediately for a fresh IDR",
                 native_session_slot_name(target));
            if (!native_slot_connect(app, target, false)) {
                /* Startup failure (e.g. worker spawn): route it through the standard
                 * failure drain — configurator with a status / auto-switch to a survivor
                 * — instead of leaving a dead slot behind a frozen stream. */
                native_slot_report_terminal(target_slot, RDP_STATE_NETWORK_ERROR,
                                            rdp_state_exit_code(RDP_STATE_NETWORK_ERROR));
            }
        } else {
            rdp_set_suppress_output(target_slot->rdp, true);
            rdp_request_refresh(target_slot->rdp);
        }
    }

    /* Watchdog: if no decodable frame arrives, reconnect the slot (a fresh connection
     * always starts with an IDR — covers servers where neither suppress-resume nor the
     * display-control refresh produces a keyframe, e.g. grd mirror mode). */
    app->switch_deadline_ticks = SDL_GetTicks() + NATIVE_SWITCH_KEYFRAME_TIMEOUT_MS;
    app->switch_baseline_frames = switch_baseline;
    app->switch_reconnect_used = false;

    native_show_session_indicator(app, target);
}

#endif
