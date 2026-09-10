//! Outbound edge: the C callback table, wrapped so the worker and every DVC
//! handler can fire callbacks without touching raw function pointers.

use std::ffi::CString;
use std::fmt;

use ironrdp_graphics::pointer::DecodedPointer;

use super::egfx::NativeBitmapUnit;
use super::{RdpCallbacks, RdpDisconnectReason, RdpLogLevel, RdpState};

fn cstring_lossy(value: &str) -> CString {
    let bytes: Vec<u8> = value
        .as_bytes()
        .iter()
        .copied()
        .filter(|b| *b != 0)
        .collect();
    CString::new(bytes).expect("interior NUL bytes are removed")
}

#[derive(Clone, Copy, Default)]
pub(super) struct CallbackSink {
    pub(super) callbacks: RdpCallbacks,
}

// The C shell owns `ctx` and guarantees that it outlives the session. Callbacks are invoked
// synchronously from the worker thread, and byte/string pointers are valid only for the call.
unsafe impl Send for CallbackSink {}
unsafe impl Sync for CallbackSink {}

impl CallbackSink {
    pub(super) fn new(callbacks: RdpCallbacks) -> Self {
        Self { callbacks }
    }

    pub(super) fn emit_state(&self, state: RdpState, detail: impl AsRef<str>) {
        self.emit_state_reason(state, RdpDisconnectReason::None, detail);
    }

    pub(super) fn emit_state_reason(
        &self,
        state: RdpState,
        reason: RdpDisconnectReason,
        detail: impl AsRef<str>,
    ) {
        if let Some(cb) = self.callbacks.on_state {
            let detail = cstring_lossy(detail.as_ref());
            cb(self.callbacks.ctx, state, reason, detail.as_ptr());
        }
    }

    pub(super) fn log_enabled(&self, level: RdpLogLevel) -> bool {
        self.callbacks.on_log.is_some()
            && self
                .callbacks
                .on_log_enabled
                .is_none_or(|cb| cb(self.callbacks.ctx, level))
    }

    pub(super) fn emit_log(&self, level: RdpLogLevel, target: &str, line: &str) {
        if let Some(cb) = self.callbacks.on_log {
            let target = cstring_lossy(target);
            let line = cstring_lossy(line.as_ref());
            cb(self.callbacks.ctx, level, target.as_ptr(), line.as_ptr());
        }
    }

    pub(super) fn log(&self, level: RdpLogLevel, target: &str, arguments: fmt::Arguments<'_>) {
        if self.log_enabled(level) {
            self.emit_log(level, target, &arguments.to_string());
        }
    }

    pub(super) fn desktop_size(&self, width: u16, height: u16) {
        if let Some(cb) = self.callbacks.on_desktop_size {
            cb(self.callbacks.ctx, width, height);
        }
    }

    pub(super) fn video_au(&self, data: &[u8], pts90k: u64) {
        if let Some(cb) = self.callbacks.on_video_au {
            cb(self.callbacks.ctx, data.as_ptr(), data.len(), pts90k);
        }
    }
    pub(super) fn bitmap_update(&self, update: &NativeBitmapUnit) {
        if let Some(cb) = self.callbacks.on_bitmap_update {
            cb(
                self.callbacks.ctx,
                update.surface_id,
                update.left,
                update.top,
                update.width,
                update.height,
                update.stride,
                update.data.as_ptr(),
                update.data.len(),
            );
        }
    }

    pub(super) fn audio_format(&self, codec: u32, sample_rate: u32, channels: u16) {
        if let Some(cb) = self.callbacks.on_audio_format {
            cb(self.callbacks.ctx, codec, sample_rate, channels);
        }
    }

    pub(super) fn pointer_bitmap(&self, pointer: &DecodedPointer) {
        if let Some(cb) = self.callbacks.on_pointer_bitmap {
            cb(
                self.callbacks.ctx,
                pointer.width,
                pointer.height,
                pointer.hotspot_x,
                pointer.hotspot_y,
                pointer.bitmap_data.as_ptr(),
                pointer.bitmap_data.len(),
            );
        }
    }

    pub(super) fn pointer_position(&self, x: u16, y: u16) {
        if let Some(cb) = self.callbacks.on_pointer_position {
            cb(self.callbacks.ctx, x, y);
        }
    }

    pub(super) fn pointer_state(&self, state: u32) {
        if let Some(cb) = self.callbacks.on_pointer_state {
            cb(self.callbacks.ctx, state);
        }
    }

    pub(super) fn audio_data(&self, data: &[u8], ts_ms: u32) {
        if let Some(cb) = self.callbacks.on_audio_data {
            cb(self.callbacks.ctx, data.as_ptr(), data.len(), ts_ms);
        }
    }

    pub(super) fn camera_start(&self, generation: u64, width: u16, height: u16, fps: u16) {
        if let Some(cb) = self.callbacks.on_camera_start {
            cb(self.callbacks.ctx, generation, width, height, fps);
        }
    }

    pub(super) fn camera_stop(&self, generation: u64) {
        if let Some(cb) = self.callbacks.on_camera_stop {
            cb(self.callbacks.ctx, generation);
        }
    }

    pub(super) fn camera_sample_request(&self, generation: u64) {
        if let Some(cb) = self.callbacks.on_camera_sample_request {
            cb(self.callbacks.ctx, generation);
        }
    }

    pub(super) fn audio_input_start(
        &self,
        generation: u64,
        sample_rate: u32,
        channels: u16,
        frames_per_packet: u32,
    ) {
        if let Some(cb) = self.callbacks.on_audio_input_start {
            cb(
                self.callbacks.ctx,
                generation,
                sample_rate,
                channels,
                frames_per_packet,
            );
        }
    }

