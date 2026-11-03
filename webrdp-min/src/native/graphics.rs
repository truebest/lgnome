//! Serial graphics callbacks, bounded across all sessions, independent of network dispatch.

use std::collections::VecDeque;
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::{Arc, Condvar, Mutex};
use std::thread::{self, JoinHandle};
use std::time::Duration;

use super::egfx::{NativeBitmapUnit, NativeVideoUnit};
use super::{CallbackSink, CallbackSinkGuard, RdpState};

const MAX_DATA_EVENTS: usize = 32;
const MAX_EVENTS: usize = 256;
const MAX_BYTES: usize = 64 * 1024 * 1024;
pub(super) const MAX_AU_BYTES: usize = 32 * 1024 * 1024;
static GRAPHICS_BYTES: AtomicUsize = AtomicUsize::new(0);

pub(super) enum Event {
    Video(NativeVideoUnit, u64),
    Bitmap(NativeBitmapUnit),
    Desktop(u16, u16),
    Refresh(u32),
    State(RdpState, String),
    PointerPosition(u16, u16),
    Frame,
}

impl Event {
    fn bytes(&self) -> usize {
        match self {
            Self::Video(unit, _) => unit.data.len(),
            Self::Bitmap(unit) => unit.data.len(),
            _ => 0,
        }
    }
    fn dispatch(&self, sink: CallbackSink) {
        match self {
            Self::Video(unit, pts) => sink.video_au(unit.codec, unit.flags, &unit.data, *pts),
            Self::Bitmap(unit) => sink.bitmap_update(unit),
            Self::Desktop(w, h) => sink.desktop_size(*w, *h),
            Self::Refresh(cookie) => sink.refresh_started(*cookie),
            Self::State(state, detail) => sink.emit_state(*state, detail),
            Self::PointerPosition(x, y) => sink.pointer_position(*x, *y),
            Self::Frame => {}
        }
    }
}

struct Pending {
    event: Event,
    generation: u64,
    sequence: u64,
}
impl Drop for Pending {
    fn drop(&mut self) {
        GRAPHICS_BYTES.fetch_sub(self.event.bytes(), Ordering::AcqRel);
    }
}

#[derive(Default)]
struct State {
    pending: VecDeque<Pending>,
    generation: u64,
    submitted: u64,
    completed: u64,
    cancelled: u64,
    data_events: usize,
    running: bool,
    stopped: bool,
    error: Option<&'static str>,
    next_pts90k: u64,
    pts_step: u64,
}

#[derive(Default)]
pub(super) struct GraphicsQueue {
    state: Mutex<State>,
    changed: Condvar,
}

impl GraphicsQueue {
    pub(super) fn configure_pts(&self, fps: u16) {
        self.state.lock().unwrap().pts_step = (90_000 / u64::from(fps.max(1))).max(1);
    }

    pub(super) fn submit_video(&self, codec: u32, flags: u32, data: &[u8]) {
        if data.len() > MAX_AU_BYTES {
            self.state.lock().unwrap().error = Some("graphics AU exceeds 32 MiB");
            self.changed.notify_all();
            return;
        }
        self.submit(Event::Video(
            NativeVideoUnit {
                codec,
                flags,
                data: data.to_vec(),
            },
            0,
        ));
    }

    pub(super) fn submit(&self, event: Event) -> Option<(u64, u64)> {
        let mut state = self.state.lock().unwrap();
        if state.stopped || state.error.is_some() {
            return None;
        }
        let bytes = event.bytes();
        if state.pending.len() + usize::from(state.running) >= MAX_EVENTS
            || (bytes > 0 && state.data_events >= MAX_DATA_EVENTS)
            || matches!(&event, Event::Video(_, _) if bytes > MAX_AU_BYTES)
            || GRAPHICS_BYTES
                .fetch_update(Ordering::AcqRel, Ordering::Acquire, |used| {
                    used.checked_add(bytes).filter(|total| *total <= MAX_BYTES)
                })
                .is_err()
        {
            state.error =
                Some("graphics queue overflow (32 data events/session, 64 MiB/application)");
            self.changed.notify_all();
            return None;
        }
        state.submitted += 1;
        let generation = state.generation;
        let sequence = state.submitted;
        if bytes > 0 {
            state.data_events += 1;
        }
        state.pending.push_back(Pending {
            event,
            generation,
            sequence,
        });
        self.changed.notify_all();
        Some((generation, sequence))
    }

