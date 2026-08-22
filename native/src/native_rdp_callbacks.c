/* Worker-side callback table plus state/logging, pointer, and capture adapters.
 * Video, audio, and shared media ownership live in their dedicated modules. */

#include "native_rdp_callbacks.h"
#include "native_rdp_callback_parts.h"

#include <stdatomic.h>

#include "native_app.h"
#include "native_rdp_state.h"

#include "clog.h"

clog_define(g_native_log_rdp_callbacks, cLogLevelInfo, cLogFlags_Default, "native", NULL);

static void on_state(void *ctx, RdpState state, const char *detail) {
    NativeSessionSlot *slot = (NativeSessionSlot *)ctx;
    App *app = slot ? slot->app : NULL;
    if (slot) {
        atomic_store(&slot->current_state, (int)state);
        /* Input arming/NumLock sync happen on the SDL thread, derived from this state
         * (see the per-tick block in the app loop): writing shared input state from a
         * worker here would race the SDL thread's switch sequence — a demoted slot's
         * stale set_active(false) landing after the switch would strand input off. */
    }

    const char *safe_detail = redact_if_sensitive(app, detail);
    clog(cLogLevelInfo, "%s session state=%s(%d)%s%s",
         slot ? native_session_slot_name(slot->index) : "?", rdp_state_name(state), (int)state,
         safe_detail[0] ? " " : "", safe_detail);

    if (slot && rdp_state_is_terminal_error(state)) {
        clog(cLogLevelError, "terminal native error on the %s session: %s",
             native_session_slot_name(slot->index), rdp_state_name(state));
        native_slot_report_terminal(slot, state, rdp_state_exit_code(state));
    } else if (slot && state == RDP_STATE_STOPPED && atomic_load(&app->exit_code) == 0 &&
               !atomic_load(&slot->session_failed)) {
        /* Graceful server-side stop: flag it for the SDL thread (interactive builds go
         * back to the pre-connect UI or auto-switch; non-interactive builds exit). The
         * session_failed guard keeps the worker's final Stopped emission from
         * overwriting a terminal error already recorded for this slot — the UI must
         * report "failed: NetworkError", not "session stopped". */
        native_slot_report_terminal(slot, state, 0);
    }
}

static bool on_log_enabled(void *ctx, RdpLogLevel level, const char *target) {
    (void)ctx;
    (void)target;
    return native_rdp_log_is_enabled(level);
}

static void on_log(void *ctx, RdpLogLevel level, const char *target, const char *message) {
    NativeSessionSlot *slot = (NativeSessionSlot *)ctx;
    App *app = slot ? slot->app : NULL;
    native_rdp_log_emit(level, slot ? native_session_slot_name(slot->index) : "?", target,
                        redact_if_sensitive(app, message));
}

/* Fires on the initial MCS/GCC handshake and again on every RDPGFX_RESET_GRAPHICS_PDU, so
 * the real EGFX graphics output size (which can differ from the negotiated session size)
 * stays current for both the NDL/H.264 and RemoteFX RGBA paths and for pointer mapping. */
static void on_desktop_size(void *ctx, uint16_t width, uint16_t height) {
    NativeSessionSlot *slot = (NativeSessionSlot *)ctx;
    if (!slot) {
        return;
    }
    (void)slot->app;
    /* Only the slot's own atomics are written here. The input mapping derives from the
     * ACTIVE slot's atomics on the SDL thread once per tick: an is-active check followed
     * by a direct native_input_set_desktop_size would race a concurrent session switch —
     * this (then-active) worker could overwrite the size the switch installed for the
     * NEW slot, and that server may never send another size update. */
    atomic_store(&slot->desktop_width, width);
    atomic_store(&slot->desktop_height, height);
    clog(cLogLevelInfo, "%s desktop=%ux%u", native_session_slot_name(slot->index), (unsigned)width,
         (unsigned)height);
}

