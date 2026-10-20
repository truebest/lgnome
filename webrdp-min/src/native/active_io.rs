//! Client-originated input, media, control, graphics, and reactivation handling.

use std::sync::atomic::Ordering;
use std::sync::mpsc::TryRecvError;

use ironrdp_connector::connection_activation::ConnectionActivationState;
use ironrdp_connector::Sequence as _;
use ironrdp_core::WriteBuf;
use ironrdp_dvc::DvcMessage;
use ironrdp_graphics::image_processing::PixelFormat;
use ironrdp_pdu::geometry::InclusiveRectangle;
use ironrdp_pdu::rdp::headers::ShareDataPdu;
use ironrdp_pdu::rdp::refresh_rectangle::RefreshRectanglePdu;
use ironrdp_pdu::rdp::suppress_output::SuppressOutputPdu;
use ironrdp_session::image::DecodedImage;
use ironrdp_session::{ActiveStage, ActiveStageOutput};
use ironrdp_svc::ChannelFlags;

use super::input::{ControlCommand, InputCommand, WorkerCommand};
use super::logging::{LOG_TARGET_GRAPHICS, LOG_TARGET_SESSION};
use super::media_mailbox::{
    CameraSubmission, PendingMediaBatch, PreparedAudioSend, PreparedCameraSend,
};
use super::transport::{hex_prefix, TlsStream};
use super::{rdpedisp, NativeError, NativeWorker, RdpLogLevel};

const MAX_CANVAS_BYTES: u32 = 64 * 1024 * 1024;

fn validate_canvas(width: u16, height: u16) -> Result<(), NativeError> {
    if width == 0
        || height == 0
        || u32::from(width)
            .checked_mul(u32::from(height))
            .and_then(|pixels| pixels.checked_mul(4))
            .is_none_or(|bytes| bytes > MAX_CANVAS_BYTES)
    {
        return Err(NativeError::protocol(format!(
            "server canvas {width}x{height} exceeds the native memory limit"
        )));
    }
    Ok(())
}

pub(super) fn new_image(width: u16, height: u16) -> Result<DecodedImage, NativeError> {
    validate_canvas(width, height)?;
    Ok(DecodedImage::new(PixelFormat::RgbA32, width, height))
}

impl NativeWorker {
    fn dispatch_input(
        &mut self,
        tls: &mut TlsStream,
        active: &mut ActiveStage,
        image: &mut DecodedImage,
        input: InputCommand,
    ) -> Result<(), NativeError> {
        let event = input.into_event();
        let outputs = active
            .process_fastpath_input(image, &[event])
            .map_err(|e| NativeError::protocol(format!("fast-path input: {e}")))?;
        for output in outputs {
            if let ActiveStageOutput::ResponseFrame(frame) = output {
                self.write_all(tls, &frame, "fast-path input")?;
            }
        }
        Ok(())
    }

    pub(super) fn drain_input(
        &mut self,
        tls: &mut TlsStream,
        active: &mut ActiveStage,
        image: &mut DecodedImage,
    ) -> Result<(), NativeError> {
        // Replay anything poll_stop buffered while the worker was busy (e.g. during
        // reactivation) before draining fresh events, preserving order.
        if !self.pending_control.is_empty() {
            for control in std::mem::take(&mut self.pending_control) {
                self.dispatch_control(tls, active, image, control)?;
            }
        }
        if !self.pending_input.is_empty() {
            for input in std::mem::take(&mut self.pending_input) {
                self.dispatch_input(tls, active, image, input)?;
            }
        }
        loop {
            match self.rx.try_recv() {
                Ok(WorkerCommand::Stop) => {
                    self.stop.store(true, Ordering::SeqCst);
                    return Ok(());
                }
                Ok(WorkerCommand::Input(input)) => {
                    self.dispatch_input(tls, active, image, input)?
                }
                Ok(WorkerCommand::Control(control)) => {
                    self.dispatch_control(tls, active, image, control)?
                }
                Ok(WorkerCommand::MediaReady) => {}
                Err(TryRecvError::Empty) => return Ok(()),
                Err(TryRecvError::Disconnected) => {
                    self.stop.store(true, Ordering::SeqCst);
                    return Ok(());
                }
            }
        }
    }

