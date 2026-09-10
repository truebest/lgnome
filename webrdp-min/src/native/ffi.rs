//! Inbound C ABI edge: everything the C shell can see, matched 1:1 against
//! native/include/rdp_ffi.h (a change on either side must update the other,
//! plus tests and docs). All entry points are `unsafe extern "C"`: every one
//! dereferences the caller's session pointer, so the pointer contract below is
//! part of each signature.
//!
//! The media mailbox is deliberately separate from input. A short socket
//! timeout is its wake-up bound on rustls' blocking transport; camera/audio
//! producers never wait on TCP.

use std::ffi::{c_char, CStr};
use std::ptr;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::mpsc::{sync_channel, SyncSender, TrySendError};
use std::sync::Arc;
use std::thread::{self, JoinHandle};

use ironrdp_pdu::input::fast_path::SynchronizeFlags;

use super::abi::{RdpCallbacks, RdpConfig, RdpState};
use super::input::{ControlCommand, InputCommand, WorkerCommand};
use super::media_mailbox::MediaGate;
use super::sink::CallbackSink;
use super::{worker_main, NativeConfig};

const INPUT_QUEUE_DEPTH: usize = 1024;

pub struct RdpSession {
    stop: Arc<AtomicBool>,
    tx: SyncSender<WorkerCommand>,
    media: Arc<MediaGate>,
    desired_camera_available: Arc<AtomicBool>,
    worker: Option<JoinHandle<()>>,
}

/// The pointer contract shared by every entry point below: `session` must be
/// null or a pointer returned by `rdp_session_start` that has not yet been
/// passed to `rdp_session_stop`.
unsafe fn session_from<'a>(session: *mut RdpSession) -> Option<&'a RdpSession> {
    unsafe { session.as_ref() }
}

/// Bounded copy of a caller buffer. `data` must identify `len` readable bytes.
unsafe fn copy_bytes(data: *const u8, len: usize, max: usize) -> Option<Vec<u8>> {
    if data.is_null() || len == 0 || len > max {
        return None;
    }
    Some(unsafe { core::slice::from_raw_parts(data, len) }.to_vec())
}

unsafe fn cstr_lossy(ptr: *const c_char) -> String {
    if ptr.is_null() {
        return String::new();
    }
    unsafe { CStr::from_ptr(ptr) }
        .to_string_lossy()
        .into_owned()
}

unsafe fn copy_config(config: *const RdpConfig) -> Result<NativeConfig, &'static str> {
    let config = unsafe { config.as_ref() }.ok_or("missing config")?;
    let host = unsafe { cstr_lossy(config.host) };
    if host.is_empty() {
        return Err("missing host");
    }
    Ok(NativeConfig {
        host,
        port: if config.port == 0 { 3389 } else { config.port },
        username: unsafe { cstr_lossy(config.username) },
        password: unsafe { cstr_lossy(config.password) },
        domain: unsafe { cstr_lossy(config.domain) },
        width: config.width.max(1),
        height: config.height.max(1),
        fps: config.fps.max(1),
        prefer_pcm_audio: config.prefer_pcm_audio != 0,
        enable_camera: config.enable_camera != 0,
        enable_audio_input: config.enable_audio_input != 0,
        camera_width: config.camera_width.max(1),
        camera_height: config.camera_height.max(1),
        camera_fps: config.camera_fps.max(1),
    })
}

unsafe fn enqueue_command(session: *mut RdpSession, command: WorkerCommand) {
    let Some(session) = (unsafe { session_from(session) }) else {
        return;
    };
    match session.tx.try_send(command) {
        Ok(()) => {}
        // The receiver is gone (worker stopped); nothing to deliver to.
        Err(TrySendError::Disconnected(_)) => {}
        Err(TrySendError::Full(cmd)) => {
            // Pointer moves are idempotent — only the latest position matters — so dropping
            // one under backpressure is correct and avoids a stale backlog. Every other
            // event is state-changing: silently dropping a button-up / key-up would leave
            // the server with a stuck button (phantom drag) or an auto-repeating key, and a
            // dropped suppress-output toggle would blank or waste the wrong session. Fall
            // back to a blocking send for those; it is bounded — the worker either drains a
            // slot or drops the receiver on stop / write-timeout, which unblocks with Err.
            if !matches!(
                cmd,
                WorkerCommand::Input(InputCommand::PointerMove { .. }) | WorkerCommand::MediaReady
            ) {
                let _ = session.tx.send(cmd);
            }
        }
    }
}

