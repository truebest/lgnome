//! RDPECAM device enumerator dynamic-channel handler.

use ironrdp_core::impl_as_any;
use ironrdp_dvc::{DvcMessage, DvcProcessor};
use ironrdp_pdu::PduResult;

use super::super::{RdpLogLevel, LOG_TARGET_CAMERA};
use super::bridge::{advertise_device_locked, retire_device_locked, CameraBridge, DeferredLog};
use super::wire::{raw, MSG_SELECT_VERSION_REQUEST, MSG_SELECT_VERSION_RESPONSE, PROTOCOL_VERSION};
use super::ENUMERATOR_CHANNEL_NAME;

pub(in super::super) struct CameraEnumerator {
    bridge: CameraBridge,
    channel_id: Option<u32>,
}

impl CameraEnumerator {
    pub(in super::super) fn new(bridge: CameraBridge) -> Self {
        Self {
            bridge,
            channel_id: None,
        }
    }
}

impl_as_any!(CameraEnumerator);

impl DvcProcessor for CameraEnumerator {
    fn channel_name(&self) -> &str {
        ENUMERATOR_CHANNEL_NAME
    }

    fn start(&mut self, channel_id: u32) -> PduResult<Vec<DvcMessage>> {
        self.channel_id = Some(channel_id);
        let stop = if let Ok(mut state) = self.bridge.inner.lock() {
            state.enum_channel_id = Some(channel_id);
            state.enum_negotiated = false;
            state.advertised = false;
            retire_device_locked(&mut state)
        } else {
            None
        };
        if let Some(stop) = stop {
            stop.invoke();
        }
        Ok(vec![raw(vec![
            PROTOCOL_VERSION,
            MSG_SELECT_VERSION_REQUEST,
        ])])
    }

    fn process(&mut self, channel_id: u32, payload: &[u8]) -> PduResult<Vec<DvcMessage>> {
        if self.channel_id != Some(channel_id) {
            return Ok(Vec::new());
        }
        if payload.len() != 2
            || payload[1] != MSG_SELECT_VERSION_RESPONSE
            || !(1..=PROTOCOL_VERSION).contains(&payload[0])
        {
            let callbacks = self.bridge.inner.lock().ok().map(|state| state.callbacks);
            if let Some(callbacks) = callbacks {
                callbacks.log(
                    RdpLogLevel::Warning,
                    LOG_TARGET_CAMERA,
                    format_args!(
                        "camera: invalid RDPECAM version response ({} bytes)",
                        payload.len()
                    ),
                );
            }
            return Ok(Vec::new());
        }

        let (notification, log) = {
            let mut state = match self.bridge.inner.lock() {
                Ok(state) => state,
                Err(_) => return Ok(Vec::new()),
            };
            if state.enum_channel_id != Some(channel_id) {
                return Ok(Vec::new());
            }
            state.version = payload[0];
            state.enum_negotiated = true;
            let notification = advertise_device_locked(&mut state);
            let log = notification.as_ref().map(|_| {
                DeferredLog::new(
                    state.callbacks,
                    RdpLogLevel::Info,
                    format!(
                        "camera: TX DeviceAdded {}x{}@{} H.264 state={}",
                        state.width,
                        state.height,
                        state.fps,
                        state.lifecycle.name(),
                    ),
                )
            });
            (notification, log)
        };
        if let Some(log) = log {
            log.emit();
        }
        Ok(notification.map_or_else(Vec::new, |(_, messages)| messages))
    }

    fn close(&mut self, channel_id: u32) {
        if self.channel_id != Some(channel_id) {
            return;
        }
        self.channel_id = None;
        let stop = if let Ok(mut state) = self.bridge.inner.lock() {
            if state.enum_channel_id != Some(channel_id) {
                return;
            }
            state.enum_channel_id = None;
            state.enum_negotiated = false;
            state.advertised = false;
            retire_device_locked(&mut state)
        } else {
            None
        };
        if let Some(stop) = stop {
            stop.invoke();
        }
    }
}