    pub(super) fn drain_media(
        &mut self,
        tls: &mut TlsStream,
        active: &mut ActiveStage,
    ) -> Result<(), NativeError> {
        // Reconcile coalesced device state without blocking on the C capture-manager lock.
        let camera_available = self.desired_camera_available.load(Ordering::Acquire);
        let availability_update = self.camera.set_available(camera_available);
        if !camera_available {
            // Removal invalidates every queued answer and credit for this device instance.
            self.media
                .discard_camera_submission()
                .map_err(|()| NativeError::protocol("media mailbox lock poisoned"))?;
        }
        if let Some((channel_id, messages)) = availability_update {
            self.send_dvc_messages(
                tls,
                active,
                channel_id,
                messages,
                if camera_available {
                    "camera device added"
                } else {
                    "camera device removed"
                },
            )?;
        }

        let PendingMediaBatch {
            camera,
            audio_input,
        } = self
            .media
            .take_pending()
            .map_err(|()| NativeError::protocol("media mailbox lock poisoned"))?;

        if let Some(submission) = camera {
            // Revalidate batches already taken when C synchronously closes capture.
            let prepared = self
                .media
                .prepare_camera_send(submission)
                .map_err(|()| NativeError::protocol("media mailbox lock poisoned"))?;
            if let Some(PreparedCameraSend { submission, permit }) = prepared {
                // Hold the send permit through TLS write/flush, including stale-AU SampleError replies.
                let outbound = match submission {
                    CameraSubmission::H264 {
                        generation, data, ..
                    } => self.camera.submit_h264(generation, data),
                    CameraSubmission::Error { generation, .. } => {
                        self.camera.submit_error(generation)
                    }
                    CameraSubmission::DeferredError { .. } => {
                        unreachable!("deferred camera error was prepared for sending")
                    }
                };
                if let Some((channel_id, messages)) = outbound {
                    let result =
                        self.send_dvc_messages(tls, active, channel_id, messages, "camera sample");
                    drop(permit);
                    result?;
                }
            }
        }

        // Release the send permit before callbacks; inactive capture retains the next credit notification.
        if self
            .media
            .capture_is_active()
            .map_err(|()| NativeError::protocol("media mailbox lock poisoned"))?
        {
            self.camera.notify_next_credit();
        }

        for submission in audio_input {
            let Some(PreparedAudioSend {
                generation,
                data,
                permit,
            }) = self
                .media
                .prepare_audio_send(submission)
                .map_err(|()| NativeError::protocol("media mailbox lock poisoned"))?
            else {
                continue;
            };
            if let Some((channel_id, messages)) = self.audio_input.submit_pcm(generation, data) {
                let result =
                    self.send_dvc_messages(tls, active, channel_id, messages, "microphone data");
                drop(permit);
                result?;
            }
        }
        Ok(())
    }

    fn send_dvc_messages(
        &mut self,
        tls: &mut TlsStream,
        active: &mut ActiveStage,
        channel_id: u32,
        messages: Vec<DvcMessage>,
        label: &str,
    ) -> Result<(), NativeError> {
        let svc = ironrdp_dvc::encode_dvc_messages(channel_id, messages, ChannelFlags::empty())
            .map_err(|e| NativeError::protocol(format!("{label}: DVC encode: {e}")))?;
        let frame = active
            .encode_dvc_messages(svc)
            .map_err(|e| NativeError::protocol(format!("{label}: session encode: {e}")))?;
        self.write_all(tls, &frame, label)
    }

    fn graphics_dimensions(&self, image: &DecodedImage) -> Result<(u16, u16), NativeError> {
        let gfx = self
            .gfx
            .lock()
            .map_err(|_| NativeError::protocol("native graphics state lock poisoned"))?;
        if gfx.graphics_width == 0 || gfx.graphics_height == 0 {
            return Ok((image.width(), image.height()));
        }
        let width = u16::try_from(gfx.graphics_width)
            .map_err(|_| NativeError::protocol("graphics width exceeds the native limit"))?;
        let height = u16::try_from(gfx.graphics_height)
            .map_err(|_| NativeError::protocol("graphics height exceeds the native limit"))?;
        validate_canvas(width, height)?;
        Ok((width, height))
    }

    pub(super) fn recover_bitmap<T: std::io::Write>(
        &mut self,
        tls: &mut T,
        active: &mut ActiveStage,
        image: &DecodedImage,
    ) -> Result<(), NativeError> {
        if !active.take_bitmap_recovery_request() || self.suppress_display_latched {
            return Ok(());
        }
        let (width, height) = self.graphics_dimensions(image)?;
        let frames = active
            .request_full_redraw(
                width,
                height,
                self.refresh_rect_support,
                self.suppress_output_support,
            )
            .map_err(|e| NativeError::protocol(format!("bitmap recovery: {e}")))?;
        for frame in frames {
            self.write_all(tls, &frame, "bitmap recovery")?;
        }
        Ok(())
    }

