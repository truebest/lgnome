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
use ironrdp_session::{fast_path, ActiveStage, ActiveStageOutput};
use ironrdp_svc::ChannelFlags;

use super::input::{ControlCommand, InputCommand, WorkerCommand};
use super::logging::{LOG_TARGET_GRAPHICS, LOG_TARGET_SESSION};
use super::media_mailbox::{
    CameraSubmission, PendingMediaBatch, PreparedAudioSend, PreparedCameraSend,
};
use super::transport::{hex_prefix, TlsStream};
use super::{rdpedisp, NativeError, NativeWorker, RdpLogLevel};

impl NativeWorker {
    fn dispatch_input(
        &mut self,
        tls: &mut TlsStream,
        active: &mut ActiveStage,
        image: &mut DecodedImage,
        input: InputCommand,
    ) -> Result<(), NativeError> {
        let events = input.into_events();
        let outputs = active
            .process_fastpath_input(image, &events)
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
        // Reconcile desired state on every pass. A rapid remove -> add may coalesce,
        // but can never leave the worker stuck at an obsolete edge-mailbox value.
        // The atomic store originates under the C capture-manager lock and must not
        // block behind an RDP callback trying to acquire that lock.
        let camera_available = self.desired_camera_available.load(Ordering::Acquire);
        let availability_update = self.camera.set_available(camera_available);
        if !camera_available {
            // DeviceRemoved retires the old device and all of its sample credits.
            // Drop every queued answer for that instance, including a current-epoch
            // answer when physical/permission removal leaves the foreground capture
            // gate open. None may leak into a later DeviceAdded instance.
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
            // set_capture_active also rewrites an H.264 AU still in the mailbox.
            // Revalidate here as well because the worker may already have taken a
            // batch when C synchronously backgrounds the session.
            let prepared = self
                .media
                .prepare_camera_send(submission)
                .map_err(|()| NativeError::protocol("media mailbox lock poisoned"))?;
            if let Some(PreparedCameraSend { submission, permit }) = prepared {
                // A stale H.264 answer becomes SampleError only after capture has
                // reopened. Every wire response, including that fallback, holds the
                // send permit through TLS write+flush so close is synchronous.
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

        // submit_h264/submit_error arm the next server credit before the DVC
        // write. Release the write permit first, then suppress the callback if a
        // concurrent close already made capture inactive. A coalesced reopen
        // delivers any retained notification on this or the next drain pass.
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

    /// Sends one session-control request on the live connection. Failures here are
    /// connection failures (the encode paths are infallible for valid state), so they
    /// propagate like any other write error.
    fn dispatch_control(
        &mut self,
        tls: &mut TlsStream,
        active: &mut ActiveStage,
        image: &mut DecodedImage,
        control: ControlCommand,
    ) -> Result<(), NativeError> {
        let full_rect = InclusiveRectangle {
            left: 0,
            top: 0,
            right: image.width().saturating_sub(1),
            bottom: image.height().saturating_sub(1),
        };
        match control {
            ControlCommand::SuppressOutput { allow_display } => {
                self.send_suppress_output(tls, active, full_rect, allow_display)?;
            }
            ControlCommand::RequestRefresh => {
                // gnome-remote-desktop disables TS_REFRESH_RECT_PDU (FreeRDP_RefreshRect =
                // FALSE) and does not force a keyframe when suppressed output resumes, but
                // it tears down and recreates its encode sessions on EVERY Display Control
                // monitor-layout submission — even a byte-identical one — ending in a
                // RESET_GRAPHICS and a fresh IDR. So re-submit the current layout over the
                // Display Control DVC (when the server opened it; mirror-mode grd does
                // not), and also send a full-screen Refresh Rect for servers that honor
                // the classic path.
                let layout_messages =
                    rdpedisp::encode_refresh_layout(active, image.width(), image.height())?;
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

    fn send_suppress_output(
        &mut self,
        tls: &mut TlsStream,
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
        let factory = self
            .activation_factory
            .as_ref()
            .ok_or_else(|| NativeError::protocol("deactivate-all with no activation factory"))?;
        let mut seq = factory.create();
        let mut out = WriteBuf::new();
        loop {
            if let ConnectionActivationState::Finalized {
                desktop_size,
                share_id,
                enable_server_pointer,
                pointer_software_rendering,
            } = seq.connection_activation_state()
            {
                *image =
                    DecodedImage::new(PixelFormat::RgbA32, desktop_size.width, desktop_size.height);
                active.set_fastpath_processor(
                    fast_path::ProcessorBuilder {
                        io_channel_id: seq.io_channel_id(),
                        user_channel_id: seq.user_channel_id(),
                        share_id,
                        enable_server_pointer,
                        pointer_software_rendering,
                        bulk_decompressor: None,
                    }
                    .build(),
                );
                active.set_share_id(share_id);
                active.set_enable_server_pointer(enable_server_pointer);
                self.callbacks
                    .desktop_size(desktop_size.width, desktop_size.height);
                self.write_all(tls, out.filled(), "reactivation finalized")?;
                return Ok(());
            }

            if seq.next_pdu_hint().is_some() {
                let pdu = self.read_activation_pdu(tls, &seq)?;
                seq.step(&pdu, &mut out).map_err(|e| {
                    // grd is known to send nonstandard PDUs during reactivation; keep the
                    // bytes so a field failure identifies the offending PDU from the log
                    // alone, without a verbose-tracing rebuild.
                    NativeError::protocol(format!(
                        "reactivation step: {e}; pdu {} bytes: {}",
                        pdu.len(),
                        hex_prefix(&pdu, 64)
                    ))
                })?;
            } else {
                seq.step_no_input(&mut out).map_err(|e| {
                    NativeError::protocol(format!("reactivation step_no_input: {e}"))
                })?;
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
            self.callbacks.video_au(&unit.data, pts);
        }
        Ok(())
    }
}
