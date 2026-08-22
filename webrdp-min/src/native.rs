//! Native C ABI and direct-TCP RDP driver for the webOS native app.
//!
//! The native target has no Web/RDCleanPath/browser fallback. It connects directly to
//! the RDP server over TCP, upgrades that socket to TLS, performs CredSSP, prefers
//! AVC420/H.264 EGFX for ss4s hardware decode, and also forwards native RemoteFX/bitmap
//! RGBA updates for servers that cannot provide H.264.

mod abi;
mod active;
mod active_io;
mod connection;
mod dvc;
mod egfx;
mod ffi;
mod input;
mod logging;
mod media_mailbox;
mod rdpeai;
mod rdpecam;
mod rdpedisp;
mod rdpsnd;
mod sink;
mod transport;

pub use abi::{
    RdpCallbacks, RdpConfig, RdpLogLevel, RdpState, RDP_AUDIO_CODEC_OPUS,
    RDP_AUDIO_CODEC_PCM_S16LE, RDP_POINTER_STATE_DEFAULT, RDP_POINTER_STATE_HIDDEN,
};
use egfx::NativeGfxState;
pub use ffi::RdpSession;
use input::{ControlCommand, InputCommand, WorkerCommand};
pub(crate) use logging::CallbackLogLayer;
use logging::{
    CallbackSinkGuard, LOG_TARGET_AUDIO, LOG_TARGET_AUDIO_INPUT, LOG_TARGET_CAMERA,
    LOG_TARGET_GRAPHICS, LOG_TARGET_SESSION,
};
use media_mailbox::MediaGate;
use sink::CallbackSink;
use transport::{is_timeout, ts_request_len, TlsStream};

use std::io::{Read, Write};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::mpsc::{Receiver, TryRecvError};
use std::sync::{Arc, Mutex};
use std::thread;
use std::time::Duration;

use ironrdp_connector::connection_activation::ConnectionActivationFactory;
use ironrdp_connector::{ClientConnector, Sequence as _};

// The media mailbox is deliberately separate from input. A short socket timeout is its
// wake-up bound on rustls' blocking transport; camera/audio producers never wait on TCP.

#[derive(Clone)]
struct NativeConfig {
    host: String,
    port: u16,
    username: String,
    password: String,
    domain: String,
    width: u16,
    height: u16,
    fps: u16,
    prefer_pcm_audio: bool,
    enable_camera: bool,
    enable_audio_input: bool,
    camera_width: u16,
    camera_height: u16,
    camera_fps: u16,
}

#[derive(Debug)]
struct NativeError {
    state: RdpState,
    message: String,
}

impl NativeError {
    fn with_state(state: RdpState, message: impl Into<String>) -> Self {
        Self {
            state,
            message: message.into(),
        }
    }

    fn network(message: impl Into<String>) -> Self {
        Self::with_state(RdpState::NetworkError, message)
    }

    fn protocol(message: impl Into<String>) -> Self {
        Self::with_state(RdpState::ProtocolError, message)
    }

    fn no_avc420(message: impl Into<String>) -> Self {
        Self::with_state(RdpState::NoAvc420, message)
    }
}

struct NativeWorker {
    config: NativeConfig,
    callbacks: CallbackSink,
    rx: Receiver<WorkerCommand>,
    stop: Arc<AtomicBool>,
    media: Arc<MediaGate>,
    desired_camera_available: Arc<AtomicBool>,
    camera: rdpecam::CameraBridge,
    audio_input: rdpeai::AudioInputBridge,
    gfx: Arc<Mutex<NativeGfxState>>,
    inbuf: Vec<u8>,
    // Retained from the connect result to drive the Deactivation-Reactivation Sequence
    // locally: produces a fresh `ConnectionActivationSequence` on each Server Deactivate
    // All PDU (the x224 processor no longer owns one; see ConnectionResult::activation_factory).
    activation_factory: Option<ConnectionActivationFactory>,
    next_pts90k: u64,
    frame_pts_step: u64,
    // Input events drained off the channel by poll_stop while the worker is busy (notably
    // mid-session Deactivate-Reactivate) and not yet dispatched. run_active clears this on
    // entry so pre-connect events are discarded, but events buffered during reactivation
    // survive to the next drain_input rather than being lost (a dropped release would stick).
    pending_input: Vec<InputCommand>,
    // Control commands buffered the same way (suppress-output toggles coalesce to the last
    // one; refresh requests dedupe to one).
    pending_control: Vec<ControlCommand>,
    // Last suppress-output state the client commanded. Every fresh RDP connection starts
    // server-side with display updates ALLOWED, so the silent in-worker reconnect (run's
    // ultimatum retry) must re-assert a commanded suppression or a backgrounded session
    // would silently resume streaming full-rate video nobody displays.
    suppress_display_latched: bool,
}

