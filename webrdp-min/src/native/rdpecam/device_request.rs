//! RDPECAM device request state machine.

use ironrdp_dvc::DvcMessage;

use super::super::RdpLogLevel;
use super::bridge::{
    arm_next_credit_locked, stop_capture_locked, CameraLifecycle, CameraState,
    DeferredCameraCallback, DeferredLog, DeviceInstance,
};
use super::device::CameraDevice;
use super::wire::*;
use super::MAX_PENDING_SAMPLE_REQUESTS;

impl CameraDevice {
    fn error(version: u8, code: u32) -> Vec<DvcMessage> {
        vec![raw(error_response(version, code))]
    }

    fn lifecycle_log(
        state: &CameraState,
        direction: &str,
        message: &str,
        request: Option<&str>,
        error_code: Option<u32>,
        pending_lifecycle: u8,
    ) -> DeferredLog {
        let request = request.unwrap_or("-");
        let error_code = error_code
            .map(|code| code.to_string())
            .unwrap_or_else(|| "-".to_owned());
        let (token, channel_id) = state
            .device_instance
            .map(|instance| (instance.token.to_string(), instance.channel_id.to_string()))
            .unwrap_or_else(|| ("-".to_owned(), "-".to_owned()));
        DeferredLog::new(
            state.callbacks,
            RdpLogLevel::Debug,
            format!(
                "camera: {direction} {message} request={request} stream=all state={} active_streams={} activation_refs={} pending_samples={} pending_lifecycle={} samples_rx={} samples_tx={} sample_errors={} device_token={token} channel_id={channel_id} error_code={error_code}",
                state.lifecycle.name(),
                u8::from(state.lifecycle.is_streaming()),
                state.activation_refs,
                state.pending_requests,
                pending_lifecycle,
                state.sample_requests_received,
                state.sample_responses_sent,
                state.sample_errors_sent,
            ),
        )
    }

    fn lifecycle_error(
        state: &CameraState,
        request: &str,
        version: u8,
        code: u32,
    ) -> RequestOutcome {
        RequestOutcome {
            messages: Self::error(version, code),
            callbacks: Vec::new(),
            logs: vec![Self::lifecycle_log(
                state,
                "TX",
                "ErrorResponse",
                Some(request),
                Some(code),
                0,
            )],
        }
    }