unsafe fn enqueue(session: *mut RdpSession, input: InputCommand) {
    unsafe { enqueue_command(session, WorkerCommand::Input(input)) };
}

/// Start a native RDP session and return an opaque handle for the C shell.
///
/// # Safety
///
/// `config` must point to a valid `RdpConfig` whose string pointers, when non-null,
/// are valid NUL-terminated C strings for the duration of this call. `callbacks`, when
/// non-null, must point to a valid `RdpCallbacks` table. The callback function pointers
/// must remain callable until `rdp_session_stop` returns.
#[no_mangle]
pub unsafe extern "C" fn rdp_session_start(
    config: *const RdpConfig,
    callbacks: *const RdpCallbacks,
) -> *mut RdpSession {
    let callbacks = unsafe { callbacks.as_ref() }
        .copied()
        .map(CallbackSink::new)
        .unwrap_or_default();
    let config = match unsafe { copy_config(config) } {
        Ok(config) => config,
        Err(detail) => {
            callbacks.emit_state(RdpState::ProtocolError, detail);
            return ptr::null_mut();
        }
    };

    let (tx, rx) = sync_channel(INPUT_QUEUE_DEPTH);
    let stop = Arc::new(AtomicBool::new(false));
    let media = Arc::new(MediaGate::default());
    let desired_camera_available = Arc::new(AtomicBool::new(false));
    let worker_stop = Arc::clone(&stop);
    let worker_media = Arc::clone(&media);
    let worker_camera_available = Arc::clone(&desired_camera_available);
    let worker_callbacks = callbacks;
    let worker = match thread::Builder::new()
        .name("rdp-worker".to_owned())
        .spawn(move || {
            worker_main(
                config,
                worker_callbacks,
                rx,
                worker_stop,
                worker_media,
                worker_camera_available,
            )
        }) {
        Ok(worker) => worker,
        Err(e) => {
            callbacks.emit_state(RdpState::NetworkError, format!("spawn RDP worker: {e}"));
            return ptr::null_mut();
        }
    };

    Box::into_raw(Box::new(RdpSession {
        stop,
        tx,
        media,
        desired_camera_available,
        worker: Some(worker),
    }))
}

/// Stop and destroy a native RDP session created by `rdp_session_start`.
///
/// # Safety
///
/// `session` must be either null or a pointer returned by `rdp_session_start` that has
/// not already been passed to `rdp_session_stop`. After this call returns, the pointer is
/// invalid and no further native callbacks will be made by that session.
#[no_mangle]
pub unsafe extern "C" fn rdp_session_stop(session: *mut RdpSession) {
    if session.is_null() {
        return;
    }
    let mut session = unsafe { Box::from_raw(session) };
    session.stop.store(true, Ordering::SeqCst);
    let _ = session.tx.try_send(WorkerCommand::Stop);
    if let Some(worker) = session.worker.take() {
        let _ = worker.join();
    }
}

/// # Safety
/// `session` must be null or a live pointer from `rdp_session_start` (see `session_from`).
#[no_mangle]
pub unsafe extern "C" fn rdp_send_pointer_move(session: *mut RdpSession, x: u16, y: u16) {
    unsafe { enqueue(session, InputCommand::PointerMove { x, y }) };
}

/// # Safety
/// `session` must be null or a live pointer from `rdp_session_start` (see `session_from`).
#[no_mangle]
pub unsafe extern "C" fn rdp_send_pointer_button(
    session: *mut RdpSession,
    x: u16,
    y: u16,
    button: u8,
    down: bool,
) {
    unsafe { enqueue(session, InputCommand::PointerButton { x, y, button, down }) };
}