    pub(super) fn reset(&self, connection: bool) {
        let mut state = self.state.lock().unwrap();
        let cancelled_data = state.pending.iter().filter(|p| p.event.bytes() > 0).count();
        state.data_events -= cancelled_data;
        state.generation += 1;
        let generation = state.generation;
        state.pending.retain_mut(|pending| {
            if matches!(pending.event, Event::State(..) | Event::Refresh(..)) {
                pending.generation = generation;
                true
            } else {
                false
            }
        });
        state.cancelled = state.submitted;
        if !state.running && state.pending.is_empty() {
            state.completed = state.submitted;
        }
        if connection {
            state.next_pts90k = 0;
        }
        self.changed.notify_all();
    }

    pub(super) fn progress(&self) -> (u64, u64) {
        let state = self.state.lock().unwrap();
        (state.generation, state.completed)
    }

    pub(super) fn error(&self) -> Option<&'static str> {
        self.state.lock().unwrap().error
    }

    pub(super) fn barrier(&self, timeout: Duration) -> bool {
        let state = self.state.lock().unwrap();
        let target = state.submitted;
        let (state, _) = self
            .changed
            .wait_timeout_while(state, timeout, |state| state.completed < target)
            .unwrap();
        state.completed >= target
    }

    pub(super) fn cancel(&self) {
        let mut state = self.state.lock().unwrap();
        state.stopped = true;
        state.data_events -= state.pending.iter().filter(|p| p.event.bytes() > 0).count();
        state.pending.clear();
        state.cancelled = state.submitted;
        if !state.running {
            state.completed = state.submitted;
        }
        self.changed.notify_all();
    }

    pub(super) fn start(self: &Arc<Self>, sink: CallbackSink) -> std::io::Result<GraphicsExecutor> {
        let queue = Arc::clone(self);
        let worker = thread::Builder::new()
            .name("rdp-graphics".into())
            .spawn(move || {
                let _log = CallbackSinkGuard::enter(sink);
                loop {
                    let pending = {
                        let mut state = queue.state.lock().unwrap();
                        while state.pending.is_empty() && !state.stopped {
                            state = queue.changed.wait(state).unwrap();
                        }
                        if state.stopped {
                            break;
                        }
                        state.running = true;
                        let mut pending = state.pending.pop_front().unwrap();
                        if let Event::Video(unit, pts) = &mut pending.event {
                            *pts = state.next_pts90k;
                            if unit.flags & super::abi::RDP_VIDEO_FLAG_AVC444_AUX == 0 {
                                state.next_pts90k =
                                    state.next_pts90k.wrapping_add(state.pts_step.max(1));
                            }
                        }
                        pending
                    };
                    // Reset cancels queued events; an already started callback finishes before the new generation.
                    pending.event.dispatch(sink);
                    let mut state = queue.state.lock().unwrap();
                    state.running = false;
                    state.completed = if state.pending.is_empty() {
                        pending.sequence.max(state.cancelled)
                    } else {
                        pending.sequence
                    };
                    if pending.event.bytes() > 0 {
                        state.data_events -= 1;
                    }
                    drop(pending);
                    queue.changed.notify_all();
                }
            })?;
        Ok(GraphicsExecutor {
            queue: Arc::clone(self),
            worker: Some(worker),
        })
    }

    #[cfg(test)]
    pub(super) fn videos(&self) -> Vec<NativeVideoUnit> {
        self.state
            .lock()
            .unwrap()
            .pending
            .iter()
            .filter_map(|p| match &p.event {
                Event::Video(unit, _) => Some(unit.clone()),
                _ => None,
            })
            .collect()
    }
    #[cfg(test)]
    pub(super) fn bitmaps(&self) -> Vec<NativeBitmapUnit> {
        self.state
            .lock()
            .unwrap()
            .pending
            .iter()
            .filter_map(|p| match &p.event {
                Event::Bitmap(unit) => Some(unit.clone()),
                _ => None,
            })
            .collect()
    }
}