    pub(super) fn process_request(&self, channel_id: u32, payload: &[u8]) -> Vec<DvcMessage> {
        let instance = DeviceInstance {
            token: self.token,
            channel_id,
        };
        let outcome = {
            let mut state = match self.bridge.inner.lock() {
                Ok(state) => state,
                Err(_) => return Self::error(PROTOCOL_VERSION, ERROR_UNEXPECTED),
            };
            if self.channel_id != Some(channel_id) || state.device_instance != Some(instance) {
                return Vec::new();
            }
            let (version, message_id) = match payload {
                [version, message_id, ..] => (*version, *message_id),
                _ => return Self::error(state.version, ERROR_INVALID_MESSAGE),
            };
            if version != state.version {
                return Self::error(state.version, ERROR_INVALID_MESSAGE);
            }

            match message_id {
                MSG_ACTIVATE_DEVICE if payload.len() == 2 => {
                    if !state.available || !state.advertised {
                        RequestOutcome::messages(Self::error(version, ERROR_NOT_INITIALIZED))
                    } else if let Some(next_refs) = state.activation_refs.checked_add(1) {
                        state.activation_refs = next_refs;
                        if state.lifecycle == CameraLifecycle::Deactivated {
                            state.lifecycle = CameraLifecycle::Activated;
                        }
                        RequestOutcome {
                            messages: vec![raw(vec![version, MSG_SUCCESS])],
                            callbacks: Vec::new(),
                            logs: vec![Self::lifecycle_log(
                                &state,
                                "TX",
                                "SuccessResponse",
                                Some("ActivateDeviceRequest"),
                                None,
                                0,
                            )],
                        }
                    } else {
                        Self::lifecycle_error(
                            &state,
                            "ActivateDeviceRequest",
                            version,
                            ERROR_INVALID_REQUEST,
                        )
                    }
                }
                MSG_DEACTIVATE_DEVICE if payload.len() == 2 => {
                    let mut logs = vec![Self::lifecycle_log(
                        &state,
                        "RX",
                        "DeactivateDeviceRequest",
                        None,
                        None,
                        1,
                    )];
                    if state.activation_refs == 0 {
                        Self::lifecycle_error(
                            &state,
                            "DeactivateDeviceRequest",
                            version,
                            ERROR_NOT_INITIALIZED,
                        )
                    } else {
                        state.activation_refs -= 1;
                        let mut callbacks = Vec::new();
                        let mut messages = Vec::new();
                        if state.activation_refs == 0 {
                            let transition = stop_capture_locked(&mut state);
                            state.lifecycle = CameraLifecycle::Deactivated;
                            state.sample_errors_sent = state
                                .sample_errors_sent
                                .saturating_add(u64::from(transition.pending_requests));
                            messages.extend(
                                (0..transition.pending_requests)
                                    .map(|_| raw(sample_error(version, ERROR_UNEXPECTED))),
                            );
                            if let Some(callback) = transition.callback {
                                callbacks.push(callback);
                            }
                        }
                        messages.push(raw(vec![version, MSG_SUCCESS]));
                        logs.push(Self::lifecycle_log(
                            &state,
                            "TX",
                            "SuccessResponse",
                            Some("DeactivateDeviceRequest"),
                            None,
                            0,
                        ));
                        RequestOutcome {
                            messages,
                            callbacks,
                            logs,
                        }
                    }
                }
                MSG_STREAM_LIST_REQUEST if payload.len() == 2 => {
                    if !state.lifecycle.is_active() {
                        RequestOutcome::messages(Self::error(version, ERROR_NOT_INITIALIZED))
                    } else {
                        // One Color/Capture stream, selected and shareable.
                        RequestOutcome::messages(vec![raw(vec![
                            version,
                            MSG_STREAM_LIST_RESPONSE,
                            1,
                            0,
                            1,
                            1,
                            1,
                        ])])
                    }
                }
                MSG_MEDIA_TYPE_LIST_REQUEST if payload.len() == 3 => {
                    if !state.lifecycle.is_active() {
                        RequestOutcome::messages(Self::error(version, ERROR_NOT_INITIALIZED))
                    } else if payload[2] != 0 {
                        RequestOutcome::messages(Self::error(version, ERROR_INVALID_STREAM))
                    } else {
                        let mut response = vec![version, MSG_MEDIA_TYPE_LIST_RESPONSE];
                        append_media_type(&mut response, state.width, state.height, state.fps);
                        RequestOutcome::messages(vec![raw(response)])
                    }
                }
                MSG_CURRENT_MEDIA_TYPE_REQUEST if payload.len() == 3 => {
                    if !state.lifecycle.is_active() {
                        RequestOutcome::messages(Self::error(version, ERROR_NOT_INITIALIZED))
                    } else if payload[2] != 0 {
                        RequestOutcome::messages(Self::error(version, ERROR_INVALID_STREAM))
                    } else {
                        let mut response = vec![version, MSG_CURRENT_MEDIA_TYPE_RESPONSE];
                        append_media_type(&mut response, state.width, state.height, state.fps);
                        RequestOutcome::messages(vec![raw(response)])
                    }
                }
                MSG_START_STREAMS => {
                    let rx_log =
                        Self::lifecycle_log(&state, "RX", "StartStreamsRequest", None, None, 1);
                    if !state.lifecycle.is_active() {
                        let mut outcome = Self::lifecycle_error(
                            &state,
                            "StartStreamsRequest",
                            version,
                            ERROR_NOT_INITIALIZED,
                        );
                        outcome.logs.insert(0, rx_log);
                        outcome
                    } else if payload.len() != 29 || payload[2] != 0 {
                        let mut outcome = Self::lifecycle_error(
                            &state,
                            "StartStreamsRequest",
                            version,
                            ERROR_INVALID_MEDIA_TYPE,
                        );
                        outcome.logs.insert(0, rx_log);
                        outcome
                    } else if payload[3..] != media_type(state.width, state.height, state.fps) {
                        let mut outcome = Self::lifecycle_error(
                            &state,
                            "StartStreamsRequest",
                            version,
                            ERROR_INVALID_MEDIA_TYPE,
                        );
                        outcome.logs.insert(0, rx_log);
                        outcome
                    } else if state.lifecycle.is_streaming() {
                        let mut outcome = Self::lifecycle_error(
                            &state,
                            "StartStreamsRequest",
                            version,
                            ERROR_INVALID_REQUEST,
                        );
                        outcome.logs.insert(0, rx_log);
                        outcome
                    } else {
                        state.lifecycle = CameraLifecycle::Streaming;
                        state.generation = state.generation.wrapping_add(1).max(1);
                        state.pending_requests = 0;
                        state.credit_notified = false;
                        state.pending_credit_notification = None;
                        RequestOutcome {
                            messages: vec![raw(vec![version, MSG_SUCCESS])],
                            callbacks: vec![DeferredCameraCallback::Start {
                                callbacks: state.callbacks,
                                generation: state.generation,
                                width: state.width,
                                height: state.height,
                                fps: state.fps,
                            }],
                            logs: vec![
                                rx_log,
                                Self::lifecycle_log(
                                    &state,
                                    "TX",
                                    "SuccessResponse",
                                    Some("StartStreamsRequest"),
                                    None,
                                    0,
                                ),
                            ],
                        }
                    }
                }
                MSG_STOP_STREAMS if payload.len() == 2 => {
                    let rx_log =
                        Self::lifecycle_log(&state, "RX", "StopStreamsRequest", None, None, 1);
                    if !state.lifecycle.is_active() {
                        let mut outcome = Self::lifecycle_error(
                            &state,
                            "StopStreamsRequest",
                            version,
                            ERROR_NOT_INITIALIZED,
                        );
                        outcome.logs.insert(0, rx_log);
                        outcome
                    } else {
                        let transition = stop_capture_locked(&mut state);
                        state.sample_errors_sent = state
                            .sample_errors_sent
                            .saturating_add(u64::from(transition.pending_requests));
                        let mut responses =
                            Vec::with_capacity(usize::from(transition.pending_requests) + 1);
                        responses.extend(
                            (0..transition.pending_requests)
                                .map(|_| raw(sample_error(version, ERROR_UNEXPECTED))),
                        );
                        responses.push(raw(vec![version, MSG_SUCCESS]));
                        let mut logs = vec![rx_log];
                        if transition.pending_requests > 0 {
                            logs.push(DeferredLog::new(
                                state.callbacks,
                                RdpLogLevel::Debug,
                                format!(
                                    "camera: TX SampleErrorResponse request=SampleRequest stream=0 state={} count={} reason=StopStreamsRequest error_code={}",
                                    state.lifecycle.name(),
                                    transition.pending_requests,
                                    ERROR_UNEXPECTED,
                                ),
                            ));
                        }
                        logs.push(Self::lifecycle_log(
                            &state,
                            "TX",
                            "SuccessResponse",
                            Some("StopStreamsRequest"),
                            None,
                            0,
                        ));
                        RequestOutcome {
                            messages: responses,
                            callbacks: transition.callback.into_iter().collect(),
                            logs,
                        }
                    }
                }
                MSG_SAMPLE_REQUEST if payload.len() == 3 => {
                    state.sample_requests_received =
                        state.sample_requests_received.saturating_add(1);
                    if !state.lifecycle.is_streaming() {
                        state.sample_errors_sent = state.sample_errors_sent.saturating_add(1);
                        RequestOutcome::messages(vec![raw(sample_error(
                            version,
                            ERROR_NOT_INITIALIZED,
                        ))])
                    } else if payload[2] != 0 {
                        state.sample_errors_sent = state.sample_errors_sent.saturating_add(1);
                        RequestOutcome::messages(vec![raw(sample_error(
                            version,
                            ERROR_INVALID_STREAM,
                        ))])
                    } else if state.pending_requests >= MAX_PENDING_SAMPLE_REQUESTS {
                        state.sample_errors_sent = state.sample_errors_sent.saturating_add(1);
                        RequestOutcome::messages(vec![raw(sample_error(
                            version,
                            ERROR_INVALID_REQUEST,
                        ))])
                    } else {
                        state.pending_requests += 1;
                        let callbacks = arm_next_credit_locked(&mut state)
                            .map(
                                |(callbacks, generation)| DeferredCameraCallback::SampleRequest {
                                    callbacks,
                                    generation,
                                },
                            )
                            .into_iter()
                            .collect();
                        RequestOutcome {
                            messages: Vec::new(),
                            callbacks,
                            logs: Vec::new(),
                        }
                    }
                }
                MSG_PROPERTY_LIST_REQUEST if payload.len() == 2 => {
                    if !state.lifecycle.is_active() {
                        RequestOutcome::messages(Self::error(version, ERROR_NOT_INITIALIZED))
                    } else {
                        // Camera controls are intentionally not virtualized yet.
                        RequestOutcome::messages(vec![raw(vec![
                            version,
                            MSG_PROPERTY_LIST_RESPONSE,
                        ])])
                    }
                }
                _ => RequestOutcome::messages(Self::error(version, ERROR_INVALID_MESSAGE)),
            }
        };
        outcome.finish()
    }
}

struct RequestOutcome {
    messages: Vec<DvcMessage>,
    callbacks: Vec<DeferredCameraCallback>,
    logs: Vec<DeferredLog>,
}

impl RequestOutcome {
    fn messages(messages: Vec<DvcMessage>) -> Self {
        Self {
            messages,
            callbacks: Vec::new(),
            logs: Vec::new(),
        }
    }

    fn finish(self) -> Vec<DvcMessage> {
        for log in self.logs {
            log.emit();
        }
        for callback in self.callbacks {
            callback.invoke();
        }
        self.messages
    }
}
