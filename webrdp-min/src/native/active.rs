//! ActiveStage session loop and server-output fan-out.

use std::io::Read;
use std::sync::atomic::Ordering;

use ironrdp_connector::ConnectionResult;
use ironrdp_graphics::image_processing::PixelFormat;
use ironrdp_pdu::geometry::InclusiveRectangle;
use ironrdp_pdu::geometry::Rectangle as _;
use ironrdp_session::image::DecodedImage;
use ironrdp_session::{ActiveStageBuilder, ActiveStageOutput};

use super::egfx::NativeBitmapUnit;
use super::input::ControlCommand;
use super::transport::{is_timeout, monotonic_now, TlsStream};
use super::{
    NativeError, NativeWorker, RdpState, RDP_POINTER_STATE_DEFAULT, RDP_POINTER_STATE_HIDDEN,
};

impl NativeWorker {
    pub(super) fn run_active(
        &mut self,
        mut tls: TlsStream,
        result: ConnectionResult,
    ) -> Result<(), NativeError> {
        let desktop_w = result.desktop_size.width;
        let desktop_h = result.desktop_size.height;
        tracing::info!(target: super::LOG_TARGET_SESSION,
            requested_width = self.config.width, requested_height = self.config.height,
            negotiated_width = desktop_w, negotiated_height = desktop_h,
            compression = ?result.compression_type,
            server_pointer = result.enable_server_pointer,
            "RDP activation complete; actual EGFX size and codec follow server graphics PDUs");
        self.callbacks.desktop_size(desktop_w, desktop_h);
        self.callbacks.emit_state(
            RdpState::Active,
            format!("active {}x{} native AVC420/RemoteFX", desktop_w, desktop_h),
        );

        self.activation_factory = Some(result.activation_factory);
        let mut active = ActiveStageBuilder {
            static_channels: result.static_channels,
            user_channel_id: result.user_channel_id,
            io_channel_id: result.io_channel_id,
            message_channel_id: result.message_channel_id,
            share_id: result.share_id,
            compression_type: result.compression_type,
            enable_server_pointer: result.enable_server_pointer,
            pointer_software_rendering: result.pointer_software_rendering,
        }
        .build();
        let mut image = DecodedImage::new(PixelFormat::RgbA32, desktop_w, desktop_h);

        // Discard any input poll_stop buffered during the pre-connect phase; the C side does
        // not send input until the session is active, so this is normally empty, and replaying
        // stale pre-connect events into a fresh session would be wrong.
        self.pending_input.clear();

        // Reapply suppression unless a newer toggle is already queued.
        let suppress_toggle_pending = self
            .pending_control
            .iter()
            .any(|c| matches!(c, ControlCommand::SuppressOutput { .. }));
        if self.suppress_display_latched && !suppress_toggle_pending {
            self.pending_control.push(ControlCommand::SuppressOutput {
                allow_display: false,
            });
        }

        loop {
            if self.stop.load(Ordering::SeqCst) {
                return Ok(());
            }
            self.drain_input(&mut tls, &mut active, &mut image)?;
            self.drain_media(&mut tls, &mut active)?;
            if self.stop.load(Ordering::SeqCst) {
                return Ok(());
            }
            self.drain_gfx()?;

            while let Some(info) = ironrdp_pdu::find_size(&self.inbuf)
                .map_err(|e| NativeError::protocol(format!("active PDU size: {e}")))?
            {
                if self.inbuf.len() < info.length {
                    break;
                }
                let frame: Vec<u8> = self.inbuf.drain(..info.length).collect();
                let outputs = active
                    .process(&mut image, info.action, &frame)
                    .map_err(|e| NativeError::protocol(format!("active stage process: {e}")))?;
                let deactivate = self.handle_active_outputs(&mut tls, &image, outputs)?;
                self.drain_gfx()?;

                if deactivate {
                    self.drive_reactivation(&mut tls, &mut active, &mut image)?;
                }
            }

            let mut buf = [0u8; 8192];
            match tls.read(&mut buf) {
                Ok(0) => return Err(NativeError::network("RDP server closed the TLS stream")),
                Ok(n) => {
                    self.last_read_at = Some(monotonic_now());
                    self.inbuf.extend_from_slice(&buf[..n]);
                }
                Err(e) if is_timeout(&e) => {}
                Err(e) => return Err(NativeError::network(format!("active read: {e}"))),
            }
        }
    }

    /// Fans one batch of ActiveStage outputs out to the wire and the C
    /// callbacks; returns true when the server requested Deactivate-All.
    fn handle_active_outputs(
        &mut self,
        tls: &mut TlsStream,
        image: &DecodedImage,
        outputs: Vec<ActiveStageOutput>,
    ) -> Result<bool, NativeError> {
        let mut deactivate = false;
        for output in outputs {
            match output {
                ActiveStageOutput::ResponseFrame(frame) => {
                    self.write_all(tls, &frame, "active response")?
                }
                ActiveStageOutput::Terminate(reason) => {
                    return Err(NativeError::disconnect(reason));
                }
                ActiveStageOutput::DeactivateAll => deactivate = true,
                ActiveStageOutput::GraphicsUpdate(rect) => {
                    self.forward_slowpath_bitmap(image, &rect);
                }
                ActiveStageOutput::PointerBitmap(pointer) => {
                    // IronRDP represents an invisible pointer as a zero-sized bitmap.
                    if pointer.width == 0 || pointer.height == 0 {
                        self.callbacks.pointer_state(RDP_POINTER_STATE_HIDDEN);
                    } else {
                        self.callbacks.pointer_bitmap(&pointer);
                    }
                }
                ActiveStageOutput::PointerPosition { x, y } => {
                    self.callbacks.pointer_position(x, y);
                }
                ActiveStageOutput::PointerHidden => {
                    self.callbacks.pointer_state(RDP_POINTER_STATE_HIDDEN);
                }
                ActiveStageOutput::PointerDefault => {
                    self.callbacks.pointer_state(RDP_POINTER_STATE_DEFAULT);
                }
                _ => {}
            }
        }
        Ok(deactivate)
    }

    /// data_for_rect retains the full image stride, not the rectangle width.
    fn forward_slowpath_bitmap(&mut self, image: &DecodedImage, rect: &InclusiveRectangle) {
        let width = u32::from(rect.width());
        let height = u32::from(rect.height());
        if width > 0 && height > 0 {
            self.callbacks.bitmap_update(&NativeBitmapUnit {
                surface_id: 0,
                left: u32::from(rect.left),
                top: u32::from(rect.top),
                width,
                height,
                stride: image.stride() as u32,
                data: image.data_for_rect(rect).to_vec(),
            });
        }
    }
}