pub(super) struct GraphicsExecutor {
    queue: Arc<GraphicsQueue>,
    worker: Option<JoinHandle<()>>,
}
impl Drop for GraphicsExecutor {
    fn drop(&mut self) {
        self.queue.cancel();
        if let Some(worker) = self.worker.take() {
            let _ = worker.join();
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::atomic::AtomicBool;
    use std::time::Instant;

    #[derive(Default)]
    struct Calls {
        seen: Mutex<Vec<u8>>,
        entered: AtomicBool,
        release: (Mutex<bool>, Condvar),
        audio: AtomicUsize,
    }
    extern "C" fn video(
        ctx: *mut core::ffi::c_void,
        _: u32,
        _: u32,
        data: *const u8,
        _: usize,
        _: u64,
    ) {
        let calls = unsafe { &*ctx.cast::<Calls>() };
        let value = unsafe { *data };
        calls.seen.lock().unwrap().push(value);
        if value == 1 {
            calls.entered.store(true, Ordering::Release);
            let guard = calls.release.0.lock().unwrap();
            let (_guard, timeout) = calls
                .release
                .1
                .wait_timeout_while(guard, Duration::from_secs(5), |released| !*released)
                .unwrap();
            assert!(!timeout.timed_out(), "test failed to release callback");
        }
    }
    extern "C" fn audio(ctx: *mut core::ffi::c_void, _: *const u8, _: usize, _: u32) {
        unsafe { &*ctx.cast::<Calls>() }
            .audio
            .fetch_add(1, Ordering::Relaxed);
    }
    fn sink(calls: &Calls) -> CallbackSink {
        CallbackSink::new(super::super::RdpCallbacks {
            ctx: (calls as *const Calls).cast_mut().cast(),
            on_video_au: Some(video),
            on_audio_data: Some(audio),
            ..Default::default()
        })
    }
    fn au(value: u8) -> Event {
        Event::Video(
            NativeVideoUnit {
                codec: 1,
                flags: 0,
                data: vec![value],
            },
            0,
        )
    }
    fn entered(calls: &Calls) {
        let deadline = Instant::now() + Duration::from_secs(2);
        while !calls.entered.load(Ordering::Acquire) {
            assert!(Instant::now() < deadline);
            thread::yield_now();
        }
    }
    fn release(calls: &Calls) {
        *calls.release.0.lock().unwrap() = true;
        calls.release.1.notify_all();
    }

    #[test]
    fn blocked_graphics_keeps_audio_and_input_live_and_barrier_bounded() {
        let calls = Calls::default();
        let (mut worker, tx) = super::super::tests::test_worker();
        worker.callbacks = sink(&calls);
        let queue = worker.graphics();
        let executor = queue.start(worker.callbacks).unwrap();
        queue.submit(au(1)).unwrap();
        entered(&calls);
        worker.callbacks.audio_data(&[0, 0], 0);
        tx.send(super::super::WorkerCommand::Input(
            super::super::InputCommand::PointerMove { x: 13, y: 21 },
        ))
        .unwrap();
        assert!(!worker.drain_commands());
        assert_eq!(worker.pending_input.len(), 1);
        assert_eq!(calls.audio.load(Ordering::Relaxed), 1);
        assert!(!queue.barrier(Duration::from_millis(2)));
        queue.submit(au(2)).unwrap();
        queue.submit(au(3)).unwrap();
        release(&calls);
        assert!(queue.barrier(Duration::from_secs(1)));
        assert_eq!(*calls.seen.lock().unwrap(), [1, 2, 3]);
        drop(executor);
        assert!(queue.submit(au(4)).is_none());
    }

    #[test]
    fn generation_reset_cancels_old_data_but_waits_for_running_callback() {
        let calls = Calls::default();
        let queue = Arc::new(GraphicsQueue::default());
        let executor = queue.start(sink(&calls)).unwrap();
        queue.submit(au(1)).unwrap();
        entered(&calls);
        queue.submit(au(2)).unwrap();
        let old = queue.progress().0;
        queue.reset(false);
        assert_eq!(queue.progress().0, old + 1);
        assert!(!queue.barrier(Duration::ZERO));
        queue.submit(au(3)).unwrap();
        release(&calls);
        assert!(queue.barrier(Duration::from_secs(1)));
        assert_eq!(*calls.seen.lock().unwrap(), [1, 3]);
        drop(executor);
    }

    #[test]
    fn stop_cancels_pending_and_joins_before_context_release() {
        let calls = Calls::default();
        let queue = Arc::new(GraphicsQueue::default());
        let executor = queue.start(sink(&calls)).unwrap();
        queue.submit(au(1)).unwrap();
        entered(&calls);
        queue.submit(au(2)).unwrap();
        queue.cancel();
        assert!(!queue.barrier(Duration::ZERO));
        release(&calls);
        drop(executor);
        assert!(queue.barrier(Duration::ZERO));
        assert_eq!(*calls.seen.lock().unwrap(), [1]);
        assert_eq!(queue.state.lock().unwrap().data_events, 0);
    }

    #[test]
    fn data_limit_includes_the_running_callback_without_dropping_auxiliary_aus() {
        let calls = Calls::default();
        let queue = Arc::new(GraphicsQueue::default());
        let executor = queue.start(sink(&calls)).unwrap();
        queue.submit(au(1)).unwrap();
        entered(&calls);
        for _ in 1..MAX_DATA_EVENTS {
            assert!(queue.submit(au(2)).is_some());
        }
        assert!(queue.submit(au(3)).is_none());
        assert!(queue.error().unwrap().contains("overflow"));
        assert_eq!(queue.state.lock().unwrap().data_events, MAX_DATA_EVENTS);
        queue.cancel();
        release(&calls);
        drop(executor);
    }

    #[test]
    fn control_events_are_bounded_and_oversized_video_is_not_queued() {
        let queue = GraphicsQueue::default();
        for _ in 0..MAX_EVENTS {
            assert!(queue.submit(Event::Frame).is_some());
        }
        assert!(queue.submit(Event::Frame).is_none());
        assert!(queue.error().is_some());
        queue.cancel();

        let queue = GraphicsQueue::default();
        queue.submit_video(0, 0, &vec![0; MAX_AU_BYTES + 1]);
        assert!(queue.error().unwrap().contains("32 MiB"));
        assert!(queue.videos().is_empty());
    }

    #[test]
    fn global_budget_covers_all_sessions_and_releases_on_cancel() {
        let first = GraphicsQueue::default();
        let second = GraphicsQueue::default();
        let large = || {
            Event::Bitmap(NativeBitmapUnit {
                surface_id: 0,
                left: 0,
                top: 0,
                width: 1,
                height: 1,
                stride: 4,
                data: vec![0; 24 * 1024 * 1024],
            })
        };
        assert!(first.submit(large()).is_some());
        assert!(second.submit(large()).is_some());
        assert!(second.submit(large()).is_none());
        first.cancel();
        second.cancel();
        let third = GraphicsQueue::default();
        assert!(third.submit(large()).is_some());
    }

    #[test]
    fn main_aux_pts_keep_the_verified_sequence() {
        extern "C" fn pts(
            ctx: *mut core::ffi::c_void,
            _: u32,
            _: u32,
            _: *const u8,
            _: usize,
            pts: u64,
        ) {
            unsafe { &*ctx.cast::<Mutex<Vec<u64>>>() }
                .lock()
                .unwrap()
                .push(pts);
        }
        let seen = Mutex::new(Vec::<u64>::new());
        let queue = Arc::new(GraphicsQueue::default());
        queue.configure_pts(30);
        queue.submit(au(9));
        queue.reset(false);
        queue.submit(au(1));
        let mut aux = au(2);
        if let Event::Video(unit, _) = &mut aux {
            unit.flags = super::super::abi::RDP_VIDEO_FLAG_AVC444_AUX;
        }
        queue.submit(aux);
        queue.submit(au(3));
        let sink = CallbackSink::new(super::super::RdpCallbacks {
            ctx: (&seen as *const Mutex<Vec<u64>>).cast_mut().cast(),
            on_video_au: Some(pts),
            ..Default::default()
        });
        let executor = queue.start(sink).unwrap();
        assert!(queue.barrier(Duration::from_secs(1)));
        drop(executor);
        assert_eq!(*seen.lock().unwrap(), [0, 3000, 3000]);
    }
}