impl NativeWorker {
    fn new(
        config: NativeConfig,
        callbacks: CallbackSink,
        rx: Receiver<WorkerCommand>,
        stop: Arc<AtomicBool>,
        media: Arc<MediaGate>,
        desired_camera_available: Arc<AtomicBool>,
    ) -> Self {
        let fps = u64::from(config.fps.max(1));
        let camera = rdpecam::CameraBridge::new(
            callbacks,
            config.camera_width,
            config.camera_height,
            config.camera_fps,
        );
        let audio_input = rdpeai::AudioInputBridge::new(callbacks);
        Self {
            config,
            callbacks,
            rx,
            stop,
            media,
            desired_camera_available,
            camera,
            audio_input,
            gfx: Arc::new(Mutex::new(NativeGfxState::default())),
            inbuf: Vec::new(),
            activation_factory: None,
            next_pts90k: 0,
            frame_pts_step: 90_000 / fps,
            pending_input: Vec::new(),
            pending_control: Vec::new(),
            suppress_display_latched: false,
        }
    }

    fn run(&mut self) -> Result<(), NativeError> {
        // gnome-remote-desktop closes the connection with an MCS Disconnect Provider
        // Ultimatum (preceded by ServerSetErrorInfo(RpcInitiatedDisconnect)) as a NORMAL
        // part of e.g. handing a session over between its daemons, and expects the
        // client to reconnect — mstsc/FreeRDP do so automatically. Retry a few times
        // before surfacing the failure.
        const MAX_SESSION_ATTEMPTS: u32 = 3;
        for attempt in 1..=MAX_SESSION_ATTEMPTS {
            match self.run_session() {
                Ok(()) => return Ok(()),
                Err(e)
                    if attempt < MAX_SESSION_ATTEMPTS
                        && !self.stop.load(Ordering::SeqCst)
                        && e.message.contains("disconnect provider ultimatum") =>
                {
                    self.callbacks.log(
                        RdpLogLevel::Notice,
                        LOG_TARGET_SESSION,
                        format_args!(
                            "server closed the session (attempt {attempt}/{MAX_SESSION_ATTEMPTS}); reconnecting"
                        ),
                    );
                    self.reset_session_state();
                    thread::sleep(Duration::from_millis(1000));
                }
                Err(e) => return Err(e),
            }
        }
        unreachable!("loop either returns or retries")
    }

    /// Clears per-session accumulated state so a reconnect starts clean.
    fn reset_session_state(&mut self) {
        self.inbuf.clear();
        self.activation_factory = None;
        self.next_pts90k = 0;
        self.pending_input.clear();
        // A suppress/resume queued while the failed session was still handshaking is the
        // newest commanded state and must survive the retry: fold it into the latch
        // (run_active re-asserts a latched suppression on the fresh connection; a fresh
        // connection already starts with display allowed for the resume case). Queued
        // refreshes belong to the OLD encode session — reconnecting yields a new IDR
        // anyway — so those simply drop.
        for control in self.pending_control.drain(..) {
            if let ControlCommand::SuppressOutput { allow_display } = control {
                self.suppress_display_latched = !allow_display;
            }
        }
        if let Ok(mut shared) = self.gfx.lock() {
            *shared = NativeGfxState::default();
        }
        // A silent in-worker reconnect bypasses the C session-teardown path that restores
        // the default cursor, so a pointer the old session left hidden or custom-shaped would
        // leak into the new one until the server next changes it. Reset it to default+visible.
        self.callbacks.pointer_state(RDP_POINTER_STATE_DEFAULT);
    }

