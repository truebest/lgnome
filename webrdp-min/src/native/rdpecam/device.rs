//! RDPECAM device-channel adapter and instance retirement.

use ironrdp_core::impl_as_any;
use ironrdp_dvc::{
    DvcChannelListener, DvcClientProcessor, DvcMessage, DvcProcessor, DynamicChannelId,
};
use ironrdp_pdu::PduResult;

use super::super::RdpLogLevel;
use super::bridge::{
    replace_device_locked, stop_capture_locked, CameraBridge, CameraLifecycle, DeferredLog,
    DeviceInstance,
};
use super::DEVICE_CHANNEL_NAME;

pub(in super::super) struct CameraDeviceFactory {
    bridge: CameraBridge,
}

impl CameraDeviceFactory {
    pub(in super::super) fn new(bridge: CameraBridge) -> Self {
        Self { bridge }
    }
}

impl DvcChannelListener for CameraDeviceFactory {
    fn channel_name(&self) -> &str {
        DEVICE_CHANNEL_NAME
    }

    fn create(&mut self, _channel_id: DynamicChannelId) -> Option<Box<dyn DvcClientProcessor>> {
        let token = self.bridge.allocate_device_token()?;
        Some(Box::new(CameraDevice::new(self.bridge.clone(), token)))
    }
}

pub(in super::super) struct CameraDevice {
    pub(super) bridge: CameraBridge,
    pub(super) token: u64,
    pub(super) channel_id: Option<u32>,
}

impl CameraDevice {
    pub(super) fn new(bridge: CameraBridge, token: u64) -> Self {
        Self {
            bridge,
            token,
            channel_id: None,
        }
    }
}

impl_as_any!(CameraDevice);

impl DvcProcessor for CameraDevice {
    fn channel_name(&self) -> &str {
        DEVICE_CHANNEL_NAME
    }

    fn start(&mut self, channel_id: u32) -> PduResult<Vec<DvcMessage>> {
        self.channel_id = Some(channel_id);
        let instance = DeviceInstance {
            token: self.token,
            channel_id,
        };
        let (callback, log) = if let Ok(mut state) = self.bridge.inner.lock() {
            let current_is_newer = state
                .device_instance
                .is_some_and(|current| current.token > self.token);
            if !state.available
                || !state.advertised
                || self.token <= state.retired_through_token
                || current_is_newer
            {
                (None, None)
            } else if state.device_instance == Some(instance) {
                (None, None)
            } else if state
                .device_instance
                .is_some_and(|current| current.token == self.token)
            {
                state.device_instance = Some(instance);
                (None, None)
            } else {
                let callback = replace_device_locked(&mut state, instance);
                let log = DeferredLog::new(
                    state.callbacks,
                    RdpLogLevel::Debug,
                    format!(
                        "camera: device channel attached device_token={} channel_id={channel_id}",
                        self.token,
                    ),
                );
                (callback, Some(log))
            }
        } else {
            (None, None)
        };
        if let Some(log) = log {
            log.emit();
        }
        if let Some(callback) = callback {
            callback.invoke();
        }
        Ok(Vec::new())
    }

    fn process(&mut self, channel_id: u32, payload: &[u8]) -> PduResult<Vec<DvcMessage>> {
        Ok(self.process_request(channel_id, payload))
    }

    fn close(&mut self, channel_id: u32) {
        if self.channel_id != Some(channel_id) {
            return;
        }
        self.channel_id = None;
        let instance = DeviceInstance {
            token: self.token,
            channel_id,
        };
        let callback = if let Ok(mut state) = self.bridge.inner.lock() {
            if state.device_instance != Some(instance) {
                return;
            }
            let transition = stop_capture_locked(&mut state);
            state.lifecycle = CameraLifecycle::Deactivated;
            state.activation_refs = 0;
            state.device_instance = None;
            state.retired_through_token = state.retired_through_token.max(self.token);
            transition.callback
        } else {
            None
        };
        if let Some(callback) = callback {
            callback.invoke();
        }
    }
}

impl DvcClientProcessor for CameraDevice {}