static void on_pointer_bitmap(void *ctx, uint16_t width, uint16_t height, uint16_t hotspot_x,
                              uint16_t hotspot_y, const uint8_t *rgba, size_t len) {
    NativeSessionSlot *slot = (NativeSessionSlot *)ctx;
    if (!slot) {
        return;
    }
    /* Always cache into the slot's own cursor: a background session keeps its latest
     * shape current so a switch can re-apply it immediately. */
    native_cursor_submit_bitmap(&slot->cursor, width, height, hotspot_x, hotspot_y, rgba, len);
}

static void on_pointer_state(void *ctx, uint32_t state) {
    NativeSessionSlot *slot = (NativeSessionSlot *)ctx;
    if (!slot) {
        return;
    }
    native_cursor_submit_state(&slot->cursor, state);
}

static void on_pointer_position(void *ctx, uint16_t x, uint16_t y) {
    NativeSessionSlot *slot = (NativeSessionSlot *)ctx;
    if (!slot || !native_slot_is_active(slot)) {
        return;
    }
#ifdef HELLOLG_TARGET_WEBOS
    App *app = slot->app;
    atomic_store(&app->pointer_warp_x, (unsigned)x);
    atomic_store(&app->pointer_warp_y, (unsigned)y);
    atomic_store(&app->pointer_warp_slot, slot->index);
    atomic_store(&app->pointer_warp_pending, true);
#else
    (void)x;
    (void)y;
#endif
}

static void on_camera_start(void *ctx, uint64_t generation, uint16_t width,
                            uint16_t height, uint16_t fps) {
    NativeSessionSlot *slot = (NativeSessionSlot *)ctx;
    if (!slot) {
        return;
    }
    native_capture_redirect_camera_start(&slot->app->capture_redirect, slot->index,
                                         generation, width, height, fps);
}

static void on_camera_stop(void *ctx, uint64_t generation) {
    NativeSessionSlot *slot = (NativeSessionSlot *)ctx;
    if (!slot) {
        return;
    }
    native_capture_redirect_camera_stop(&slot->app->capture_redirect, slot->index,
                                        generation);
}

static void on_camera_sample_request(void *ctx, uint64_t generation) {
    NativeSessionSlot *slot = (NativeSessionSlot *)ctx;
    if (!slot) {
        return;
    }
    native_capture_redirect_camera_sample_request(
        &slot->app->capture_redirect, slot->index, generation);
}

static void on_audio_input_start(void *ctx, uint64_t generation,
                                 uint32_t sample_rate, uint16_t channels,
                                 uint32_t frames_per_packet) {
    NativeSessionSlot *slot = (NativeSessionSlot *)ctx;
    if (!slot) {
        return;
    }
    native_capture_redirect_audio_start(
        &slot->app->capture_redirect, slot->index, generation, sample_rate,
        channels, frames_per_packet);
}

static void on_audio_input_stop(void *ctx, uint64_t generation) {
    NativeSessionSlot *slot = (NativeSessionSlot *)ctx;
    if (!slot) {
        return;
    }
    native_capture_redirect_audio_stop(&slot->app->capture_redirect, slot->index,
                                       generation);
}

/* The full callback table for one slot's RDP session; ctx is the slot itself. */
RdpCallbacks native_rdp_callbacks(NativeSessionSlot *slot) {
    RdpCallbacks callbacks = {
        .ctx = slot,
        .on_state = on_state,
        .on_log_enabled = on_log_enabled,
        .on_log = on_log,
        .on_desktop_size = on_desktop_size,
        .on_pointer_bitmap = on_pointer_bitmap,
        .on_pointer_position = on_pointer_position,
        .on_pointer_state = on_pointer_state,
        .on_camera_start = on_camera_start,
        .on_camera_stop = on_camera_stop,
        .on_camera_sample_request = on_camera_sample_request,
        .on_audio_input_start = on_audio_input_start,
        .on_audio_input_stop = on_audio_input_stop,
    };
    native_rdp_install_audio_callbacks(&callbacks);
    native_rdp_install_video_callbacks(&callbacks);
    return callbacks;
}