/// # Safety
/// `session` must be null or a live pointer from `rdp_session_start` (see `session_from`).
#[no_mangle]
pub unsafe extern "C" fn rdp_send_pointer_wheel(
    session: *mut RdpSession,
    x: u16,
    y: u16,
    delta: i16,
) {
    unsafe { enqueue(session, InputCommand::PointerWheel { x, y, delta }) };
}

/// # Safety
/// `session` must be null or a live pointer from `rdp_session_start` (see `session_from`).
#[no_mangle]
pub unsafe extern "C" fn rdp_send_key(
    session: *mut RdpSession,
    scancode: u8,
    down: bool,
    extended: bool,
) {
    unsafe {
        enqueue(
            session,
            InputCommand::Key {
                scancode,
                down,
                extended,
            },
        )
    };
}

/// # Safety
/// `session` must be null or a live pointer from `rdp_session_start` (see `session_from`).
#[no_mangle]
pub unsafe extern "C" fn rdp_send_unicode(session: *mut RdpSession, codepoint: u16, down: bool) {
    unsafe { enqueue(session, InputCommand::Unicode { codepoint, down }) };
}

/// Toggle server display updates for this session (TS_SUPPRESS_OUTPUT_PDU).
/// `allow_display=false` pauses graphics (rdpsnd audio keeps flowing on its DVC);
/// `true` resumes them. Note: gnome-remote-desktop resumes with a delta frame, not a
/// keyframe — pair with `rdp_request_refresh` when the local decoder lost its state.
/// # Safety
/// `session` must be null or a live pointer from `rdp_session_start` (see `session_from`).
#[no_mangle]
pub unsafe extern "C" fn rdp_set_suppress_output(session: *mut RdpSession, allow_display: bool) {
    unsafe {
        enqueue_command(
            session,
            WorkerCommand::Control(ControlCommand::SuppressOutput { allow_display }),
        )
    };
}

/// Ask the server for a fresh full frame / keyframe: re-submits the current monitor
/// layout on the Display Control DVC (forces gnome-remote-desktop to recreate its encode
/// session → RESET_GRAPHICS + IDR) and sends a full-screen Refresh Rect for servers that
/// honor the classic path.
/// # Safety
/// `session` must be null or a live pointer from `rdp_session_start` (see `session_from`).
#[no_mangle]
pub unsafe extern "C" fn rdp_request_refresh(session: *mut RdpSession) {
    unsafe {
        enqueue_command(
            session,
            WorkerCommand::Control(ControlCommand::RequestRefresh),
        )
    };
}

/// # Safety
/// `session` must be null or a live pointer from `rdp_session_start` (see `session_from`).
#[no_mangle]
pub unsafe extern "C" fn rdp_set_camera_available(session: *mut RdpSession, available: bool) {
    let Some(session) = (unsafe { session_from(session) }) else {
        return;
    };
    session
        .desired_camera_available
        .store(available, Ordering::Release);
    let _ = session.tx.try_send(WorkerCommand::MediaReady);
}

/// Open or close this session's outgoing camera/microphone mailbox. Closing is
/// synchronous with producers and the worker's payload writer: queued PCM is
/// discarded, a queued camera AU is reduced to a credit-safe fallback, and the
/// call waits for any already-authorized payload write.
///
/// # Safety
/// `session` must be null or a live pointer from `rdp_session_start` (see `session_from`).
#[no_mangle]
pub unsafe extern "C" fn rdp_set_capture_active(session: *mut RdpSession, active: bool) {
    let Some(session) = (unsafe { session_from(session) }) else {
        return;
    };
    let changed = match session.media.set_capture_active(active) {
        Ok(changed) => changed,
        Err(_) => return,
    };
    if changed {
        let _ = session.tx.try_send(WorkerCommand::MediaReady);
    }
}