    fn dispatch_control<T: std::io::Write>(
        &mut self,
        tls: &mut T,
        active: &mut ActiveStage,
        image: &mut DecodedImage,
        control: ControlCommand,
    ) -> Result<(), NativeError> {
        let (width, height) = self.graphics_dimensions(image)?;
        let full_rect = InclusiveRectangle {
            left: 0,
            top: 0,
            right: width.saturating_sub(1),
            bottom: height.saturating_sub(1),
        };
        match control {
            ControlCommand::SuppressOutput { allow_display } => {
                self.send_suppress_output(tls, active, full_rect, allow_display)?;
            }
            ControlCommand::RequestRefresh => {
                // Layout resubmission forces a grd IDR when Display Control is available; also try Refresh Rect.
                let layout_messages = rdpedisp::encode_refresh_layout(
                    active,
                    width,
                    height,
                    self.config.desktop_scale_factor,
                )?;
                if let Some(messages) = layout_messages {
                    let frame = active.encode_dvc_messages(messages).map_err(|e| {
                        NativeError::protocol(format!("refresh layout DVC encode: {e}"))
                    })?;
                    self.write_all(tls, &frame, "refresh monitor layout")?;
                    self.callbacks.log(
                        RdpLogLevel::Info,
                        LOG_TARGET_GRAPHICS,
                        format_args!(
                            "refresh requested: re-submitted monitor layout for a fresh keyframe"
                        ),
                    );
                } else {
                    self.callbacks.log(
                        RdpLogLevel::Warning,
                        LOG_TARGET_GRAPHICS,
                        format_args!(
                            "refresh requested but the display-control channel is unavailable; \
                             relying on Refresh Rect only"
                        ),
                    );
                }

                let pdu = ShareDataPdu::RefreshRectangle(RefreshRectanglePdu {
                    areas_to_refresh: vec![full_rect],
                });
                let mut buf = WriteBuf::new();
                active
                    .encode_static(&mut buf, pdu)
                    .map_err(|e| NativeError::protocol(format!("refresh rect encode: {e}")))?;
                self.write_all(tls, buf.filled(), "refresh rect")?;
            }
        }
        Ok(())
    }

    fn send_suppress_output<T: std::io::Write>(
        &mut self,
        tls: &mut T,
        active: &mut ActiveStage,
        full_rect: InclusiveRectangle,
        allow_display: bool,
    ) -> Result<(), NativeError> {
        self.suppress_display_latched = !allow_display;
        let pdu = ShareDataPdu::SuppressOutput(SuppressOutputPdu {
            // Per MS-RDPBCGR the desktop rectangle is present exactly when display
            // updates are re-allowed.
            desktop_rect: allow_display.then_some(full_rect),
        });
        let mut buf = WriteBuf::new();
        active
            .encode_static(&mut buf, pdu)
            .map_err(|e| NativeError::protocol(format!("suppress output encode: {e}")))?;
        self.write_all(tls, buf.filled(), "suppress output")?;
        self.callbacks.log(
            RdpLogLevel::Info,
            LOG_TARGET_SESSION,
            format_args!(
                "display updates {} by client request",
                if allow_display {
                    "resumed"
                } else {
                    "suppressed"
                }
            ),
        );
        Ok(())
    }

