//! MS-RDPEDISP resolution requests and layout-based keyframe refresh.

#![forbid(unsafe_code)]

use ironrdp_displaycontrol::client::DisplayControlClient;
use ironrdp_displaycontrol::pdu::{
    DisplayControlMonitorLayout, DisplayControlPdu, MonitorLayoutEntry,
};
use std::sync::{Arc, Mutex};

use ironrdp_dvc::{
    DvcChannelListener, DvcClientProcessor, DvcMessage, DvcProcessor, DynamicChannelId,
};
use ironrdp_session::ActiveStage;
use ironrdp_svc::SvcMessage;

use super::egfx::NativeGfxState;
use super::{CallbackSink, NativeError, RdpLogLevel, LOG_TARGET_GRAPHICS};

/// Request the configured resolution when the DVC opens; errors preserve the existing session.
pub(super) fn make_display_control(
    width: u16,
    height: u16,
    sink: CallbackSink,
    gfx: Arc<Mutex<NativeGfxState>>,
) -> DisplayControlClient {
    DisplayControlClient::new(move |caps| {
        let (width, height) =
            MonitorLayoutEntry::adjust_display_size(u32::from(width), u32::from(height));
        // Windows ends the session a few seconds after a layout it is already using;
        // only ask when the server's own output size differs.
        if let Ok(shared) = gfx.lock() {
            if shared.graphics_width == width && shared.graphics_height == height {
                sink.log(
                    RdpLogLevel::Info,
                    LOG_TARGET_GRAPHICS,
                    format_args!("display: server already at {width}x{height}; keeping its layout"),
                );
                return Ok(Vec::new());
            }
        }
        if u64::from(width) * u64::from(height) > caps.max_monitor_area() {
            sink.log(
                RdpLogLevel::Warning,
                LOG_TARGET_GRAPHICS,
                format_args!(
                    "display: {width}x{height} exceeds the server's max monitor area; keeping the server layout"
                ),
            );
            return Ok(Vec::new());
        }
        let layout = match DisplayControlMonitorLayout::new_single_primary_monitor(
            width, height, None, None,
        ) {
            Ok(layout) => layout,
            Err(e) => {
                sink.log(
                    RdpLogLevel::Error,
                    LOG_TARGET_GRAPHICS,
                    format_args!("display: failed to build {width}x{height} monitor layout: {e}"),
                );
                return Ok(Vec::new());
            }
        };
        sink.log(
            RdpLogLevel::Info,
            LOG_TARGET_GRAPHICS,
            format_args!("display: requesting server resolution {width}x{height}"),
        );
        Ok(vec![Box::new(DisplayControlPdu::from(layout)) as DvcMessage])
    })
}

/// Create a fresh processor whenever the server reopens the display DVC.
pub(super) struct DisplayControlFactory {
    pub(super) width: u16,
    pub(super) height: u16,
    pub(super) sink: CallbackSink,
    pub(super) gfx: Arc<Mutex<NativeGfxState>>,
}

impl DvcChannelListener for DisplayControlFactory {
    fn channel_name(&self) -> &str {
        ironrdp_displaycontrol::CHANNEL_NAME
    }

    fn create(&mut self, _channel_id: DynamicChannelId) -> Option<Box<dyn DvcClientProcessor>> {
        Some(Box::new(make_display_control(
            self.width,
            self.height,
            self.sink,
            Arc::clone(&self.gfx),
        )))
    }
}

