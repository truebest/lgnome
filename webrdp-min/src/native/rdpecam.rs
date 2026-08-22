//! Minimal MS-RDPECAM client used by the native camera redirection path.
//!
//! gnome-remote-desktop currently consumes one H.264 capture stream.  Keep the
//! implementation intentionally narrow: one synthetic device, one stream, one
//! configured media type and server-driven sample credits.

#![forbid(unsafe_code)]

mod bridge;
mod device;
mod device_request;
mod enumerator;
mod wire;

use ironrdp_dvc::{DvcMessage, DvcProcessor};

use super::CallbackSink;
pub(super) use bridge::CameraBridge;
use bridge::{CameraLifecycle, DeviceInstance};
pub(super) use device::{CameraDevice, CameraDeviceFactory};
pub(super) use enumerator::CameraEnumerator;
use wire::*;

pub(super) const ENUMERATOR_CHANNEL_NAME: &str = "RDCamera_Device_Enumerator";
pub(super) const DEVICE_CHANNEL_NAME: &str = "GnomeCast_Camera_0";

const DEVICE_DISPLAY_NAME: &str = "GnomeCast Camera";
const MAX_PENDING_SAMPLE_REQUESTS: u8 = 8;

#[cfg(test)]
mod tests {
    use super::*;
    use ironrdp_core::encode_vec;
    use std::sync::atomic::{AtomicBool, AtomicUsize, Ordering};
    use std::sync::{Mutex as StdMutex, OnceLock};

    fn encoded(message: &DvcMessage) -> Vec<u8> {
        encode_vec(message.as_ref()).unwrap()
    }

    fn start_streams_request() -> Vec<u8> {
        let mut start = vec![2, MSG_START_STREAMS, 0];
        start.extend_from_slice(&media_type(640, 480, 15));
        start
    }

    fn ready_bridge(callbacks: CallbackSink) -> CameraBridge {
        let bridge = CameraBridge::new(callbacks, 640, 480, 15);
        assert!(bridge.set_available(true).is_none());
        let mut enumerator = CameraEnumerator::new(bridge.clone());
        assert_eq!(encoded(&enumerator.start(42).unwrap()[0]), [2, 3]);
        let added = enumerator.process(42, &[2, 4]).unwrap();
        assert_eq!(&encoded(&added[0])[..2], &[2, MSG_DEVICE_ADDED]);
        bridge
    }

    fn open_device(bridge: &CameraBridge, channel_id: u32) -> CameraDevice {
        let token = bridge.allocate_device_token().unwrap();
        let mut device = CameraDevice::new(bridge.clone(), token);
        assert!(device.start(channel_id).unwrap().is_empty());
        assert_eq!(
            bridge.inner.lock().unwrap().device_instance,
            Some(DeviceInstance { token, channel_id })
        );
        device
    }

    #[test]
    fn media_type_is_h264_and_exactly_26_bytes() {
        let media = media_type(640, 480, 15);
        assert_eq!(media.len(), 26);
        assert_eq!(media[0], 1);
        assert_eq!(u32::from_le_bytes(media[1..5].try_into().unwrap()), 640);
        assert_eq!(u32::from_le_bytes(media[5..9].try_into().unwrap()), 480);
        assert_eq!(u32::from_le_bytes(media[9..13].try_into().unwrap()), 15);
        assert_eq!(media[25], 1);
    }

    #[test]
    fn enumerator_advertises_after_version_and_availability() {
        let bridge = CameraBridge::new(CallbackSink::empty(), 640, 480, 15);
        let mut enumerator = CameraEnumerator::new(bridge.clone());
        let start = enumerator.start(42).unwrap();
        assert_eq!(encoded(&start[0]), [2, 3]);
        assert!(enumerator.process(42, &[2, 4]).unwrap().is_empty());
        let (_, added) = bridge.set_available(true).unwrap();
        let bytes = encoded(&added[0]);
        assert_eq!(&bytes[..2], &[2, 5]);
        assert!(bytes.ends_with(b"GnomeCast_Camera_0\0"));
    }

