//! Bridge from IronRDP's `tracing` events to the C shell's log callback.
//!
//! Each worker thread pins its own `CallbackSink` in TLS for the duration of a
//! session (`CallbackSinkGuard`); the global `CallbackLogLayer` filters by a
//! process-wide level ceiling that only ever rises, so parallel sessions with
//! different C-side levels stay isolated without re-registering subscribers.

use std::cell::Cell;
use std::fmt;
use std::sync::atomic::{AtomicU8, Ordering};

use super::{CallbackSink, RdpLogLevel};

pub(super) const LOG_TARGET_CAMERA: &str = "webrdp.camera";
pub(super) const LOG_TARGET_AUDIO_INPUT: &str = "webrdp.audio_input";
pub(super) const LOG_TARGET_TRANSPORT: &str = "webrdp.transport";
pub(super) const LOG_TARGET_SESSION: &str = "webrdp.session";
pub(super) const LOG_TARGET_GRAPHICS: &str = "webrdp.graphics";
pub(super) const LOG_TARGET_AUDIO: &str = "webrdp.audio";

thread_local! {
    static CURRENT_CALLBACK_SINK: Cell<Option<CallbackSink>> = const { Cell::new(None) };
}

pub(super) struct CallbackSinkGuard {
    previous: Option<CallbackSink>,
}

impl CallbackSinkGuard {
    pub(super) fn enter(sink: CallbackSink) -> Self {
        raise_level_ceiling(&sink);
        Self {
            previous: CURRENT_CALLBACK_SINK.with(|current| current.replace(Some(sink))),
        }
    }
}

impl Drop for CallbackSinkGuard {
    fn drop(&mut self) {
        CURRENT_CALLBACK_SINK.with(|current| current.set(self.previous));
    }
}

fn current_callback_sink() -> Option<CallbackSink> {
    CURRENT_CALLBACK_SINK.with(Cell::get)
}

/// Most verbose level any callback sink has ever reported deliverable, as a rank
/// (0 = none, 1 = error … 5 = trace). Levels above the ceiling are pruned at the
/// callsite instead of paying a per-event FFI check. Raise-only: the C level
/// configuration is fixed at launch (LGNOME_LOG), so a scope-entry probe is
/// exact, and a sink below the ceiling still rejects its own events in `enabled`.
static MAX_SINK_LEVEL_RANK: AtomicU8 = AtomicU8::new(0);

fn rdp_level_rank(level: RdpLogLevel) -> u8 {
    match level {
        RdpLogLevel::Trace => 5,
        RdpLogLevel::Debug => 4,
        RdpLogLevel::Info | RdpLogLevel::Notice => 3,
        RdpLogLevel::Warning => 2,
        RdpLogLevel::Error | RdpLogLevel::Fatal => 1,
    }
}

fn level_filter_for_rank(rank: u8) -> tracing::level_filters::LevelFilter {
    use tracing::level_filters::LevelFilter;

    match rank {
        0 => LevelFilter::OFF,
        1 => LevelFilter::ERROR,
        2 => LevelFilter::WARN,
        3 => LevelFilter::INFO,
        4 => LevelFilter::DEBUG,
        _ => LevelFilter::TRACE,
    }
}

fn probe_sink_level_rank(sink: &CallbackSink) -> u8 {
    const PROBE_LEVELS: [RdpLogLevel; 5] = [
        RdpLogLevel::Trace,
        RdpLogLevel::Debug,
        RdpLogLevel::Info,
        RdpLogLevel::Warning,
        RdpLogLevel::Error,
    ];
    PROBE_LEVELS
        .into_iter()
        .filter(|&level| sink.log_enabled(level))
        .map(rdp_level_rank)
        .max()
        .unwrap_or(0)
}

fn raise_level_ceiling(sink: &CallbackSink) {
    let rank = probe_sink_level_rank(sink);
    let previous = MAX_SINK_LEVEL_RANK.fetch_max(rank, Ordering::AcqRel);
    let observed_ceiling = previous.max(rank);
    let raised_ceiling = rank > previous;

    // A newly raised ceiling always requires rebuilding cached Interest values. The
    // winning fetch_max thread may still be doing that work when another worker sees
    // the new rank: tracing-core publishes LevelFilter::current() only at the end of
    // a rebuild, so that observer must perform its own idempotent rebuild while the
    // published filter lags. Returning from scope entry is therefore the
    // synchronization point for the first events emitted by the worker.
    if raised_ceiling
        || tracing::level_filters::LevelFilter::current() < level_filter_for_rank(observed_ceiling)
    {
        tracing::callsite::rebuild_interest_cache();
    }
}