/// Re-encodes the current single-monitor layout for `RequestRefresh`, when the
/// server opened the display-control channel (mirror-mode grd does not).
pub(super) fn encode_refresh_layout(
    active: &mut ActiveStage,
    width: u16,
    height: u16,
) -> Result<Option<Vec<SvcMessage>>, NativeError> {
    let (width, height) =
        MonitorLayoutEntry::adjust_display_size(u32::from(width), u32::from(height));
    match active.get_dvc::<DisplayControlClient>() {
        Some(dvc) => match (dvc.channel_id(), dvc.processor()) {
            (channel_id, client) if client.ready() => client
                .encode_single_primary_monitor(channel_id, width, height, None, None)
                .map_err(|e| NativeError::protocol(format!("refresh layout encode: {e}")))
                .map(Some),
            _ => Ok(None),
        },
        None => Ok(None),
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use ironrdp_core::{Decode as _, ReadCursor};
    use ironrdp_displaycontrol::pdu::DisplayControlCapabilities;

    /// Include the DISPLAYCONTROL_HEADER, as sent on the wire.
    fn display_caps_payload(max_num_monitors: u32, factor_a: u32, factor_b: u32) -> Vec<u8> {
        let caps: DisplayControlPdu =
            DisplayControlCapabilities::new(max_num_monitors, factor_a, factor_b)
                .expect("caps")
                .into();
        ironrdp_core::encode_vec(&caps).expect("encode caps")
    }

    fn decode_monitor_layout(message: &DvcMessage) -> DisplayControlMonitorLayout {
        let bytes = ironrdp_core::encode_vec(message.as_ref()).expect("encode layout message");
        let mut cursor = ReadCursor::new(&bytes);
        match DisplayControlPdu::decode(&mut cursor).expect("decode DisplayControlPdu") {
            DisplayControlPdu::MonitorLayout(layout) => layout,
            other => panic!("expected MonitorLayout, got {other:?}"),
        }
    }

    #[test]
    fn display_control_pushes_configured_resolution_on_caps() {
        let payload = display_caps_payload(1, 1920, 1080);
        // Golden wire bytes: DISPLAYCONTROL_HEADER (Type=0x05 Caps, Length=0x14) + body.
        // Matches the vendored IronRDP testsuite vector shape and MS-RDPEDISP 2.2.2.1.
        assert_eq!(
            payload,
            [
                0x05, 0x00, 0x00, 0x00, // Header: Type = DISPLAYCONTROL_PDU_TYPE_CAPS
                0x14, 0x00, 0x00, 0x00, // Header: Length = 20
                0x01, 0x00, 0x00, 0x00, // MaxNumMonitors = 1
                0x80, 0x07, 0x00, 0x00, // MaxMonitorAreaFactorA = 1920
                0x38, 0x04, 0x00, 0x00, // MaxMonitorAreaFactorB = 1080
            ]
        );
        let mut client = make_display_control(
            1920,
            1080,
            CallbackSink::default(),
            Arc::new(Mutex::new(NativeGfxState::default())),
        );
        let replies = client.process(0, &payload).expect("process caps");
        assert_eq!(replies.len(), 1);
        let layout = decode_monitor_layout(&replies[0]);
        assert_eq!(layout.monitors().len(), 1);
        let monitor = &layout.monitors()[0];
        assert!(monitor.is_primary());
        assert_eq!(monitor.dimensions(), (1920, 1080));
        assert_eq!(monitor.position(), Some((0, 0)));
    }

    #[test]
    fn display_control_factory_serves_every_channel_create() {
        let mut factory = DisplayControlFactory {
            width: 1920,
            height: 1080,
            sink: CallbackSink::default(),
            gfx: Arc::new(Mutex::new(NativeGfxState::default())),
        };
        // grd closes and re-creates the channel mid-session; each create must yield a
        // live client that still pushes the configured layout on caps.
        for channel_id in [7u32, 9u32] {
            let mut client = factory
                .create(channel_id)
                .expect("factory serves every create");
            let replies = client
                .process(channel_id, &display_caps_payload(1, 8192, 8192))
                .expect("process caps");
            assert_eq!(replies.len(), 1);
            let layout = decode_monitor_layout(&replies[0]);
            assert_eq!(layout.monitors()[0].dimensions(), (1920, 1080));
        }
    }

    #[test]
    fn display_control_skips_layout_exceeding_server_area() {
        let mut client = make_display_control(
            1920,
            1080,
            CallbackSink::default(),
            Arc::new(Mutex::new(NativeGfxState::default())),
        );
        // A 640x480 max monitor area cannot fit 1920x1080; must stay silent, never error.
        let replies = client
            .process(0, &display_caps_payload(1, 640, 480))
            .expect("process caps");
        assert!(replies.is_empty());
    }

    #[test]
    fn display_control_evens_odd_width() {
        let mut client = make_display_control(
            1367,
            768,
            CallbackSink::default(),
            Arc::new(Mutex::new(NativeGfxState::default())),
        );
        let replies = client
            .process(0, &display_caps_payload(1, 8192, 8192))
            .expect("process caps");
        assert_eq!(replies.len(), 1);
        let layout = decode_monitor_layout(&replies[0]);
        assert_eq!(layout.monitors()[0].dimensions(), (1366, 768));
    }
}