    #[test]
    fn device_negotiates_one_stream_and_credit_gates_samples() {
        static CREDITS: AtomicUsize = AtomicUsize::new(0);
        extern "C" fn on_sample_request(_ctx: *mut core::ffi::c_void, _generation: u64) {
            CREDITS.fetch_add(1, Ordering::SeqCst);
        }

        CREDITS.store(0, Ordering::SeqCst);
        let mut callbacks = CallbackSink::empty();
        callbacks.callbacks.on_camera_sample_request = Some(on_sample_request);
        let bridge = ready_bridge(callbacks);
        let device = open_device(&bridge, 7);
        assert_eq!(encoded(&device.process_request(7, &[2, 7])[0]), [2, 1]);
        assert_eq!(
            encoded(&device.process_request(7, &[2, 9])[0]),
            [2, 10, 1, 0, 1, 1, 1]
        );
        let start = start_streams_request();
        assert_eq!(encoded(&device.process_request(7, &start)[0]), [2, 1]);
        let generation = bridge.inner.lock().unwrap().generation;
        assert!(device.process_request(7, &[2, 17, 0]).is_empty());
        assert!(device.process_request(7, &[2, 17, 0]).is_empty());
        assert_eq!(CREDITS.load(Ordering::SeqCst), 1);
        let (channel, sample) = bridge
            .submit_h264(generation, vec![0, 0, 0, 1, 0x65])
            .unwrap();
        assert_eq!(channel, 7);
        assert_eq!(encoded(&sample[0]), [2, 18, 0, 0, 0, 0, 1, 0x65]);
        assert_eq!(CREDITS.load(Ordering::SeqCst), 1);
        bridge.notify_next_credit();
        assert_eq!(CREDITS.load(Ordering::SeqCst), 2);
        assert!(bridge.submit_h264(generation, vec![1]).is_some());
        bridge.notify_next_credit();
        assert!(bridge.submit_h264(generation, vec![1]).is_none());
    }

    #[test]
    fn stop_is_idempotent_and_completes_pending_samples_before_success() {
        static STOPS: AtomicUsize = AtomicUsize::new(0);
        extern "C" fn on_stop(_ctx: *mut core::ffi::c_void, _generation: u64) {
            STOPS.fetch_add(1, Ordering::SeqCst);
        }

        STOPS.store(0, Ordering::SeqCst);
        let mut callbacks = CallbackSink::empty();
        callbacks.callbacks.on_camera_stop = Some(on_stop);
        let bridge = ready_bridge(callbacks);
        let device = open_device(&bridge, 7);
        assert_eq!(
            encoded(&device.process_request(7, &[2, MSG_ACTIVATE_DEVICE])[0]),
            [2, MSG_SUCCESS]
        );
        assert_eq!(
            encoded(&device.process_request(7, &start_streams_request())[0]),
            [2, MSG_SUCCESS]
        );
        let first_generation = bridge.inner.lock().unwrap().generation;

        for _ in 0..3 {
            assert!(device
                .process_request(7, &[2, MSG_SAMPLE_REQUEST, 0])
                .is_empty());
        }
        let stop = device.process_request(7, &[2, MSG_STOP_STREAMS]);
        assert_eq!(stop.len(), 4);
        for response in &stop[..3] {
            assert_eq!(
                encoded(response),
                [2, MSG_SAMPLE_ERROR, 0, ERROR_UNEXPECTED as u8, 0, 0, 0,]
            );
        }
        assert_eq!(encoded(&stop[3]), [2, MSG_SUCCESS]);
        assert_eq!(STOPS.load(Ordering::SeqCst), 1);
        assert!(bridge.submit_h264(first_generation, vec![1]).is_none());
        {
            let state = bridge.inner.lock().unwrap();
            assert_eq!(state.lifecycle, CameraLifecycle::Activated);
            assert_eq!(state.pending_requests, 0);
            assert!(!state.credit_notified);
            assert_eq!(state.sample_requests_received, 3);
            assert_eq!(state.sample_errors_sent, 3);
        }

        let duplicate_stop = device.process_request(7, &[2, MSG_STOP_STREAMS]);
        assert_eq!(duplicate_stop.len(), 1);
        assert_eq!(encoded(&duplicate_stop[0]), [2, MSG_SUCCESS]);
        assert_eq!(STOPS.load(Ordering::SeqCst), 1);

        assert_eq!(
            encoded(&device.process_request(7, &start_streams_request())[0]),
            [2, MSG_SUCCESS]
        );
        let restarted_generation = bridge.inner.lock().unwrap().generation;
        assert_ne!(restarted_generation, first_generation);
        assert!(device
            .process_request(7, &[2, MSG_SAMPLE_REQUEST, 0])
            .is_empty());
        assert!(bridge
            .submit_h264(restarted_generation, vec![0, 0, 0, 1, 0x65])
            .is_some());
    }