/// Copy one H.264 Annex-B access unit into the bounded camera mailbox.
///
/// # Safety
/// `data` must identify `len` readable bytes for this call.
#[no_mangle]
pub unsafe extern "C" fn rdp_submit_camera_h264(
    session: *mut RdpSession,
    generation: u64,
    data: *const u8,
    len: usize,
    _is_keyframe: bool,
) -> bool {
    const MAX_CAMERA_AU: usize = 4 * 1024 * 1024;
    let Some(session) = (unsafe { session_from(session) }) else {
        return false;
    };
    let Some(bytes) = (unsafe { copy_bytes(data, len, MAX_CAMERA_AU) }) else {
        return false;
    };
    let Ok(accepted) = session.media.try_enqueue_camera_h264(generation, bytes) else {
        return false;
    };
    if !accepted {
        return false;
    }
    let _ = session.tx.try_send(WorkerCommand::MediaReady);
    true
}

/// # Safety
/// `session` must be null or a live pointer from `rdp_session_start` (see `session_from`).
#[no_mangle]
pub unsafe extern "C" fn rdp_submit_camera_error(
    session: *mut RdpSession,
    generation: u64,
) -> bool {
    let Some(session) = (unsafe { session_from(session) }) else {
        return false;
    };
    let Ok(accepted) = session.media.try_enqueue_camera_error(generation) else {
        return false;
    };
    if !accepted {
        return false;
    }
    let _ = session.tx.try_send(WorkerCommand::MediaReady);
    true
}

/// Copy one complete negotiated PCM packet into the bounded microphone mailbox.
///
/// # Safety
/// `data` must identify `len` readable bytes for this call.
#[no_mangle]
pub unsafe extern "C" fn rdp_submit_audio_input_pcm(
    session: *mut RdpSession,
    generation: u64,
    data: *const u8,
    len: usize,
) -> bool {
    const MAX_AUDIO_INPUT_PACKET: usize = 64 * 1024;
    let Some(session) = (unsafe { session_from(session) }) else {
        return false;
    };
    let Some(bytes) = (unsafe { copy_bytes(data, len, MAX_AUDIO_INPUT_PACKET) }) else {
        return false;
    };
    let Ok(accepted) = session.media.try_enqueue_audio_pcm(generation, bytes) else {
        return false;
    };
    if !accepted {
        return false;
    }
    let _ = session.tx.try_send(WorkerCommand::MediaReady);
    true
}

/// # Safety
/// `session` must be null or a live pointer from `rdp_session_start` (see `session_from`).
#[no_mangle]
pub unsafe extern "C" fn rdp_send_sync(
    session: *mut RdpSession,
    scroll_lock: bool,
    num_lock: bool,
    caps_lock: bool,
) {
    let mut flags = SynchronizeFlags::empty();
    if scroll_lock {
        flags |= SynchronizeFlags::SCROLL_LOCK;
    }
    if num_lock {
        flags |= SynchronizeFlags::NUM_LOCK;
    }
    if caps_lock {
        flags |= SynchronizeFlags::CAPS_LOCK;
    }
    unsafe {
        enqueue(
            session,
            InputCommand::SyncLocks {
                flags: flags.bits(),
            },
        )
    };
}

#[cfg(test)]
mod tests {
    use super::super::media_mailbox::CameraSubmission;
    use super::*;
    use std::sync::Mutex;

    #[test]
    fn null_config_reports_protocol_error() {
        extern "C" fn on_state(
            ctx: *mut core::ffi::c_void,
            state: RdpState,
            _reason: crate::native::RdpDisconnectReason,
            _detail: *const c_char,
        ) {
            let states = unsafe { &*(ctx.cast::<Mutex<Vec<RdpState>>>()) };
            states.lock().unwrap().push(state);
        }

        let states = Mutex::new(Vec::new());
        let callbacks = RdpCallbacks {
            ctx: (&states as *const Mutex<Vec<RdpState>>).cast_mut().cast(),
            on_state: Some(on_state),
            ..RdpCallbacks::default()
        };
        let session = unsafe { rdp_session_start(ptr::null(), &callbacks) };
        assert!(session.is_null());
        assert_eq!(*states.lock().unwrap(), vec![RdpState::ProtocolError]);
    }

