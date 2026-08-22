//! Fast-path input and session-control command model.
//!
//! Pure data: the C entry points enqueue these on the worker channel and the
//! worker turns them into IronRDP fast-path events. No I/O, no shared state.

#![forbid(unsafe_code)]

use ironrdp_pdu::input::fast_path::{FastPathInputEvent, KeyboardFlags, SynchronizeFlags};
use ironrdp_pdu::input::mouse::{MousePdu, PointerFlags};

#[derive(Debug)]
pub(super) enum WorkerCommand {
    Stop,
    Input(InputCommand),
    Control(ControlCommand),
    /// A non-payload wake hint. Actual media bytes live in `MediaMailbox`.
    MediaReady,
}

/// Session-level (non-input) client requests, dispatched on the x224/DVC channels rather
/// than the fast-path input channel. Added for multi-session video switching.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(super) enum ControlCommand {
    /// TS_SUPPRESS_OUTPUT_PDU: `allow_display=false` asks the server to stop sending
    /// graphics (audio DVCs keep flowing), `true` resumes them.
    SuppressOutput { allow_display: bool },
    /// Ask the server for a fresh full frame / keyframe so a hardware decoder that lost
    /// its state (the shared webOS pipeline reloads on every track close) can resync.
    RequestRefresh,
}

#[derive(Debug)]
pub(super) enum InputCommand {
    PointerMove {
        x: u16,
        y: u16,
    },
    PointerButton {
        x: u16,
        y: u16,
        button: u8,
        down: bool,
    },
    PointerWheel {
        x: u16,
        y: u16,
        delta: i16,
    },
    Key {
        scancode: u8,
        down: bool,
        extended: bool,
    },
    Unicode {
        codepoint: u16,
        down: bool,
    },
    /// Absolute toggle-key state (TS_FP_SYNC_EVENT); bits follow `SynchronizeFlags`.
    SyncLocks {
        flags: u8,
    },
}

impl InputCommand {
    pub(super) fn into_events(self) -> Vec<FastPathInputEvent> {
        match self {
            InputCommand::PointerMove { x, y } => vec![FastPathInputEvent::MouseEvent(MousePdu {
                flags: PointerFlags::MOVE,
                number_of_wheel_rotation_units: 0,
                x_position: x,
                y_position: y,
            })],
            InputCommand::PointerButton { x, y, button, down } => {
                let mut flags = match button {
                    1 => PointerFlags::LEFT_BUTTON,
                    2 => PointerFlags::RIGHT_BUTTON,
                    3 => PointerFlags::MIDDLE_BUTTON_OR_WHEEL,
                    _ => PointerFlags::LEFT_BUTTON,
                };
                if down {
                    flags |= PointerFlags::DOWN;
                }
                vec![FastPathInputEvent::MouseEvent(MousePdu {
                    flags,
                    number_of_wheel_rotation_units: 0,
                    x_position: x,
                    y_position: y,
                })]
            }
            InputCommand::PointerWheel { x, y, delta } => {
                vec![FastPathInputEvent::MouseEvent(MousePdu {
                    flags: PointerFlags::VERTICAL_WHEEL,
                    number_of_wheel_rotation_units: delta,
                    x_position: x,
                    y_position: y,
                })]
            }
            InputCommand::Key {
                scancode,
                down,
                extended,
            } => {
                let mut flags = KeyboardFlags::empty();
                if !down {
                    flags |= KeyboardFlags::RELEASE;
                }
                if extended {
                    flags |= KeyboardFlags::EXTENDED;
                }
                vec![FastPathInputEvent::KeyboardEvent(flags, scancode)]
            }
            InputCommand::Unicode { codepoint, down } => {
                let mut flags = KeyboardFlags::empty();
                if !down {
                    flags |= KeyboardFlags::RELEASE;
                }
                vec![FastPathInputEvent::UnicodeKeyboardEvent(flags, codepoint)]
            }
            InputCommand::SyncLocks { flags } => vec![FastPathInputEvent::SyncEvent(
                SynchronizeFlags::from_bits_truncate(flags),
            )],
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn pointer_button_values_match_c_abi() {
        let cases = [
            (1, PointerFlags::LEFT_BUTTON),
            (2, PointerFlags::RIGHT_BUTTON),
            (3, PointerFlags::MIDDLE_BUTTON_OR_WHEEL),
        ];

        for (button, expected_button_flag) in cases {
            let events = InputCommand::PointerButton {
                x: 10,
                y: 20,
                button,
                down: true,
            }
            .into_events();
            assert_eq!(events.len(), 1);
            match &events[0] {
                FastPathInputEvent::MouseEvent(pdu) => {
                    assert_eq!(pdu.flags, expected_button_flag | PointerFlags::DOWN);
                    assert_eq!(pdu.number_of_wheel_rotation_units, 0);
                    assert_eq!(pdu.x_position, 10);
                    assert_eq!(pdu.y_position, 20);
                }
                other => panic!("expected mouse event for button {button}, got {other:?}"),
            }
        }
    }

    #[test]
    fn sync_locks_command_encodes_toggle_flags() {
        let events = InputCommand::SyncLocks {
            flags: (SynchronizeFlags::NUM_LOCK | SynchronizeFlags::CAPS_LOCK).bits(),
        }
        .into_events();
        assert_eq!(events.len(), 1);
        match &events[0] {
            FastPathInputEvent::SyncEvent(flags) => {
                assert_eq!(
                    *flags,
                    SynchronizeFlags::NUM_LOCK | SynchronizeFlags::CAPS_LOCK
                );
            }
            other => panic!("expected sync event, got {other:?}"),
        }
    }
}