    #[test]
    fn stop_while_deactivated_is_not_initialized() {
        let bridge = ready_bridge(CallbackSink::empty());
        let device = open_device(&bridge, 7);
        let response = device.process_request(7, &[2, MSG_STOP_STREAMS]);
        assert_eq!(response.len(), 1);
        assert_eq!(
            encoded(&response[0]),
            [2, MSG_ERROR, ERROR_NOT_INITIALIZED as u8, 0, 0, 0,]
        );
    }

    #[test]
    fn availability_before_negotiation_is_advertised_once_negotiated() {
        let bridge = CameraBridge::new(CallbackSink::empty(), 640, 480, 15);
        assert!(bridge.set_available(true).is_none());
        assert!(bridge.set_available(true).is_none());

        let mut enumerator = CameraEnumerator::new(bridge.clone());
        assert_eq!(encoded(&enumerator.start(42).unwrap()[0]), [2, 3]);
        let added = enumerator.process(42, &[2, 4]).unwrap();
        assert_eq!(added.len(), 1);
        assert_eq!(&encoded(&added[0])[..2], &[2, MSG_DEVICE_ADDED]);
        assert!(bridge.set_available(true).is_none());
    }

    #[test]
    fn final_deactivate_completes_samples_and_honors_activation_refcount() {
        static STOPS: AtomicUsize = AtomicUsize::new(0);
        extern "C" fn on_stop(_ctx: *mut core::ffi::c_void, _generation: u64) {
            STOPS.fetch_add(1, Ordering::SeqCst);
        }

        STOPS.store(0, Ordering::SeqCst);
        let mut callbacks = CallbackSink::empty();
        callbacks.callbacks.on_camera_stop = Some(on_stop);
        let bridge = ready_bridge(callbacks);
        let device = open_device(&bridge, 7);

        for _ in 0..2 {
            assert_eq!(
                encoded(&device.process_request(7, &[2, MSG_ACTIVATE_DEVICE])[0]),
                [2, MSG_SUCCESS]
            );
        }
        assert_eq!(
            encoded(&device.process_request(7, &start_streams_request())[0]),
            [2, MSG_SUCCESS]
        );
        for _ in 0..2 {
            assert!(device
                .process_request(7, &[2, MSG_SAMPLE_REQUEST, 0])
                .is_empty());
        }

        let first = device.process_request(7, &[2, MSG_DEACTIVATE_DEVICE]);
        assert_eq!(first.len(), 1);
        assert_eq!(encoded(&first[0]), [2, MSG_SUCCESS]);
        assert_eq!(STOPS.load(Ordering::SeqCst), 0);
        {
            let state = bridge.inner.lock().unwrap();
            assert_eq!(state.activation_refs, 1);
            assert_eq!(state.lifecycle, CameraLifecycle::Streaming);
            assert_eq!(state.pending_requests, 2);
        }

        let final_deactivate = device.process_request(7, &[2, MSG_DEACTIVATE_DEVICE]);
        assert_eq!(final_deactivate.len(), 3);
        for response in &final_deactivate[..2] {
            assert_eq!(
                encoded(response),
                [2, MSG_SAMPLE_ERROR, 0, ERROR_UNEXPECTED as u8, 0, 0, 0,]
            );
        }
        assert_eq!(encoded(&final_deactivate[2]), [2, MSG_SUCCESS]);
        assert_eq!(STOPS.load(Ordering::SeqCst), 1);
        {
            let state = bridge.inner.lock().unwrap();
            assert_eq!(state.activation_refs, 0);
            assert_eq!(state.lifecycle, CameraLifecycle::Deactivated);
            assert_eq!(state.pending_requests, 0);
            assert_eq!(state.sample_errors_sent, 2);
        }

        let extra = device.process_request(7, &[2, MSG_DEACTIVATE_DEVICE]);
        assert_eq!(
            encoded(&extra[0]),
            [2, MSG_ERROR, ERROR_NOT_INITIALIZED as u8, 0, 0, 0]
        );
    }

