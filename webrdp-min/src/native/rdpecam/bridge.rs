//! Shared camera lifecycle, device identity, and capture callback bridge.

use std::sync::{Arc, Mutex};

use ironrdp_dvc::DvcMessage;

use super::super::{CallbackSink, RdpLogLevel, LOG_TARGET_CAMERA};
use super::wire::{
    device_added, device_removed, raw, sample_error, ERROR_UNEXPECTED, MSG_SAMPLE_RESPONSE,
    PROTOCOL_VERSION,
};

#[derive(Clone)]
pub(in super::super) struct CameraBridge {
    pub(super) inner: Arc<Mutex<CameraState>>,
}

pub(super) struct CameraState {
    pub(super) callbacks: CallbackSink,
    pub(super) width: u16,
    pub(super) height: u16,
    pub(super) fps: u16,
    pub(super) available: bool,
    pub(super) advertised: bool,
    pub(super) enum_channel_id: Option<u32>,
    pub(super) enum_negotiated: bool,
    pub(super) version: u8,
    pub(super) next_device_token: u64,
    pub(super) retired_through_token: u64,
    pub(super) device_instance: Option<DeviceInstance>,
    pub(super) activation_refs: u32,
    pub(super) lifecycle: CameraLifecycle,
    pub(super) generation: u64,
    pub(super) pending_requests: u8,
    pub(super) credit_notified: bool,
    pub(super) pending_credit_notification: Option<(CallbackSink, u64)>,
    pub(super) sample_requests_received: u64,
    pub(super) sample_responses_sent: u64,
    pub(super) sample_errors_sent: u64,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(super) struct DeviceInstance {
    pub(super) token: u64,
    pub(super) channel_id: u32,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(super) enum CameraLifecycle {
    Deactivated,
    Activated,
    Streaming,
}

impl CameraLifecycle {
    pub(super) fn name(self) -> &'static str {
        match self {
            Self::Deactivated => "deactivated",
            Self::Activated => "stopped",
            Self::Streaming => "running",
        }
    }

    pub(super) fn is_active(self) -> bool {
        self != Self::Deactivated
    }

    pub(super) fn is_streaming(self) -> bool {
        self == Self::Streaming
    }
}

impl CameraBridge {
    pub(in super::super) fn new(
        callbacks: CallbackSink,
        width: u16,
        height: u16,
        fps: u16,
    ) -> Self {
        Self {
            inner: Arc::new(Mutex::new(CameraState {
                callbacks,
                width,
                height,
                fps,
                available: false,
                advertised: false,
                enum_channel_id: None,
                enum_negotiated: false,
                version: PROTOCOL_VERSION,
                next_device_token: 0,
                retired_through_token: 0,
                device_instance: None,
                activation_refs: 0,
                lifecycle: CameraLifecycle::Deactivated,
                generation: 0,
                pending_requests: 0,
                credit_notified: false,
                pending_credit_notification: None,
                sample_requests_received: 0,
                sample_responses_sent: 0,
                sample_errors_sent: 0,
            })),
        }
    }

    /// Reconciles desired device availability. If version negotiation already
    /// completed, returns the enumerator notification that must be sent now.
    pub(in super::super) fn set_available(
        &self,
        available: bool,
    ) -> Option<(u32, Vec<DvcMessage>)> {
        let (notification, stop, log) = {
            let mut state = self.inner.lock().ok()?;
            if state.available == available {
                return None;
            }
            state.available = available;

            if available {
                let notification = advertise_device_locked(&mut state);
                let log = notification.as_ref().map(|_| {
                    DeferredLog::new(
                        state.callbacks,
                        RdpLogLevel::Info,
                        format!(
                            "camera: TX DeviceAdded state={} activation_refs={} pending_samples={}",
                            state.lifecycle.name(),
                            state.activation_refs,
                            state.pending_requests,
                        ),
                    )
                });
                (notification, None, log)
            } else {
                let was_advertised = state.advertised;
                let channel_id = state.enum_channel_id;
                let version = state.version;
                let pending = state.pending_requests;
                state.advertised = false;
                let stop = retire_device_locked(&mut state);
                let notification = if was_advertised {
                    channel_id.map(|channel_id| (channel_id, vec![raw(device_removed(version))]))
                } else {
                    None
                };
                let log = notification.as_ref().map(|_| {
                    DeferredLog::new(
                        state.callbacks,
                        RdpLogLevel::Info,
                        format!(
                            "camera: TX DeviceRemoved state={} activation_refs={} cancelled_samples={pending}",
                            state.lifecycle.name(),
                            state.activation_refs,
                        ),
                    )
                });
                (notification, stop, log)
            }
        };
        if let Some(log) = log {
            log.emit();
        }
        if let Some(stop) = stop {
            stop.invoke();
        }
        notification
    }

    pub(super) fn allocate_device_token(&self) -> Option<u64> {
        let mut state = self.inner.lock().ok()?;
        state.next_device_token = state.next_device_token.checked_add(1)?;
        Some(state.next_device_token)
    }

    pub(in super::super) fn submit_h264(
        &self,
        generation: u64,
        data: Vec<u8>,
    ) -> Option<(u32, Vec<DvcMessage>)> {
        if data.is_empty() {
            return self.submit_error(generation);
        }
        let (channel_id, version) = {
            let mut state = self.inner.lock().ok()?;
            if !state.lifecycle.is_streaming()
                || state.generation != generation
                || state.pending_requests == 0
                || !state.credit_notified
            {
                return None;
            }
            let channel_id = state.device_instance?.channel_id;
            state.pending_requests -= 1;
            state.credit_notified = false;
            state.sample_responses_sent = state.sample_responses_sent.saturating_add(1);
            let next_credit = arm_next_credit_locked(&mut state);
            debug_assert!(state.pending_credit_notification.is_none());
            state.pending_credit_notification = next_credit;
            (channel_id, state.version)
        };
        let mut response = Vec::with_capacity(3 + data.len());
        response.extend_from_slice(&[version, MSG_SAMPLE_RESPONSE, 0]);
        response.extend_from_slice(&data);
        Some((channel_id, vec![raw(response)]))
    }

