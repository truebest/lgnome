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
use super::transport::{is_timeout, TlsStream};
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

        // Re-assert a commanded suppression on this fresh connection (see the latch field)
        // — unless ANY suppress toggle is already pending: a queued resume (the user
        // switched to this session mid-reconnect) is newer intent and must not be
        // overridden by the stale latch.
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
                Ok(n) => self.inbuf.extend_from_slice(&buf[..n]),
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
                    // Any Terminate we receive is server-side origin (a client
                    // stop closes the TCP stream without one), and
                    // gnome-remote-desktop sends its handoff ultimatum with the
                    // reason wired as UserRequested — so every reason must reach
                    // the reconnect loop in run(), which matches this message and
                    // is guarded by the stop flag for genuine client stops.
                    return Err(NativeError::protocol(format!(
                        "received disconnect provider ultimatum: {}",
                        reason.description()
                    )));
                }
                ActiveStageOutput::DeactivateAll => deactivate = true,
                ActiveStageOutput::GraphicsUpdate(rect) => {
                    self.forward_slowpath_bitmap(image, &rect);
                }
                ActiveStageOutput::PointerBitmap(pointer) => {
                    // A zero-dimension shape is IronRDP's decoded form of an
                    // "invisible" server pointer (DecodedPointer::new_invisible); the
                    // C side rejects empty bitmaps, so translate it to a hide request
                    // rather than dropping it and leaving the cursor visible.
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

    /// Classic slow-path/fast-path bitmap updates (servers without EGFX/H.264)
    /// are decoded by IronRDP directly into `image`; forward the changed region
    /// to the native presenter the same way EGFX RemoteFX tiles are.
    /// `data_for_rect` returns a slice through the full image buffer, so the
    /// row stride is the image's own stride, not width * bytes-per-pixel.
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
