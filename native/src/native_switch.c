/* Per-tick coordinator for deferred session switches, snapshot backgrounding,
 * worker refresh requests, and the keyframe watchdog. SDL thread only. */
#ifdef HELLOLG_TARGET_WEBOS

#include "native_switch_internal.h"

#include <stdatomic.h>

#include "native_app.h"
#include "native_rdp_state.h"
#include "native_session.h"

#include "clog.h"

clog_define(g_native_log_switch, cLogLevelInfo, cLogFlags_Default, "native", NULL);

/* Snapshot the ACTIVE slot's decode counter BEFORE issuing any resume/refresh
 * request: the requested keyframe may decode immediately, and a later baseline
 * would satisfy the watchdog spuriously. */
void native_switch_watchdog_baseline(App *app, NativeSessionSlot *active) {
    app->switch_baseline_frames = atomic_load(&active->video_ok_frames);
}

/* Start the watchdog window AFTER the requests are submitted: a state-changing
 * command falls back to a blocking send on a full worker queue, and time spent
 * blocked there must not consume the keyframe window. */
void native_switch_watchdog_start(App *app, uint32_t timeout_ms) {
    app->switch_deadline_ticks = SDL_GetTicks() + timeout_ms;
    app->switch_reconnect_used = false;
}

/* Per-tick bookkeeping: worker-side refresh requests, snapshot backgrounding and the
 * keyframe watchdog. */