    #[test]
    fn device_removed_retires_stream_without_sample_errors() {
        static STOPS: AtomicUsize = AtomicUsize::new(0);
        extern "C" fn on_stop(_ctx: *mut core::ffi::c_void, _generation: u64) {
            STOPS.fetch_add(1, Ordering::SeqCst);
        }

        STOPS.store(0, Ordering::SeqCst);
        let mut callbacks = CallbackSink::empty();
        callbacks.callbacks.on_camera_stop = Some(on_stop);
        let bridge = ready_bridge(callbacks);
        let device = open_device(&bridge, 7);
        assert_eq!(
            encoded(&device.process_request(7, &[2, MSG_ACTIVATE_DEVICE])[0]),
            [2, MSG_SUCCESS]
        );
        assert_eq!(
            encoded(&device.process_request(7, &start_streams_request())[0]),
            [2, MSG_SUCCESS]
        );
        let generation = bridge.inner.lock().unwrap().generation;
        for _ in 0..3 {
            assert!(device
                .process_request(7, &[2, MSG_SAMPLE_REQUEST, 0])
                .is_empty());
        }

        let (enum_channel, removed) = bridge.set_available(false).unwrap();
        assert_eq!(enum_channel, 42);
        assert_eq!(removed.len(), 1);
        assert_eq!(&encoded(&removed[0])[..2], &[2, MSG_DEVICE_REMOVED]);
        assert_eq!(STOPS.load(Ordering::SeqCst), 1);
        assert!(bridge.submit_error(generation).is_none());
        assert!(device.process_request(7, &[2, MSG_STOP_STREAMS]).is_empty());
        {
            let state = bridge.inner.lock().unwrap();
            assert!(!state.advertised);
            assert_eq!(state.lifecycle, CameraLifecycle::Deactivated);
            assert_eq!(state.activation_refs, 0);
            assert_eq!(state.pending_requests, 0);
            assert_eq!(state.sample_errors_sent, 0);
            assert!(state.device_instance.is_none());
        }
        assert!(bridge.set_available(false).is_none());
    }

    #[test]
    fn stale_processor_cannot_touch_readded_device_with_reused_channel_id() {
        let bridge = ready_bridge(CallbackSink::empty());
        let mut old = open_device(&bridge, 7);
        assert_eq!(
            encoded(&old.process_request(7, &[2, MSG_ACTIVATE_DEVICE])[0]),
            [2, MSG_SUCCESS]
        );
        assert_eq!(
            encoded(&old.process_request(7, &start_streams_request())[0]),
            [2, MSG_SUCCESS]
        );

        assert!(bridge.set_available(false).is_some());
        assert!(bridge.set_available(true).is_some());
        let new = open_device(&bridge, 7);
        assert_eq!(
            encoded(&new.process_request(7, &[2, MSG_ACTIVATE_DEVICE])[0]),
            [2, MSG_SUCCESS]
        );
        assert_eq!(
            encoded(&new.process_request(7, &start_streams_request())[0]),
            [2, MSG_SUCCESS]
        );
        let new_instance = bridge.inner.lock().unwrap().device_instance.unwrap();

        assert!(old.process_request(7, &[2, MSG_STOP_STREAMS]).is_empty());
        old.close(7);
        {
            let state = bridge.inner.lock().unwrap();
            assert_eq!(state.device_instance, Some(new_instance));
            assert_eq!(state.lifecycle, CameraLifecycle::Streaming);
            assert_eq!(state.activation_refs, 1);
        }
        assert_eq!(
            encoded(&new.process_request(7, &[2, MSG_STOP_STREAMS])[0]),
            [2, MSG_SUCCESS]
        );
    }

