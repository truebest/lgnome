//! Native RDP worker: TCP/TLS/CredSSP transport and C callbacks.

mod abi;
mod active;
mod active_io;
mod connection;
mod disconnect;
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
    RdpCallbacks, RdpConfig, RdpDisconnectReason, RdpLogLevel, RdpState, RDP_AUDIO_CODEC_OPUS,
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
use transport::{is_timeout, monotonic_now, ts_request_len, TlsStream};

use std::io::{Read, Write};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::mpsc::{Receiver, TryRecvError};
use std::sync::{Arc, Mutex};
use std::thread;
use std::time::Duration;

use ironrdp_connector::connection_activation::ConnectionActivationFactory;
use ironrdp_connector::{ClientConnector, Sequence as _};
use ironrdp_core::MonotonicInstant;

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

const MAX_SESSION_ATTEMPTS: u32 = 3;

#[derive(Debug)]
struct NativeError {
    state: RdpState,
    reason: RdpDisconnectReason,
    retry_handoff: bool,
    message: String,
}

impl NativeError {
    fn with_state(state: RdpState, message: impl Into<String>) -> Self {
        Self {
            state,
            reason: RdpDisconnectReason::None,
            retry_handoff: false,
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
    // Buffered PDUs retain the time of the read that completed them.
    last_read_at: Option<MonotonicInstant>,
    // Retained factory for server-initiated reactivation.
    activation_factory: Option<ConnectionActivationFactory>,
    next_pts90k: u64,
    frame_pts_step: u64,
    // Discarded on initial activation; preserved across reactivation, including key releases.
    pending_input: Vec<InputCommand>,
    // Control commands buffered the same way (suppress-output toggles coalesce to the last
    // one; refresh requests dedupe to one).
    pending_control: Vec<ControlCommand>,
    // Reapply background suppression after reconnect; fresh connections allow graphics.
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
            last_read_at: None,
            activation_factory: None,
            next_pts90k: 0,
            frame_pts_step: 90_000 / fps,
            pending_input: Vec::new(),
            pending_control: Vec::new(),
            suppress_display_latched: false,
        }
    }

    fn run(&mut self) -> Result<(), NativeError> {
        // grd daemon handoffs disconnect normally and require a client reconnect.
        for attempt in 1..=MAX_SESSION_ATTEMPTS {
            match self.run_session() {
                Ok(()) => return Ok(()),
                Err(e) if e.should_retry(attempt, self.stop.load(Ordering::SeqCst)) => {
                    self.callbacks.log(
                        RdpLogLevel::Notice,
                        LOG_TARGET_SESSION,
                        format_args!(
                            "server closed the session (attempt {attempt}/{MAX_SESSION_ATTEMPTS}); reconnecting"
                        ),
                    );
                    self.callbacks
                        .emit_state(RdpState::Reconnecting, "reconnecting after server handoff");
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
        self.last_read_at = None;
        self.activation_factory = None;
        self.next_pts90k = 0;
        self.pending_input.clear();
        // Preserve the newest suppression state across retry; discard old-session refreshes.
        for control in self.pending_control.drain(..) {
            if let ControlCommand::SuppressOutput { allow_display } = control {
                self.suppress_display_latched = !allow_display;
            }
        }
        if let Ok(mut shared) = self.gfx.lock() {
            *shared = NativeGfxState::default();
        }
        // Worker-only reconnect bypasses C teardown, including its cursor reset.
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
                self.last_read_at = Some(monotonic_now());
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

    /// Buffer commands without blocking; return true on stop.
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
                    // Retain input edges during reactivation; coalesce only consecutive pointer moves.
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
                worker
                    .callbacks
                    .emit_state_reason(err.state, err.reason, err.message);
            }
        }
    }
    worker.callbacks.emit_state(RdpState::Stopped, "stopped");
}

#[cfg(test)]
mod tests {
    use super::*;
    use ironrdp_connector::ClientConnectorState;
    use ironrdp_core::{decode, encode_vec};
    use ironrdp_pdu::mcs::{McsMessage, SendDataIndication};
    use ironrdp_pdu::rdp::autodetect::{
        AutoDetectReqPdu, AutoDetectRequest, AutoDetectResponse, AutoDetectRspPdu,
    };
    use ironrdp_pdu::x224::X224;
    use std::collections::VecDeque;

    fn test_worker() -> (NativeWorker, std::sync::mpsc::SyncSender<WorkerCommand>) {
        let (tx, rx) = std::sync::mpsc::sync_channel::<WorkerCommand>(4);
        let callbacks = RdpCallbacks::default();
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
        let worker = NativeWorker::new(
            config,
            CallbackSink::new(callbacks),
            rx,
            Arc::new(AtomicBool::new(false)),
            Arc::new(MediaGate::default()),
            Arc::new(AtomicBool::new(false)),
        );
        (worker, tx)
    }

    #[test]
    fn early_authorization_preserves_access_denial_and_buffered_data() {
        for code in [0u32, 5, 42] {
            let (mut worker, _tx) = test_worker();
            let bytes = code.to_le_bytes();
            worker.inbuf.extend_from_slice(&bytes[..2]);
            let mut stream = std::io::Cursor::new([bytes[2], bytes[3], 0xaa]);
            let result = worker.read_early_user_auth_result(&mut stream);
            assert_eq!(worker.inbuf, [0xaa]);
            if code == 0 {
                result.unwrap();
            } else {
                let error = result.unwrap_err();
                assert_eq!(error.state, RdpState::ProtocolError);
                assert_eq!(
                    error.reason,
                    if code == 5 {
                        RdpDisconnectReason::AccessDenied
                    } else {
                        RdpDisconnectReason::None
                    }
                );
                assert!(!error.should_retry(1, false));
            }
        }
    }