    pub(super) fn drive_reactivation(
        &mut self,
        tls: &mut TlsStream,
        active: &mut ActiveStage,
        image: &mut DecodedImage,
    ) -> Result<(), NativeError> {
        self.phase_deadline =
            Some(std::time::Instant::now() + super::transport::REACTIVATE_TIMEOUT);
        tls.sock.phase_deadline = self.phase_deadline;
        let factory = self
            .activation_factory
            .as_ref()
            .ok_or_else(|| NativeError::protocol("deactivate-all with no activation factory"))?;
        let mut seq = factory.create();
        let mut out = WriteBuf::new();
        loop {
            self.check_phase_deadline("reactivation")?;
            if self.drain_commands() {
                return Err(NativeError::network("reactivation stopped"));
            }
            if let ConnectionActivationState::Finalized {
                desktop_size,
                share_id,
                enable_server_pointer,
                pointer_software_rendering,
                static_channel_chunk_size,
                refresh_rect_support,
                suppress_output_support,
                ..
            } = seq.connection_activation_state()
            {
                *image = new_image(desktop_size.width, desktop_size.height)?;
                self.refresh_rect_support = refresh_rect_support;
                self.suppress_output_support = suppress_output_support;
                if let Ok(mut gfx) = self.gfx.lock() {
                    gfx.graphics_width = 0;
                    gfx.graphics_height = 0;
                }
                // Reactivation renegotiates VCChunkSize; applying the rest by hand would
                // leave the X.224 processor chunking to the previous, possibly larger, limit.
                if !active.reactivate(
                    seq.io_channel_id(),
                    seq.user_channel_id(),
                    share_id,
                    enable_server_pointer,
                    pointer_software_rendering,
                    static_channel_chunk_size,
                ) {
                    return Err(NativeError::protocol(format!(
                        "reactivation offered an unusable static channel chunk size ({static_channel_chunk_size})"
                    )));
                }
                self.callbacks
                    .desktop_size(desktop_size.width, desktop_size.height);
                self.write_all(tls, out.filled(), "reactivation finalized")?;
                self.phase_deadline = None;
                tls.sock.phase_deadline = None;
                return Ok(());
            }

            if seq.next_pdu_hint().is_some() {
                let pdu = self.read_activation_pdu(tls, &seq)?;
                seq.step(&pdu, self.last_read_at, &mut out).map_err(|e| {
                    let mut error = NativeError::connector(e);
                    error.message = format!(
                        "{}; pdu {} bytes: {}",
                        error.message,
                        pdu.len(),
                        hex_prefix(&pdu, 64)
                    );
                    error
                })?;
            } else {
                seq.step_no_input(&mut out)
                    .map_err(NativeError::connector)?;
            }

            if !out.filled().is_empty() {
                self.write_all(tls, out.filled(), "reactivation output")?;
                out = WriteBuf::new();
            }
        }
    }

