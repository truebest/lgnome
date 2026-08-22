#ifndef GNOMECAST_NATIVE_RDP_STATE_H
#define GNOMECAST_NATIVE_RDP_STATE_H

#include <stdbool.h>

#include "rdp_ffi.h"

/* RdpState naming, terminal-session policy, process exit codes, and the
 * cross-slot password redaction used before any worker detail is logged. */

typedef struct App App;
typedef struct NativeSessionSlot NativeSessionSlot;

const char *rdp_state_name(RdpState state);
bool rdp_state_is_terminal_error(RdpState state);
/* NoAvc420/DecoderError: native pipeline failures that must exit even in
 * interactive builds (reconnecting cannot fix them). */
bool rdp_state_is_native_runtime_failure(RdpState state);
int rdp_state_exit_code(RdpState state);

/* Marks a slot's session as terminated (worker or SDL thread); active and
 * background slots follow different failure policies. */
void native_slot_report_terminal(NativeSessionSlot *slot, RdpState state, int exit_code);

/* Thread-safe against config rewrites (takes App.redaction_lock); returns
 * either `line` itself or a static replacement string. */
const char *redact_if_sensitive(App *app, const char *line);

#endif