    #[test]
    fn retry_folds_pending_suppress_into_latch() {
        let (mut worker, _tx) = test_worker();
        worker.inbuf.push(0);
        worker.last_read_at = Some(MonotonicInstant::from_millis(42));

        // A suppress queued during the failed session survives the retry as the latch;
        // the stale refresh drops (a fresh connection starts with an IDR anyway).
        worker.pending_control.push(ControlCommand::SuppressOutput {
            allow_display: false,
        });
        worker.pending_control.push(ControlCommand::RequestRefresh);
        worker.reset_session_state();
        assert!(worker.inbuf.is_empty());
        assert_eq!(worker.last_read_at, None);
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

    #[derive(Default)]
    struct AutodetectStream {
        reads: VecDeque<(Duration, Vec<u8>)>,
        writes: Vec<Vec<u8>>,
        write_delay: Duration,
    }

    impl Read for AutodetectStream {
        fn read(&mut self, buf: &mut [u8]) -> std::io::Result<usize> {
            let Some((delay, bytes)) = self.reads.pop_front() else {
                return Ok(0);
            };
            thread::sleep(delay);
            assert!(bytes.len() <= buf.len());
            buf[..bytes.len()].copy_from_slice(&bytes);
            Ok(bytes.len())
        }
    }

    impl Write for AutodetectStream {
        fn write(&mut self, bytes: &[u8]) -> std::io::Result<usize> {
            thread::sleep(self.write_delay);
            self.writes.push(bytes.to_vec());
            Ok(bytes.len())
        }

        fn flush(&mut self) -> std::io::Result<()> {
            Ok(())
        }
    }

    fn autodetect_frame(request: AutoDetectRequest) -> Vec<u8> {
        let user_data = encode_vec(&AutoDetectReqPdu::new(request)).unwrap();
        encode_vec(&X224(McsMessage::SendDataIndication(SendDataIndication {
            initiator_id: 1002,
            channel_id: 1004,
            user_data: user_data.into(),
        })))
        .unwrap()
    }

    fn run_bandwidth_probe(stream: &mut AutodetectStream) -> (u32, u32) {
        let (mut worker, _tx) = test_worker();
        let mut connector = worker.new_connector("127.0.0.1:12345".parse().unwrap());
        connector.state = ClientConnectorState::ConnectTimeAutoDetection {
            io_channel_id: 1003,
            user_channel_id: 1002,
        };
        connector.message_channel_id = Some(1004);
        let err = worker.pump_connector(stream, connector).unwrap_err();
        assert!(err.message.contains("peer closed connection"), "{err:?}");

        let response = stream.writes.last().expect("bandwidth reply");
        let X224(McsMessage::SendDataRequest(data)) = decode(response).unwrap() else {
            panic!("expected MCS SendDataRequest");
        };
        assert_eq!(data.channel_id, 1004);
        let response = decode::<AutoDetectRspPdu>(&data.user_data).unwrap();
        let AutoDetectResponse::BandwidthMeasureResults {
            sequence_number,
            time_delta_ms,
            byte_count,
            ..
        } = response.response
        else {
            panic!("expected BandwidthMeasureResults");
        };
        assert_eq!(sequence_number, 3);
        (time_delta_ms, byte_count)
    }

    #[test]
    fn connector_bandwidth_counts_payloads_and_times_separate_reads() {
        let start = autodetect_frame(AutoDetectRequest::bw_start_connect_time(1));
        let payload = autodetect_frame(AutoDetectRequest::bw_payload(2, vec![0; 1024]));
        let stop = autodetect_frame(AutoDetectRequest::bw_stop_connect_time(3, vec![0; 512]));
        let mut stream = AutodetectStream {
            // Split the payload across reads, with Stop buffered behind its final bytes.
            reads: VecDeque::from([
                (Duration::ZERO, [start, payload[..10].to_vec()].concat()),
                (
                    Duration::from_millis(20),
                    [payload[10..].to_vec(), stop].concat(),
                ),
            ]),
            ..Default::default()
        };
        let (elapsed_ms, bytes) = run_bandwidth_probe(&mut stream);
        assert!(
            elapsed_ms >= 20,
            "expected the read delay, got {elapsed_ms} ms"
        );
        assert_eq!(bytes, (1024 + 8) + (512 + 8));
    }

    #[test]
    fn connector_bandwidth_ignores_processing_delay_for_buffered_pdus() {
        let batch = [
            autodetect_frame(AutoDetectRequest::bw_start_connect_time(1)),
            autodetect_frame(AutoDetectRequest::rtt_connect_time(4)),
            autodetect_frame(AutoDetectRequest::bw_payload(2, vec![0; 1024])),
            autodetect_frame(AutoDetectRequest::bw_stop_connect_time(3, vec![0; 512])),
        ]
        .concat();
        let mut stream = AutodetectStream {
            reads: VecDeque::from([(Duration::ZERO, batch)]),
            // Sending the RTT reply delays processing Stop, but not its arrival.
            write_delay: Duration::from_millis(20),
            ..Default::default()
        };
        assert_eq!(
            run_bandwidth_probe(&mut stream),
            (1, (1024 + 8) + (512 + 8))
        );
        assert_eq!(stream.writes.len(), 2);
    }
}