    pub(in super::super) fn submit_error(&self, generation: u64) -> Option<(u32, Vec<DvcMessage>)> {
        let (channel_id, version) = {
            let mut state = self.inner.lock().ok()?;
            if !state.lifecycle.is_streaming()
                || state.generation != generation
                || state.pending_requests == 0
                || !state.credit_notified
            {
                return None;
            }
            let channel_id = state.device_instance?.channel_id;
            state.pending_requests -= 1;
            state.credit_notified = false;
            state.sample_errors_sent = state.sample_errors_sent.saturating_add(1);
            let next_credit = arm_next_credit_locked(&mut state);
            debug_assert!(state.pending_credit_notification.is_none());
            state.pending_credit_notification = next_credit;
            (channel_id, state.version)
        };
        Some((
            channel_id,
            vec![raw(sample_error(version, ERROR_UNEXPECTED))],
        ))
    }

    /// Deliver the next server credit only after the preceding response has
    /// left the capture send barrier. The C callback may take the capture
    /// manager lock, which is also held while capture is synchronously closed.
    pub(in super::super) fn notify_next_credit(&self) {
        let notification = self
            .inner
            .lock()
            .ok()
            .and_then(|mut state| state.pending_credit_notification.take());
        if let Some((callbacks, generation)) = notification {
            callbacks.camera_sample_request(generation);
        }
    }
}

#[derive(Clone, Copy)]
pub(super) enum DeferredCameraCallback {
    Start {
        callbacks: CallbackSink,
        generation: u64,
        width: u16,
        height: u16,
        fps: u16,
    },
    Stop {
        callbacks: CallbackSink,
        generation: u64,
    },
    SampleRequest {
        callbacks: CallbackSink,
        generation: u64,
    },
}

impl DeferredCameraCallback {
    pub(super) fn invoke(self) {
        match self {
            Self::Start {
                callbacks,
                generation,
                width,
                height,
                fps,
            } => callbacks.camera_start(generation, width, height, fps),
            Self::Stop {
                callbacks,
                generation,
            } => callbacks.camera_stop(generation),
            Self::SampleRequest {
                callbacks,
                generation,
            } => callbacks.camera_sample_request(generation),
        }
    }
}

pub(super) struct DeferredLog {
    callbacks: CallbackSink,
    level: RdpLogLevel,
    line: String,
}

impl DeferredLog {
    pub(super) fn new(callbacks: CallbackSink, level: RdpLogLevel, line: String) -> Self {
        Self {
            callbacks,
            level,
            line,
        }
    }

    pub(super) fn emit(self) {
        self.callbacks
            .log(self.level, LOG_TARGET_CAMERA, format_args!("{}", self.line));
    }
}

pub(super) fn arm_next_credit_locked(state: &mut CameraState) -> Option<(CallbackSink, u64)> {
    if state.lifecycle.is_streaming() && state.pending_requests > 0 && !state.credit_notified {
        state.credit_notified = true;
        Some((state.callbacks, state.generation))
    } else {
        None
    }
}

pub(super) struct StopTransition {
    pub(super) pending_requests: u8,
    pub(super) callback: Option<DeferredCameraCallback>,
}

/// Stops capture atomically with respect to device requests and submissions. The
/// caller decides whether accepted sample credits become errors or are cancelled
/// by a DeviceRemoved/channel-retirement transition.
pub(super) fn stop_capture_locked(state: &mut CameraState) -> StopTransition {
    let pending_requests = state.pending_requests;
    state.pending_credit_notification = None;
    let callback = if state.lifecycle.is_streaming() {
        let generation = state.generation;
        state.lifecycle = CameraLifecycle::Activated;
        state.generation = state.generation.wrapping_add(1).max(1);
        Some(DeferredCameraCallback::Stop {
            callbacks: state.callbacks,
            generation,
        })
    } else {
        None
    };
    state.pending_requests = 0;
    state.credit_notified = false;
    StopTransition {
        pending_requests,
        callback,
    }
}

pub(super) fn retire_device_locked(state: &mut CameraState) -> Option<DeferredCameraCallback> {
    let transition = stop_capture_locked(state);
    state.lifecycle = CameraLifecycle::Deactivated;
    state.activation_refs = 0;
    state.device_instance = None;
    state.retired_through_token = state.next_device_token;
    transition.callback
}

pub(super) fn replace_device_locked(
    state: &mut CameraState,
    instance: DeviceInstance,
) -> Option<DeferredCameraCallback> {
    let previous_token = state.device_instance.map(|current| current.token);
    let transition = stop_capture_locked(state);
    state.lifecycle = CameraLifecycle::Deactivated;
    state.activation_refs = 0;
    state.device_instance = Some(instance);
    if let Some(previous_token) = previous_token {
        state.retired_through_token = state.retired_through_token.max(previous_token);
    }
    transition.callback
}

pub(super) fn advertise_device_locked(state: &mut CameraState) -> Option<(u32, Vec<DvcMessage>)> {
    if !state.available || state.advertised || !state.enum_negotiated {
        return None;
    }
    let channel_id = state.enum_channel_id?;
    // A processor created before this DeviceAdded belongs to an older
    // advertisement, even if the server later reuses its numeric channel ID.
    state.retired_through_token = state.next_device_token;
    state.advertised = true;
    Some((channel_id, vec![raw(device_added(state.version))]))
}