    fn run_session(&mut self) -> Result<(), NativeError> {
        self.callbacks.emit_state(
            RdpState::Connecting,
            format!("connecting to {}:{}", self.config.host, self.config.port),
        );

        let mut tcp = match self.connect_tcp()? {
            Some(tcp) => tcp,
            None => return Ok(()),
        };
        let client_addr = tcp
            .local_addr()
            .map_err(|e| NativeError::network(format!("local address: {e}")))?;
        let mut connector = self.new_connector(client_addr);

        self.send_x224_request(&mut tcp, &mut connector)?;
        if self.drain_commands() {
            return Ok(());
        }

        self.callbacks
            .emit_state(RdpState::Tls, "starting TLS security upgrade");
        let (mut tls, public_key) = self.upgrade_tls(tcp)?;
        connector.mark_security_upgrade_as_done();

        if !connector.should_perform_credssp() {
            return Err(NativeError::protocol("server did not select CredSSP/NLA"));
        }
        self.callbacks
            .emit_state(RdpState::Credssp, "performing CredSSP/NLA");
        self.run_credssp(&mut tls, &mut connector, public_key)?;

        let result = self.pump_connector(&mut tls, connector)?;
        self.run_active(tls, result)
    }

    fn read_connector_pdu<T: Read>(
        &mut self,
        stream: &mut T,
        connector: &ClientConnector,
        label: &str,
    ) -> Result<Vec<u8>, NativeError> {
        let hint = connector
            .next_pdu_hint()
            .ok_or_else(|| NativeError::protocol(format!("{label}: connector has no PDU hint")))?;
        self.read_hinted_pdu(stream, hint, label)
    }

    fn read_activation_pdu(
        &mut self,
        stream: &mut TlsStream,
        seq: &ironrdp_connector::connection_activation::ConnectionActivationSequence,
    ) -> Result<Vec<u8>, NativeError> {
        let hint = seq
            .next_pdu_hint()
            .ok_or_else(|| NativeError::protocol("reactivation has no PDU hint"))?;
        self.read_hinted_pdu(stream, hint, "reactivation input")
    }

    fn read_hinted_pdu<T: Read>(
        &mut self,
        stream: &mut T,
        hint: &dyn ironrdp_pdu::PduHint,
        label: &str,
    ) -> Result<Vec<u8>, NativeError> {
        loop {
            if self.drain_commands() {
                return Err(NativeError::network(format!("{label}: stopped")));
            }
            match hint
                .find_size(&self.inbuf)
                .map_err(|e| NativeError::protocol(format!("{label}: PDU size: {e}")))?
            {
                Some((_matched, size)) if self.inbuf.len() >= size => {
                    return Ok(self.inbuf.drain(..size).collect())
                }
                _ => self.read_more(stream, label)?,
            }
        }
    }

    fn read_ts_request(&mut self, stream: &mut TlsStream) -> Result<Vec<u8>, NativeError> {
        loop {
            if self.drain_commands() {
                return Err(NativeError::network("CredSSP stopped"));
            }
            if let Some(total) = ts_request_len(&self.inbuf) {
                if self.inbuf.len() >= total {
                    return Ok(self.inbuf.drain(..total).collect());
                }
            }
            self.read_more(stream, "CredSSP TSRequest")?;
        }
    }

    fn read_more<T: Read>(&mut self, stream: &mut T, label: &str) -> Result<(), NativeError> {
        let mut buf = [0u8; 8192];
        match stream.read(&mut buf) {
            Ok(0) => Err(NativeError::network(format!(
                "{label}: peer closed connection"
            ))),
            Ok(n) => {
                self.inbuf.extend_from_slice(&buf[..n]);
                Ok(())
            }
            Err(e) if is_timeout(&e) => Ok(()),
            Err(e) => Err(NativeError::network(format!("{label}: read: {e}"))),
        }
    }

    fn write_all<T: Write>(
        &mut self,
        stream: &mut T,
        bytes: &[u8],
        label: &str,
    ) -> Result<(), NativeError> {
        if bytes.is_empty() {
            return Ok(());
        }
        stream
            .write_all(bytes)
            .map_err(|e| NativeError::network(format!("{label}: write: {e}")))?;
        stream
            .flush()
            .map_err(|e| NativeError::network(format!("{label}: flush: {e}")))
    }