    pub(super) fn drain_gfx(&mut self) -> Result<(), NativeError> {
        let (video_units, bitmap_units, unsupported_graphics, graphics_size) = match self.gfx.lock()
        {
            Ok(mut shared) => {
                let graphics_size = if shared.graphics_size_pending {
                    shared.graphics_size_pending = false;
                    Some((shared.graphics_width, shared.graphics_height))
                } else {
                    None
                };
                (
                    std::mem::take(&mut shared.pending_video),
                    std::mem::take(&mut shared.pending_bitmap),
                    shared.unsupported_graphics.take(),
                    graphics_size,
                )
            }
            Err(_) => return Err(NativeError::protocol("native graphics state lock poisoned")),
        };
        if let Some(detail) = unsupported_graphics {
            return Err(NativeError::no_avc420(detail));
        }
        // Dispatched before video/bitmap units so both the ss4s/H.264 and RemoteFX paths
        // see the server's real graphics output size before processing this batch's frames.
        if let Some((width, height)) = graphics_size {
            let width = width.min(u32::from(u16::MAX)) as u16;
            let height = height.min(u32::from(u16::MAX)) as u16;
            self.callbacks.desktop_size(width, height);
        }
        for unit in bitmap_units {
            self.callbacks.bitmap_update(&unit);
        }
        for unit in video_units {
            let pts = self.next_pts90k;
            self.next_pts90k = self.next_pts90k.wrapping_add(self.frame_pts_step.max(1));
            self.callbacks.video_au(unit.codec, &unit.data, pts);
        }
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn drain_preserves_codec_per_au_across_a_batch() {
        use super::super::abi::{RDP_VIDEO_CODEC_H264, RDP_VIDEO_CODEC_H265};
        use super::super::egfx::NativeVideoUnit;
        use std::sync::Mutex;

        extern "C" fn video(
            ctx: *mut core::ffi::c_void,
            codec: u32,
            _data: *const u8,
            _len: usize,
            _pts: u64,
        ) {
            let seen = unsafe { &*ctx.cast::<Mutex<Vec<u32>>>() };
            seen.lock().unwrap().push(codec);
        }

        let (mut worker, _tx) = super::super::tests::test_worker();
        let seen = Mutex::new(Vec::<u32>::new());
        worker.callbacks.callbacks.ctx = (&seen as *const Mutex<Vec<u32>>).cast_mut().cast();
        worker.callbacks.callbacks.on_video_au = Some(video);
        for codec in [
            RDP_VIDEO_CODEC_H265,
            RDP_VIDEO_CODEC_H264,
            RDP_VIDEO_CODEC_H265,
        ] {
            worker
                .gfx
                .lock()
                .unwrap()
                .pending_video
                .push(NativeVideoUnit {
                    codec,
                    data: vec![1],
                });
        }
        worker.drain_gfx().unwrap();
        assert_eq!(
            *seen.lock().unwrap(),
            [
                RDP_VIDEO_CODEC_H265,
                RDP_VIDEO_CODEC_H264,
                RDP_VIDEO_CODEC_H265
            ]
        );
    }
    use ironrdp_core::decode;
    use ironrdp_pdu::rdp::headers::{ShareControlHeader, ShareControlPdu};
    use ironrdp_pdu::{
        mcs::{McsMessage, SendDataRequest},
        x224::X224,
    };

    fn active_stage() -> ActiveStage {
        ironrdp_session::ActiveStageBuilder {
            static_channels: ironrdp_svc::StaticChannelSet::new(),
            user_channel_id: 1001,
            io_channel_id: 1003,
            message_channel_id: None,
            share_id: 1,
            compression_type: None,
            enable_server_pointer: false,
            pointer_software_rendering: false,
        }
        .build()
    }

    fn share_data(bytes: &[u8]) -> ShareDataPdu {
        let X224(McsMessage::SendDataRequest(SendDataRequest { user_data, .. })) =
            decode(bytes).unwrap()
        else {
            panic!("MCS request expected")
        };
        let header: ShareControlHeader = decode(&user_data).unwrap();
        let ShareControlPdu::Data(data) = header.share_control_pdu else {
            panic!("share data expected")
        };
        data.share_data_pdu
    }

    #[test]
    fn canvas_bounds_precede_image_allocation() {
        for (width, height) in [
            (0, 1080),
            (1920, 0),
            (32768, 32768),
            (u16::MAX, u16::MAX),
            (8192, 8192),
        ] {
            assert!(new_image(width, height).is_err());
        }
        validate_canvas(4096, 4096).unwrap();
        assert_eq!(new_image(1280, 720).unwrap().data().len(), 1280 * 720 * 4);
    }

    #[test]
    fn controls_use_current_graphics_size_without_bitmap_reactivation() {
        let (mut worker, _tx) = super::super::tests::test_worker();
        let mut stage = active_stage();
        let mut image = new_image(1280, 720).unwrap();
        assert_eq!(worker.graphics_dimensions(&image).unwrap(), (1280, 720));
        {
            let mut gfx = worker.gfx.lock().unwrap();
            gfx.graphics_width = 1920;
            gfx.graphics_height = 1080;
        }
        assert_eq!(worker.graphics_dimensions(&image).unwrap(), (1920, 1080));
        for control in [
            ControlCommand::RequestRefresh,
            ControlCommand::SuppressOutput {
                allow_display: true,
            },
        ] {
            let mut wire = Vec::new();
            worker
                .dispatch_control(&mut wire, &mut stage, &mut image, control)
                .unwrap();
            let rect = match share_data(&wire) {
                ShareDataPdu::RefreshRectangle(refresh) => refresh.areas_to_refresh[0].clone(),
                ShareDataPdu::SuppressOutput(suppress) => suppress.desktop_rect.unwrap(),
                other => panic!("unexpected control {other:?}"),
            };
            assert_eq!((rect.right, rect.bottom), (1919, 1079));
        }
    }
    #[test]
    fn malformed_bitmap_recovers_once_with_negotiated_capabilities_and_respects_suppression() {
        use ironrdp_core::encode_vec;
        use ironrdp_pdu::{
            fast_path::{EncryptionFlags, FastPathHeader, UpdateCode},
            Action,
        };
        let mut malformed =
            encode_vec(&FastPathHeader::new(EncryptionFlags::empty(), 1 + 2 + 3200)).unwrap();
        malformed.push(UpdateCode::Bitmap.as_u8());
        malformed.extend_from_slice(&48788u16.to_le_bytes());
        malformed.resize(malformed.len() + 3200, 0);
        for (refresh, suppress, background, count) in [
            (true, false, false, 1),
            (false, true, false, 2),
            (false, false, false, 0),
            (true, true, true, 0),
        ] {
            let (mut worker, _tx) = super::super::tests::test_worker();
            worker.refresh_rect_support = refresh;
            worker.suppress_output_support = suppress;
            worker.suppress_display_latched = background;
            let mut stage = active_stage();
            let mut image = new_image(1, 1).unwrap();
            stage
                .process(&mut image, Action::FastPath, &malformed)
                .unwrap();
            let mut wire = Vec::new();
            worker
                .recover_bitmap(&mut wire, &mut stage, &image)
                .unwrap();
            let mut remaining = wire.as_slice();
            let mut frames = 0;
            while let Some(info) = ironrdp_pdu::find_size(remaining).unwrap() {
                frames += 1;
                remaining = &remaining[info.length..];
            }
            assert_eq!(frames, count);
            wire.clear();
            stage
                .process(&mut image, Action::FastPath, &malformed)
                .unwrap();
            worker
                .recover_bitmap(&mut wire, &mut stage, &image)
                .unwrap();
            assert!(wire.is_empty());
        }
    }

    #[test]
    fn refresh_layout_uses_current_egfx_dimensions() {
        use ironrdp_core::encode_vec;
        use ironrdp_displaycontrol::{
            client::DisplayControlClient,
            pdu::{DisplayControlCapabilities, DisplayControlPdu},
        };
        use ironrdp_dvc::pdu::{
            CreateRequestPdu, DrdynvcClientPdu, DrdynvcDataPdu, DrdynvcServerPdu,
        };
        use ironrdp_dvc::{DrdynvcClient, DvcProcessor};
        use ironrdp_svc::SvcProcessor;
        for scale in [100, 150, 200] {
            let (mut worker, _tx) = super::super::tests::test_worker();
            worker.config.desktop_scale_factor = scale;
            let mut display = DisplayControlClient::new(|_| Ok(Vec::new()));
            let caps: DisplayControlPdu = DisplayControlCapabilities::new(1, 8192, 8192)
                .unwrap()
                .into();
            display.process(7, &encode_vec(&caps).unwrap()).unwrap();
            let mut dvc = DrdynvcClient::new().with_dynamic_channel(display);
            dvc.process(
                &encode_vec(&DrdynvcServerPdu::Create(CreateRequestPdu::new(
                    7,
                    ironrdp_displaycontrol::CHANNEL_NAME.to_owned(),
                )))
                .unwrap(),
            )
            .unwrap();
            let mut channels = ironrdp_svc::StaticChannelSet::new();
            channels.insert(dvc);
            channels.attach_channel_id(core::any::TypeId::of::<DrdynvcClient>(), 1005);
            let mut stage = ironrdp_session::ActiveStageBuilder {
                static_channels: channels,
                user_channel_id: 1001,
                io_channel_id: 1003,
                message_channel_id: None,
                share_id: 1,
                compression_type: None,
                enable_server_pointer: false,
                pointer_software_rendering: false,
            }
            .build();
            let mut image = new_image(1280, 720).unwrap();
            {
                let mut gfx = worker.gfx.lock().unwrap();
                gfx.graphics_width = 1920;
                gfx.graphics_height = 1080;
            }
            let mut wire = Vec::new();
            worker
                .dispatch_control(
                    &mut wire,
                    &mut stage,
                    &mut image,
                    ControlCommand::RequestRefresh,
                )
                .unwrap();
            let info = ironrdp_pdu::find_size(&wire).unwrap().unwrap();
            let X224(McsMessage::SendDataRequest(data)) = decode(&wire[..info.length]).unwrap()
            else {
                panic!("MCS request expected")
            };
            let DrdynvcClientPdu::Data(DrdynvcDataPdu::Data(data)) =
                decode(&data.user_data[8..]).unwrap()
            else {
                panic!("DVC data expected")
            };
            let DisplayControlPdu::MonitorLayout(layout) = decode(data.data()).unwrap() else {
                panic!("monitor layout expected")
            };
            assert_eq!(layout.monitors()[0].dimensions(), (1920, 1080));
            assert_eq!(layout.monitors()[0].desktop_scale_factor(), Some(scale));
            assert_eq!(
                layout.monitors()[0].device_scale_factor().unwrap().value(),
                100
            );
            let ShareDataPdu::RefreshRectangle(refresh) = share_data(&wire[info.length..]) else {
                panic!("refresh rect expected")
            };
            assert_eq!(
                (
                    refresh.areas_to_refresh[0].right,
                    refresh.areas_to_refresh[0].bottom
                ),
                (1919, 1079)
            );
        }
    }
}