    #[test]
    fn camera_availability_atomic_retains_latest_desired_state() {
        let (tx, _rx) = sync_channel(32);
        let mut session = RdpSession {
            stop: Arc::new(AtomicBool::new(false)),
            tx,
            media: Arc::new(MediaGate::default()),
            desired_camera_available: Arc::new(AtomicBool::new(false)),
            worker: None,
        };
        let session_ptr = &mut session as *mut RdpSession;

        unsafe { rdp_set_camera_available(session_ptr, true) };
        unsafe { rdp_set_camera_available(session_ptr, false) };
        unsafe { rdp_set_camera_available(session_ptr, true) };

        assert!(session.desired_camera_available.load(Ordering::Acquire));
    }

    #[test]
    fn capture_gate_purges_queued_payload_and_invalidates_taken_batches() {
        let (tx, _rx) = sync_channel(32);
        let media = Arc::new(MediaGate::default());
        let mut session = RdpSession {
            stop: Arc::new(AtomicBool::new(false)),
            tx,
            media: Arc::clone(&media),
            desired_camera_available: Arc::new(AtomicBool::new(false)),
            worker: None,
        };
        let session_ptr = &mut session as *mut RdpSession;
        let camera = [1u8, 2, 3];
        let audio = [4u8, 5, 6, 7];

        // Sessions start closed; neither media nor a producer-side camera error
        // may consume the credit until C publishes this session as foreground.
        assert!(!unsafe {
            rdp_submit_camera_h264(session_ptr, 7, camera.as_ptr(), camera.len(), true)
        });
        assert!(!unsafe {
            rdp_submit_audio_input_pcm(session_ptr, 8, audio.as_ptr(), audio.len())
        });
        assert!(!unsafe { rdp_submit_camera_error(session_ptr, 7) });

        unsafe { rdp_set_capture_active(session_ptr, true) };
        assert!(unsafe {
            rdp_submit_camera_h264(session_ptr, 7, camera.as_ptr(), camera.len(), true)
        });
        assert!(unsafe { rdp_submit_audio_input_pcm(session_ptr, 8, audio.as_ptr(), audio.len()) });
        unsafe { rdp_set_capture_active(session_ptr, false) };
        assert!(!media.capture_is_active().unwrap());
        let closed_batch = media.take_pending().unwrap();
        assert!(closed_batch.audio_input.is_empty());
        assert_eq!(
            closed_batch.camera,
            Some(CameraSubmission::DeferredError { generation: 7 })
        );
        assert!(!unsafe {
            rdp_submit_audio_input_pcm(session_ptr, 8, audio.as_ptr(), audio.len())
        });

        // Model the worker taking a batch immediately before a rapid
        // background -> foreground handoff. Its old epoch must not become valid
        // again: camera completes as SampleError and PCM is discarded.
        unsafe { rdp_set_capture_active(session_ptr, true) };
        assert!(unsafe {
            rdp_submit_camera_h264(session_ptr, 9, camera.as_ptr(), camera.len(), true)
        });
        assert!(unsafe {
            rdp_submit_audio_input_pcm(session_ptr, 10, audio.as_ptr(), audio.len())
        });
        let taken_batch = media.take_pending().unwrap();
        let taken_camera = taken_batch.camera.unwrap();
        let taken_audio = taken_batch.audio_input.into_iter().next().unwrap();
        unsafe { rdp_set_capture_active(session_ptr, false) };
        unsafe { rdp_set_capture_active(session_ptr, true) };
        let prepared = media.prepare_camera_send(taken_camera).unwrap().unwrap();
        assert!(matches!(
            prepared.submission,
            CameraSubmission::Error { generation: 9, .. }
        ));
        drop(prepared.permit);
        assert!(media.prepare_audio_send(taken_audio).unwrap().is_none());
        assert!(unsafe {
            rdp_submit_camera_h264(session_ptr, 11, camera.as_ptr(), camera.len(), true)
        });
    }
}