void native_switch_tick(App *app) {
    /* Snapshot backgrounding: once a hidden-reconnected slot has its connect IDR cached,
     * silence its server. Nothing further is transmitted after the cached AUs, so the
     * post-resume delta will reference exactly the state a replay rebuilds. A snapshot
     * voided before this tick could suppress (an oversize IDR overflowed the cache) is
     * suppressed too — otherwise the backgrounded session would stream into the drop
     * path indefinitely; with no cache the switch-back takes the reconnect fallback. */
    for (int i = 0; i < NATIVE_SETTINGS_MAX_SESSIONS; i++) {
        NativeSessionSlot *slot = &app->sessions[i];
        if (!slot->snapshot_pending || i == atomic_load(&app->active_index)) {
            continue;
        }
        if (!slot->rdp || atomic_load(&slot->current_state) != (int)RDP_STATE_ACTIVE) {
            continue;
        }
        bool idr_ready = atomic_load(&slot->snapshot_idr_ready);
        bool voided = false;
        if (!idr_ready) {
            /* pending implies the snapshot was armed; disarmed now = append voided it. */
            pthread_mutex_lock(&app->video_lock);
            voided = !slot->snapshot.armed;
            pthread_mutex_unlock(&app->video_lock);
        }
        if (idr_ready || voided) {
            rdp_set_suppress_output(slot->rdp, false);
            slot->suppressed = true;
            slot->snapshot_pending = false;
            if (idr_ready) {
                clog(cLogLevelDebug, "%s connect IDR cached; suppressing the backgrounded session",
                     native_session_slot_name(i));
            } else {
                clog(cLogLevelWarning,
                     "%s snapshot voided; suppressing the backgrounded session without a cache",
                     native_session_slot_name(i));
                if (i == app->pending_switch_target) {
                    /* The deferred switch lost its cache (oversize IDR): answer the
                     * press with the immediate old-style switch rather than never. */
                    app->pending_switch_target = -1;
                    native_complete_session_switch(app, i);
                    continue;
                }
            }
        } else if (slot->snapshot_deadline_ticks == 0) {
            /* Count from ACTIVE (the guard above), not from the reconnect: a slow
             * TLS/CredSSP handshake must not eat the whole window. */
            slot->snapshot_deadline_ticks = SDL_GetTicks() + NATIVE_SWITCH_KEYFRAME_TIMEOUT_MS;
        } else if (SDL_TICKS_PASSED(SDL_GetTicks(), slot->snapshot_deadline_ticks)) {
            if (i == app->pending_switch_target && !NATIVE_SWITCH_TEST_NO_RECONNECT &&
                !slot->snapshot_retry_used && !atomic_load(&slot->video_via_bitmap)) {
                /* A deferred switch asked this server for a keyframe and none came:
                 * learn it and fall to the hidden reconnect — the user is still watching
                 * the live current stream, so the lesson costs no black screen. */
                clog(cLogLevelWarning,
                     "%s yielded no keyframe within %ums; reconnecting in the background (deferred switch)",
                     native_session_slot_name(i), (unsigned)NATIVE_SWITCH_KEYFRAME_TIMEOUT_MS);
                slot->refresh_ineffective = true;
                slot->snapshot_pending = false;
                native_prepare_pending_switch(app, i);
                continue;
            }
            /* No AU ever seeded the snapshot — e.g. the stream negotiated the RemoteFX
             * bitmap path, which feeds on_bitmap_update instead of on_video_au. Silence
             * the server anyway; the cacheless switch-back takes the reconnect fallback. */
            pthread_mutex_lock(&app->video_lock);
            native_au_snapshot_reset(&slot->snapshot);
            pthread_mutex_unlock(&app->video_lock);
            rdp_set_suppress_output(slot->rdp, false);
            slot->suppressed = true;
            slot->snapshot_pending = false;
            clog(cLogLevelWarning,
                 "%s snapshot got no AUs within %ums; suppressing the backgrounded session without a cache",
                 native_session_slot_name(i), (unsigned)NATIVE_SWITCH_KEYFRAME_TIMEOUT_MS);
            if (i == app->pending_switch_target) {
                /* Reached with the one retry spent (a stream that never feeds
                 * on_video_au, e.g. the bitmap path) or in no-reconnect test builds:
                 * answer the press with the immediate old-style switch. */
                app->pending_switch_target = -1;
                native_complete_session_switch(app, i);
                continue;
            }
        }
    }

    /* Deferred switch: complete once the target's cache is ready and its stream has
     * been quiet past the suppress tail — the replay is then race-free and instant. */
    if (app->pending_switch_target >= 0) {
        int target = app->pending_switch_target;
        NativeSessionSlot *slot = &app->sessions[target];
        if (!slot->rdp || atomic_load(&slot->session_failed)) {
            /* The prepared session died; the failure drain reports it. Stay put. */
            clog(cLogLevelWarning, "deferred switch to %s abandoned (session gone)",
                 native_session_slot_name(target));
            app->pending_switch_target = -1;
        } else if (!slot->snapshot_pending && slot->suppressed && atomic_load(&slot->snapshot_idr_ready) &&
                   native_monotonic_ms() - atomic_load(&slot->snapshot_last_au_ms) >= NATIVE_SNAPSHOT_QUIET_MS) {
            clog(cLogLevelDebug, "%s stream is ready; completing the deferred switch",
                 native_session_slot_name(target));
            app->pending_switch_target = -1;
            native_complete_session_switch(app, target);
        } else if (SDL_TICKS_PASSED(SDL_GetTicks(), app->pending_switch_deadline_ticks)) {
            /* The target never became cleanly ready — it keeps streaming through the
             * suppress (quiet window never opens) or its cache keeps dying. Answer the
             * press with the immediate old-style switch instead of staying pending
             * forever with further presses ignored. */
            clog(cLogLevelWarning, "deferred switch to %s timed out after %ums; switching the old way",
                 native_session_slot_name(target), (unsigned)NATIVE_PENDING_SWITCH_TIMEOUT_MS);
            app->pending_switch_target = -1;
            native_complete_session_switch(app, target);
        }
    }

    if (atomic_exchange(&app->video_refresh_needed, false)) {
        NativeSessionSlot *active = native_active_slot(app);
        if (active->rdp && atomic_load(&active->current_state) == (int)RDP_STATE_ACTIVE) {
            native_switch_watchdog_baseline(app, active);
            rdp_request_refresh(active->rdp);
            native_switch_watchdog_start(app, NATIVE_SWITCH_KEYFRAME_TIMEOUT_MS);
        }
    }

    if (app->switch_deadline_ticks != 0) {
        NativeSessionSlot *watched = native_active_slot(app);
        if (atomic_load(&watched->video_ok_frames) != app->switch_baseline_frames) {
            app->switch_deadline_ticks = 0;
        } else if (watched->rdp && atomic_load(&watched->current_state) != (int)RDP_STATE_ACTIVE) {
            /* Still (re)connecting: the keyframe timeout must count from ACTIVE, not from
             * the button press — otherwise a slow TLS/CredSSP handshake makes the watchdog
             * fire mid-reconnect and thrash the server with a second reconnect. */
            app->switch_deadline_ticks = SDL_GetTicks() + NATIVE_SWITCH_KEYFRAME_TIMEOUT_MS;
        } else if (SDL_TICKS_PASSED(SDL_GetTicks(), app->switch_deadline_ticks)) {
            NativeSessionSlot *active = native_active_slot(app);
            if (NATIVE_SWITCH_TEST_NO_RECONNECT) {
                clog(cLogLevelDebug,
                     "TEST: watchdog expired; would reconnect the %s session, keeping the connection",
                     native_session_slot_name(active->index));
                app->switch_deadline_ticks = SDL_GetTicks() + 10000u; /* re-check, keep logging */
            } else if (!app->switch_reconnect_used && active->rdp &&
                atomic_load(&active->current_state) == (int)RDP_STATE_ACTIVE) {
                clog(cLogLevelWarning,
                     "no keyframe %ums after the switch; reconnecting the %s session for a fresh IDR",
                     (unsigned)NATIVE_SWITCH_KEYFRAME_TIMEOUT_MS,
                     native_session_slot_name(active->index));
                /* Remember: switches to this slot should reconnect immediately from now on. */
                active->refresh_ineffective = true;
                native_switch_watchdog_baseline(app, active);
                native_switch_watchdog_start(app, 2u * NATIVE_SWITCH_KEYFRAME_TIMEOUT_MS);
                app->switch_reconnect_used = true;
                if (!native_slot_connect(app, active->index, false)) {
                    /* Same failure routing as the fast-reconnect path above. */
                    native_slot_report_terminal(active, RDP_STATE_NETWORK_ERROR,
                                                rdp_state_exit_code(RDP_STATE_NETWORK_ERROR));
                }
            } else {
                clog(cLogLevelError, "switch watchdog gave up waiting for video frames");
                app->switch_deadline_ticks = 0;
            }
        }
    }
}

#endif