    #[test]
    fn processor_created_before_device_added_is_retired() {
        let bridge = ready_bridge(CallbackSink::empty());
        assert!(bridge.set_available(false).is_some());

        let stale_token = bridge.allocate_device_token().unwrap();
        let mut stale = CameraDevice::new(bridge.clone(), stale_token);
        assert!(bridge.set_available(true).is_some());
        assert!(stale.start(7).unwrap().is_empty());
        assert!(bridge.inner.lock().unwrap().device_instance.is_none());
        assert!(stale
            .process_request(7, &[2, MSG_ACTIVATE_DEVICE])
            .is_empty());

        let current = open_device(&bridge, 7);
        assert_eq!(
            encoded(&current.process_request(7, &[2, MSG_ACTIVATE_DEVICE])[0]),
            [2, MSG_SUCCESS]
        );
    }

    static CALLBACK_BRIDGE: OnceLock<StdMutex<Option<CameraBridge>>> = OnceLock::new();
    static START_UNLOCKED: AtomicBool = AtomicBool::new(false);
    static STOP_UNLOCKED: AtomicBool = AtomicBool::new(false);
    static SAMPLE_UNLOCKED: AtomicBool = AtomicBool::new(false);

    fn record_callback_lock_state(result: &AtomicBool) {
        let bridge = CALLBACK_BRIDGE
            .get_or_init(|| StdMutex::new(None))
            .lock()
            .unwrap()
            .clone()
            .unwrap();
        let unlocked = bridge.inner.try_lock().is_ok();
        result.store(unlocked, Ordering::SeqCst);
    }

    extern "C" fn on_start_unlocked(
        _ctx: *mut core::ffi::c_void,
        _generation: u64,
        _width: u16,
        _height: u16,
        _fps: u16,
    ) {
        record_callback_lock_state(&START_UNLOCKED);
    }

    extern "C" fn on_stop_unlocked(_ctx: *mut core::ffi::c_void, _generation: u64) {
        record_callback_lock_state(&STOP_UNLOCKED);
    }

    extern "C" fn on_sample_unlocked(_ctx: *mut core::ffi::c_void, _generation: u64) {
        record_callback_lock_state(&SAMPLE_UNLOCKED);
    }

    #[test]
    fn capture_callbacks_run_outside_camera_state_lock() {
        START_UNLOCKED.store(false, Ordering::SeqCst);
        STOP_UNLOCKED.store(false, Ordering::SeqCst);
        SAMPLE_UNLOCKED.store(false, Ordering::SeqCst);
        let mut callbacks = CallbackSink::empty();
        callbacks.callbacks.on_camera_start = Some(on_start_unlocked);
        callbacks.callbacks.on_camera_stop = Some(on_stop_unlocked);
        callbacks.callbacks.on_camera_sample_request = Some(on_sample_unlocked);
        let bridge = ready_bridge(callbacks);
        *CALLBACK_BRIDGE
            .get_or_init(|| StdMutex::new(None))
            .lock()
            .unwrap() = Some(bridge.clone());
        let device = open_device(&bridge, 7);
        assert_eq!(
            encoded(&device.process_request(7, &[2, MSG_ACTIVATE_DEVICE])[0]),
            [2, MSG_SUCCESS]
        );
        assert_eq!(
            encoded(&device.process_request(7, &start_streams_request())[0]),
            [2, MSG_SUCCESS]
        );
        assert!(device
            .process_request(7, &[2, MSG_SAMPLE_REQUEST, 0])
            .is_empty());
        assert_eq!(
            encoded(&device.process_request(7, &[2, MSG_STOP_STREAMS])[1]),
            [2, MSG_SUCCESS]
        );
        assert!(START_UNLOCKED.load(Ordering::SeqCst));
        assert!(SAMPLE_UNLOCKED.load(Ordering::SeqCst));
        assert!(STOP_UNLOCKED.load(Ordering::SeqCst));
    }
}