fn tracing_log_level(level: &tracing::Level) -> RdpLogLevel {
    if *level == tracing::Level::ERROR {
        RdpLogLevel::Error
    } else if *level == tracing::Level::WARN {
        RdpLogLevel::Warning
    } else if *level == tracing::Level::INFO {
        RdpLogLevel::Info
    } else if *level == tracing::Level::DEBUG {
        RdpLogLevel::Debug
    } else {
        RdpLogLevel::Trace
    }
}

#[derive(Default)]
struct LogEventVisitor {
    message: Option<String>,
    fields: Vec<String>,
}

impl tracing::field::Visit for LogEventVisitor {
    fn record_debug(&mut self, field: &tracing::field::Field, value: &dyn fmt::Debug) {
        let value = format!("{value:?}");
        if field.name() == "message" {
            self.message = Some(value);
        } else {
            self.fields.push(format!("{}={value}", field.name()));
        }
    }
}

impl LogEventVisitor {
    fn finish(self, fallback: &str) -> String {
        match (self.message, self.fields.is_empty()) {
            (Some(message), true) => message,
            (Some(message), false) => format!("{message} {}", self.fields.join(" ")),
            (None, false) => self.fields.join(" "),
            (None, true) => fallback.to_owned(),
        }
    }
}

/// Global tracing layer whose destination is selected per worker thread. Callsites at
/// levels no sink has ever enabled are `Interest::never()` — rebuilt whenever
/// `CallbackSinkGuard::enter` lifts the ceiling — and everything else stays
/// `Interest::sometimes` because different concurrent sessions may enable different
/// levels and targets through their own C callback tables.
pub(crate) struct CallbackLogLayer;

