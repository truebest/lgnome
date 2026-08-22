//! Display Control (MS-RDPEDISP): pushes the configured resolution at connect
//! and re-submits the current layout on refresh, because gnome-remote-desktop
//! recreates its encode sessions (ending in RESET_GRAPHICS and a fresh IDR) on
//! every monitor-layout submission — even a byte-identical one.

#![forbid(unsafe_code)]

use ironrdp_displaycontrol::client::DisplayControlClient;
use ironrdp_displaycontrol::pdu::{
    DisplayControlMonitorLayout, DisplayControlPdu, MonitorLayoutEntry,
};
use ironrdp_dvc::{DvcChannelListener, DvcMessage, DvcProcessor, DynamicChannelId};
use ironrdp_session::ActiveStage;
use ironrdp_svc::SvcMessage;

use super::{CallbackSink, NativeError, RdpLogLevel, LOG_TARGET_GRAPHICS};

/// Builds the client that dictates the server's monitor resolution the moment
/// the channel becomes operational (server capabilities received — before the
/// video stream starts). Headless hosts otherwise come up with virtual-display
/// defaults like 2048x1152 that the TV's hardware video pipeline silently
/// cannot start on. Failures log and send nothing: a DVC processor error is
/// session-fatal, and an unchanged server layout is a working (if suboptimal)
/// session.
pub(super) fn make_display_control(
    width: u16,
    height: u16,
    sink: CallbackSink,
) -> DisplayControlClient {
    DisplayControlClient::new(move |caps| {
        let (width, height) =
            MonitorLayoutEntry::adjust_display_size(u32::from(width), u32::from(height));
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

/// Serves a fresh [`DisplayControlClient`] for every DYNVC_CREATE_REQ. A once-registered
/// processor (`with_dynamic_channel`) is consumed by the first create, so a server that
/// closes and re-creates this channel would leave `rdp_request_refresh`'s layout-resubmit
/// path without a channel for the rest of the connection. Defensive: the old grd builds
/// in the field (≤45) never open this channel at all — against them every refresh takes
/// the reconnect fallback regardless.
pub(super) struct DisplayControlFactory {
    pub(super) width: u16,
    pub(super) height: u16,
    pub(super) sink: CallbackSink,
}

impl DvcChannelListener for DisplayControlFactory {
    fn channel_name(&self) -> &str {
        ironrdp_displaycontrol::CHANNEL_NAME
    }

    fn create(&mut self, _channel_id: DynamicChannelId) -> Option<Box<dyn DvcProcessor>> {
        Some(Box::new(make_display_control(
            self.width,
            self.height,
            self.sink,
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
        Some(dvc) => match (
            dvc.channel_id(),
            dvc.channel_processor_downcast_ref::<DisplayControlClient>(),
        ) {
            (Some(channel_id), Some(client)) if client.ready() => client
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

    /// Encodes the full `DISPLAYCONTROL_CAPS_PDU` as a real server puts it on the wire:
    /// the `DISPLAYCONTROL_HEADER` (Type + Length) followed by the capability set
    /// (MS-RDPEDISP 2.2.2.1). Feeding the raw capability body without the header would
    /// exercise a wire shape no server ever sends.
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
        let mut client = make_display_control(1920, 1080, CallbackSink::empty());
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
            sink: CallbackSink::empty(),
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
        let mut client = make_display_control(1920, 1080, CallbackSink::empty());
        // A 640x480 max monitor area cannot fit 1920x1080; must stay silent, never error.
        let replies = client
            .process(0, &display_caps_payload(1, 640, 480))
            .expect("process caps");
        assert!(replies.is_empty());
    }

    #[test]
    fn display_control_evens_odd_width() {
        let mut client = make_display_control(1367, 768, CallbackSink::empty());
        let replies = client
            .process(0, &display_caps_payload(1, 8192, 8192))
            .expect("process caps");
        assert_eq!(replies.len(), 1);
        let layout = decode_monitor_layout(&replies[0]);
        assert_eq!(layout.monitors()[0].dimensions(), (1366, 768));
    }
}