    pub(super) fn audio_input_stop(&self, generation: u64) {
        if let Some(cb) = self.callbacks.on_audio_input_stop {
            cb(self.callbacks.ctx, generation);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::super::{RDP_POINTER_STATE_DEFAULT, RDP_POINTER_STATE_HIDDEN};
    use super::*;

    #[test]
    fn state_sink_preserves_reason_in_the_same_event() {
        use std::ffi::CStr;
        use std::sync::Mutex;
        type Events = Mutex<Vec<(RdpState, RdpDisconnectReason, String)>>;
        extern "C" fn on_state(
            ctx: *mut core::ffi::c_void,
            state: RdpState,
            reason: RdpDisconnectReason,
            detail: *const std::ffi::c_char,
        ) {
            let events = unsafe { &*ctx.cast::<Events>() };
            let detail = unsafe { CStr::from_ptr(detail) }
                .to_string_lossy()
                .into_owned();
            events.lock().unwrap().push((state, reason, detail));
        }
        let events = Events::new(Vec::new());
        let mut callbacks = CallbackSink::default().callbacks;
        callbacks.ctx = (&events as *const Events).cast_mut().cast();
        callbacks.on_state = Some(on_state);
        let sink = CallbackSink::new(callbacks);
        sink.emit_state_reason(
            RdpState::Disconnected,
            RdpDisconnectReason::ServerReboot,
            "server restarting",
        );
        sink.emit_state(RdpState::Stopped, "stopped");
        assert_eq!(
            *events.lock().unwrap(),
            vec![
                (
                    RdpState::Disconnected,
                    RdpDisconnectReason::ServerReboot,
                    "server restarting".into()
                ),
                (
                    RdpState::Stopped,
                    RdpDisconnectReason::None,
                    "stopped".into()
                ),
            ]
        );
    }

    #[test]
    fn video_sink_forwards_raw_au_and_full_width_pts() {
        use std::sync::Mutex as StdMutex;

        extern "C" fn on_video_au(
            ctx: *mut core::ffi::c_void,
            data: *const u8,
            len: usize,
            pts90k: u64,
        ) {
            let seen = unsafe { &*(ctx.cast::<StdMutex<Option<(Vec<u8>, u64)>>>()) };
            let bytes = unsafe { core::slice::from_raw_parts(data, len) }.to_vec();
            *seen.lock().unwrap() = Some((bytes, pts90k));
        }

        let seen = StdMutex::new(None);
        let mut callbacks = CallbackSink::default().callbacks;
        callbacks.ctx = (&seen as *const StdMutex<Option<(Vec<u8>, u64)>>)
            .cast_mut()
            .cast();
        callbacks.on_video_au = Some(on_video_au);
        let sink = CallbackSink::new(callbacks);
        let au = [0x00, 0x00, 0x01, 0x65, 0x88];
        let pts90k = 0xfedc_ba98_7654_3210;

        sink.video_au(&au, pts90k);

        assert_eq!(*seen.lock().unwrap(), Some((au.to_vec(), pts90k)));
    }

    #[test]
    fn pointer_sink_forwards_shape_and_state() {
        use std::sync::Mutex as StdMutex;

        #[derive(Default)]
        struct Seen {
            bitmaps: Vec<(u16, u16, u16, u16, Vec<u8>)>,
            positions: Vec<(u16, u16)>,
            states: Vec<u32>,
        }

        extern "C" fn on_pointer_bitmap(
            ctx: *mut core::ffi::c_void,
            width: u16,
            height: u16,
            hotspot_x: u16,
            hotspot_y: u16,
            rgba: *const u8,
            len: usize,
        ) {
            let seen = unsafe { &*(ctx.cast::<StdMutex<Seen>>()) };
            let bytes = unsafe { core::slice::from_raw_parts(rgba, len) }.to_vec();
            seen.lock()
                .unwrap()
                .bitmaps
                .push((width, height, hotspot_x, hotspot_y, bytes));
        }
        extern "C" fn on_pointer_position(ctx: *mut core::ffi::c_void, x: u16, y: u16) {
            let seen = unsafe { &*(ctx.cast::<StdMutex<Seen>>()) };
            seen.lock().unwrap().positions.push((x, y));
        }
        extern "C" fn on_pointer_state(ctx: *mut core::ffi::c_void, state: u32) {
            let seen = unsafe { &*(ctx.cast::<StdMutex<Seen>>()) };
            seen.lock().unwrap().states.push(state);
        }

        let seen = StdMutex::new(Seen::default());
        let mut callbacks = CallbackSink::default().callbacks;
        callbacks.ctx = (&seen as *const StdMutex<Seen>).cast_mut().cast();
        callbacks.on_pointer_bitmap = Some(on_pointer_bitmap);
        callbacks.on_pointer_position = Some(on_pointer_position);
        callbacks.on_pointer_state = Some(on_pointer_state);
        let sink = CallbackSink::new(callbacks);

        let pointer = DecodedPointer {
            width: 2,
            height: 1,
            hotspot_x: 1,
            hotspot_y: 0,
            bitmap_data: vec![0x10, 0x20, 0x30, 0xff, 0x40, 0x50, 0x60, 0x80],
        };
        sink.pointer_bitmap(&pointer);
        sink.pointer_position(123, 456);
        sink.pointer_state(RDP_POINTER_STATE_HIDDEN);
        sink.pointer_state(RDP_POINTER_STATE_DEFAULT);

        let seen = seen.lock().unwrap();
        assert_eq!(
            seen.bitmaps,
            vec![(2, 1, 1, 0, pointer.bitmap_data.clone())]
        );
        assert_eq!(seen.positions, vec![(123, 456)]);
        assert_eq!(seen.states, vec![0, 1]);
    }
}