impl<S> tracing_subscriber::Layer<S> for CallbackLogLayer
where
    S: tracing::Subscriber,
{
    fn register_callsite(
        &self,
        metadata: &'static tracing::Metadata<'static>,
    ) -> tracing::subscriber::Interest {
        if rdp_level_rank(tracing_log_level(metadata.level()))
            > MAX_SINK_LEVEL_RANK.load(Ordering::Relaxed)
        {
            return tracing::subscriber::Interest::never();
        }
        tracing::subscriber::Interest::sometimes()
    }

    fn max_level_hint(&self) -> Option<tracing::level_filters::LevelFilter> {
        Some(level_filter_for_rank(
            MAX_SINK_LEVEL_RANK.load(Ordering::Relaxed),
        ))
    }

    fn enabled(
        &self,
        metadata: &tracing::Metadata<'_>,
        _ctx: tracing_subscriber::layer::Context<'_, S>,
    ) -> bool {
        let level = tracing_log_level(metadata.level());
        if rdp_level_rank(level) > MAX_SINK_LEVEL_RANK.load(Ordering::Relaxed) {
            return false;
        }
        current_callback_sink()
            .map(|sink| sink.log_enabled(level))
            .unwrap_or(false)
    }

    fn on_event(
        &self,
        event: &tracing::Event<'_>,
        _ctx: tracing_subscriber::layer::Context<'_, S>,
    ) {
        let Some(sink) = current_callback_sink() else {
            return;
        };
        let metadata = event.metadata();
        let mut visitor = LogEventVisitor::default();
        event.record(&mut visitor);
        sink.emit_log(
            tracing_log_level(metadata.level()),
            metadata.target(),
            &visitor.finish(metadata.name()),
        );
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::ffi::{c_char, CStr};
    use std::fmt;
    use std::sync::Mutex;

    #[derive(Debug, PartialEq, Eq)]
    struct CapturedLog {
        level: RdpLogLevel,
        target: String,
        message: String,
    }

    struct LogCapture {
        enabled: bool,
        checks: Vec<RdpLogLevel>,
        logs: Vec<CapturedLog>,
    }

    impl Default for LogCapture {
        fn default() -> Self {
            Self {
                enabled: true,
                checks: Vec::new(),
                logs: Vec::new(),
            }
        }
    }

    extern "C" fn capture_log_enabled(ctx: *mut core::ffi::c_void, level: RdpLogLevel) -> bool {
        let capture = unsafe { &*(ctx.cast::<Mutex<LogCapture>>()) };
        let mut capture = capture.lock().unwrap();
        capture.checks.push(level);
        capture.enabled
    }

    extern "C" fn capture_log(
        ctx: *mut core::ffi::c_void,
        level: RdpLogLevel,
        target: *const c_char,
        message: *const c_char,
    ) {
        let capture = unsafe { &*(ctx.cast::<Mutex<LogCapture>>()) };
        let target = unsafe { CStr::from_ptr(target) }
            .to_string_lossy()
            .into_owned();
        let message = unsafe { CStr::from_ptr(message) }
            .to_string_lossy()
            .into_owned();
        capture.lock().unwrap().logs.push(CapturedLog {
            level,
            target,
            message,
        });
    }

    fn log_test_sink(capture: *const Mutex<LogCapture>) -> CallbackSink {
        let mut callbacks = CallbackSink::default().callbacks;
        callbacks.ctx = capture.cast_mut().cast();
        callbacks.on_log_enabled = Some(capture_log_enabled);
        callbacks.on_log = Some(capture_log);
        CallbackSink::new(callbacks)
    }

    #[derive(Default)]
    struct LevelProbeCapture {
        max_rank: u8,
        checks: Vec<RdpLogLevel>,
    }

    extern "C" fn capture_level_probe(ctx: *mut core::ffi::c_void, level: RdpLogLevel) -> bool {
        let capture = unsafe { &*(ctx.cast::<Mutex<LevelProbeCapture>>()) };
        let mut capture = capture.lock().unwrap();
        capture.checks.push(level);
        rdp_level_rank(level) <= capture.max_rank
    }

    extern "C" fn ignore_log(
        _ctx: *mut core::ffi::c_void,
        _level: RdpLogLevel,
        _target: *const c_char,
        _message: *const c_char,
    ) {
    }

    fn level_probe_sink(capture: *const Mutex<LevelProbeCapture>) -> CallbackSink {
        let mut callbacks = CallbackSink::default().callbacks;
        callbacks.ctx = capture.cast_mut().cast();
        callbacks.on_log_enabled = Some(capture_level_probe);
        callbacks.on_log = Some(ignore_log);
        CallbackSink::new(callbacks)
    }

    struct CountFormats(std::sync::Arc<std::sync::atomic::AtomicUsize>);

    impl fmt::Debug for CountFormats {
        fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
            self.0.fetch_add(1, Ordering::SeqCst);
            f.write_str("formatted")
        }
    }

    #[test]
    fn callback_sink_forwards_all_levels_and_filters_before_formatting() {
        let capture = Mutex::new(LogCapture::default());
        let sink = log_test_sink(&capture);
        let levels = [
            RdpLogLevel::Trace,
            RdpLogLevel::Debug,
            RdpLogLevel::Info,
            RdpLogLevel::Notice,
            RdpLogLevel::Warning,
            RdpLogLevel::Error,
            RdpLogLevel::Fatal,
        ];
        for (index, level) in levels.into_iter().enumerate() {
            sink.log(level, "webrdp.test", format_args!("message {index}"));
        }

        let capture_guard = capture.lock().unwrap();
        assert_eq!(capture_guard.checks.len(), levels.len());
        assert_eq!(capture_guard.logs.len(), levels.len());
        for (index, log) in capture_guard.logs.iter().enumerate() {
            assert_eq!(log.level, levels[index]);
            assert_eq!(log.target, "webrdp.test");
            assert_eq!(log.message, format!("message {index}"));
        }
        drop(capture_guard);

        capture.lock().unwrap().enabled = false;
        let formats = std::sync::Arc::new(std::sync::atomic::AtomicUsize::new(0));
        sink.log(
            RdpLogLevel::Debug,
            "webrdp.test",
            format_args!(
                "blocked {:?}",
                CountFormats(std::sync::Arc::clone(&formats))
            ),
        );
        assert_eq!(formats.load(Ordering::SeqCst), 0);
        assert_eq!(capture.lock().unwrap().logs.len(), levels.len());
    }

    #[test]
    fn missing_logging_callbacks_are_optional() {
        let capture = Mutex::new(LogCapture::default());
        let mut sink = log_test_sink(&capture);
        sink.callbacks.on_log_enabled = None;
        assert!(sink.log_enabled(RdpLogLevel::Trace));
        sink.log(RdpLogLevel::Info, "test", format_args!("unfiltered"));
        assert_eq!(capture.lock().unwrap().logs.len(), 1);
        sink.callbacks.on_log = None;
        assert!(!sink.log_enabled(RdpLogLevel::Error));
        assert!(!CallbackSink::default().log_enabled(RdpLogLevel::Error));
    }

    #[test]
    fn level_ceiling_rank_mapping_and_sink_probe_cover_every_filter() {
        use tracing::level_filters::LevelFilter;

        let expected_filters = [
            LevelFilter::OFF,
            LevelFilter::ERROR,
            LevelFilter::WARN,
            LevelFilter::INFO,
            LevelFilter::DEBUG,
            LevelFilter::TRACE,
        ];
        for (rank, expected_filter) in expected_filters.into_iter().enumerate() {
            assert_eq!(level_filter_for_rank(rank as u8), expected_filter);

            let capture = Mutex::new(LevelProbeCapture {
                max_rank: rank as u8,
                ..LevelProbeCapture::default()
            });
            assert_eq!(
                probe_sink_level_rank(&level_probe_sink(&capture)),
                rank as u8
            );

            let capture = capture.lock().unwrap();
            assert_eq!(
                capture.checks,
                [
                    RdpLogLevel::Trace,
                    RdpLogLevel::Debug,
                    RdpLogLevel::Info,
                    RdpLogLevel::Warning,
                    RdpLogLevel::Error
                ]
            );
        }
    }

    #[test]
    fn tracing_bridge_preserves_metadata_and_clears_worker_sink() {
        crate::init_logging();
        let capture = Mutex::new(LogCapture {
            enabled: false,
            ..LogCapture::default()
        });
        let sink = log_test_sink(&capture);
        let formats = std::sync::Arc::new(std::sync::atomic::AtomicUsize::new(0));
        {
            let _guard = CallbackSinkGuard::enter(sink);
            tracing::debug!(
                target: "webrdp.bridge-test",
                expensive = ?CountFormats(std::sync::Arc::clone(&formats)),
                "blocked event"
            );
            assert_eq!(formats.load(Ordering::SeqCst), 0);
            assert!(capture.lock().unwrap().logs.is_empty());
        }

        // Enablement is probed when a worker scope is entered, so a level flipped
        // on mid-session applies from the next scope (reconnect) on.
        capture.lock().unwrap().enabled = true;
        {
            let _guard = CallbackSinkGuard::enter(sink);
            tracing::warn!(
                target: "webrdp.bridge-test",
                code = 42_u64,
                reason = "test",
                "delivered event"
            );
        }

        let logged_before_cleanup = capture.lock().unwrap().logs.len();
        tracing::info!(target: "webrdp.bridge-test", "outside worker");
        let capture = capture.lock().unwrap();
        assert_eq!(capture.logs.len(), logged_before_cleanup);
        assert_eq!(capture.logs.len(), 1);
        assert_eq!(capture.logs[0].level, RdpLogLevel::Warning);
        assert_eq!(capture.logs[0].target, "webrdp.bridge-test");
        assert_eq!(
            capture.logs[0].message,
            "delivered event code=42 reason=\"test\""
        );
    }

    #[test]
    fn tracing_bridge_isolates_parallel_worker_sinks() {
        crate::init_logging();
        let first = std::sync::Arc::new(Mutex::new(LogCapture::default()));
        let second = std::sync::Arc::new(Mutex::new(LogCapture::default()));
        let barrier = std::sync::Arc::new(std::sync::Barrier::new(2));

        let spawn_worker =
            |id: u32,
             capture: std::sync::Arc<Mutex<LogCapture>>,
             barrier: std::sync::Arc<std::sync::Barrier>| {
                std::thread::spawn(move || {
                    let sink = log_test_sink(std::sync::Arc::as_ptr(&capture));
                    let _guard = CallbackSinkGuard::enter(sink);
                    barrier.wait();
                    tracing::info!(target: "webrdp.parallel-test", worker = id, "worker event");
                })
            };
        let first_worker = spawn_worker(
            1,
            std::sync::Arc::clone(&first),
            std::sync::Arc::clone(&barrier),
        );
        let second_worker = spawn_worker(2, std::sync::Arc::clone(&second), barrier);
        first_worker.join().unwrap();
        second_worker.join().unwrap();

        let first = first.lock().unwrap();
        let second = second.lock().unwrap();
        assert_eq!(first.logs.len(), 1);
        assert_eq!(second.logs.len(), 1);
        assert_eq!(first.logs[0].message, "worker event worker=1");
        assert_eq!(second.logs[0].message, "worker event worker=2");
    }

    #[test]
    fn sink_probe_lifts_global_level_ceiling() {
        crate::init_logging();
        let capture = Mutex::new(LogCapture::default());
        let sink = log_test_sink(&capture);
        {
            let _guard = CallbackSinkGuard::enter(sink);
        }
        assert_eq!(MAX_SINK_LEVEL_RANK.load(Ordering::Relaxed), 5);
        assert_eq!(
            tracing::level_filters::LevelFilter::current(),
            tracing::level_filters::LevelFilter::TRACE
        );
        // Entering the scope probed each bridged level exactly once.
        let capture = capture.lock().unwrap();
        assert_eq!(
            capture.checks,
            [
                RdpLogLevel::Trace,
                RdpLogLevel::Debug,
                RdpLogLevel::Info,
                RdpLogLevel::Warning,
                RdpLogLevel::Error
            ]
        );
    }
}