    /// Drains the command channel without blocking: buffers input (coalescing
    /// consecutive pointer moves) and control commands for the next dispatch,
    /// and returns true when the worker must stop. Called from every read loop
    /// so a busy or reconnecting worker never leaves the C side blocked on a
    /// full channel.
    fn drain_commands(&mut self) -> bool {
        if self.stop.load(Ordering::SeqCst) {
            return true;
        }
        loop {
            match self.rx.try_recv() {
                Ok(WorkerCommand::Stop) => {
                    self.stop.store(true, Ordering::SeqCst);
                    return true;
                }
                Ok(WorkerCommand::Input(input)) => {
                    // Draining keeps the SDL thread from blocking on a full channel while the
                    // worker is busy. Buffer the events instead of dropping them: pre-connect
                    // this buffer is cleared when run_active starts, but during an in-session
                    // Deactivate-Reactivate ActiveStage still exists and a dropped button/key
                    // release would stick — the next drain_input replays what is buffered here.
                    // Consecutive pointer moves coalesce (idempotent) so a fast pointer cannot
                    // grow the buffer without bound.
                    match (self.pending_input.last_mut(), &input) {
                        (
                            Some(InputCommand::PointerMove { x, y }),
                            InputCommand::PointerMove { x: nx, y: ny },
                        ) => {
                            *x = *nx;
                            *y = *ny;
                        }
                        _ => self.pending_input.push(input),
                    }
                }
                Ok(WorkerCommand::Control(control)) => {
                    // Only the final suppress-output state matters, and one queued refresh
                    // is as good as many; keep the buffer minimal.
                    match control {
                        ControlCommand::SuppressOutput { .. } => {
                            self.pending_control
                                .retain(|c| !matches!(c, ControlCommand::SuppressOutput { .. }));
                            self.pending_control.push(control);
                        }
                        ControlCommand::RequestRefresh => {
                            if !self
                                .pending_control
                                .contains(&ControlCommand::RequestRefresh)
                            {
                                self.pending_control.push(control);
                            }
                        }
                    }
                }
                Ok(WorkerCommand::MediaReady) => {}
                Err(TryRecvError::Empty) => return false,
                Err(TryRecvError::Disconnected) => {
                    self.stop.store(true, Ordering::SeqCst);
                    return true;
                }
            }
        }
    }
}

fn worker_main(
    config: NativeConfig,
    callbacks: CallbackSink,
    rx: Receiver<WorkerCommand>,
    stop: Arc<AtomicBool>,
    media: Arc<MediaGate>,
    desired_camera_available: Arc<AtomicBool>,
) {
    crate::init_logging();
    let _callback_sink_guard = CallbackSinkGuard::enter(callbacks);
    let mut worker =
        NativeWorker::new(config, callbacks, rx, stop, media, desired_camera_available);
    match worker.run() {
        Ok(()) => {}
        Err(err) => {
            if !worker.stop.load(Ordering::SeqCst) {
                worker.callbacks.emit_state(err.state, err.message);
            }
        }
    }
    worker.callbacks.emit_state(RdpState::Stopped, "stopped");
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn retry_folds_pending_suppress_into_latch() {
        let (_tx, rx) = std::sync::mpsc::sync_channel::<WorkerCommand>(4);
        let callbacks = RdpCallbacks {
            ctx: core::ptr::null_mut(),
            on_state: None,
            on_log_enabled: None,
            on_log: None,
            on_desktop_size: None,
            on_video_au: None,
            on_bitmap_update: None,
            on_audio_format: None,
            on_audio_data: None,
            on_pointer_bitmap: None,
            on_pointer_position: None,
            on_pointer_state: None,
            on_camera_start: None,
            on_camera_stop: None,
            on_camera_sample_request: None,
            on_audio_input_start: None,
            on_audio_input_stop: None,
        };
        let config = NativeConfig {
            host: "test".to_owned(),
            port: 3389,
            username: String::new(),
            password: String::new(),
            domain: String::new(),
            width: 1,
            height: 1,
            fps: 30,
            prefer_pcm_audio: false,
            enable_camera: false,
            enable_audio_input: false,
            camera_width: 640,
            camera_height: 480,
            camera_fps: 15,
        };
        let mut worker = NativeWorker::new(
            config,
            CallbackSink::new(callbacks),
            rx,
            Arc::new(AtomicBool::new(false)),
            Arc::new(MediaGate::default()),
            Arc::new(AtomicBool::new(false)),
        );

        // A suppress queued during the failed session survives the retry as the latch;
        // the stale refresh drops (a fresh connection starts with an IDR anyway).
        worker.pending_control.push(ControlCommand::SuppressOutput {
            allow_display: false,
        });
        worker.pending_control.push(ControlCommand::RequestRefresh);
        worker.reset_session_state();
        assert!(worker.suppress_display_latched);
        assert!(worker.pending_control.is_empty());

        // A queued resume is newer intent and must clear a stale latched suppression.
        worker.pending_control.push(ControlCommand::SuppressOutput {
            allow_display: true,
        });
        worker.reset_session_state();
        assert!(!worker.suppress_display_latched);
        assert!(worker.pending_control.is_empty());
    }
}
