//! Passive telemetry. Only the worker mutates counters; the shared lock covers a copy.
use super::{abi::RdpNetworkStats, transport::PollingSocket};
use ironrdp_pdu::rdp::autodetect::AutoDetectRequest;
use std::collections::VecDeque;
use std::io;
use std::net::TcpStream;
use std::sync::{
    atomic::{AtomicU32, Ordering},
    Arc, LazyLock, Mutex,
};
use std::time::Instant;

pub(super) fn now_ms() -> u64 {
    static EPOCH: LazyLock<Instant> = LazyLock::new(Instant::now);
    EPOCH.elapsed().as_millis() as u64 + 1
}

#[derive(Default)]
pub(super) struct SocketCounters {
    pub active: bool,
    pub rx: u64,
    pub tx: u64,
    pub last_rx: u64,
}
impl SocketCounters {
    pub fn read_result(&mut self, result: &io::Result<usize>) {
        if let Ok(n) = result {
            if self.active && *n > 0 {
                self.rx += *n as u64;
                self.last_rx = now_ms();
            }
        }
    }
    pub fn write_result(&mut self, result: &io::Result<usize>) {
        if let Ok(n) = result {
            if self.active {
                self.tx += *n as u64;
            }
        }
    }
}

#[derive(Default)]
pub(super) struct NetworkTracker {
    pub shared: Arc<Mutex<RdpNetworkStats>>,
    stats: RdpNetworkStats,
    history: VecDeque<(u64, u64, u64)>,
    last_debug: u64,
}
impl NetworkTracker {
    pub fn begin(&mut self, socket: &mut PollingSocket) {
        static GENERATION: AtomicU32 = AtomicU32::new(1);
        let now = now_ms();
        self.stats = RdpNetworkStats {
            generation: u64::from(GENERATION.fetch_add(1, Ordering::Relaxed)),
            active: 1,
            started_ms: now,
            ..Default::default()
        };
        socket.counters = SocketCounters {
            active: true,
            ..Default::default()
        };
        self.history.clear();
        self.history.push_back((now, 0, 0));
        self.last_debug = now;
        self.publish(socket, now);
    }
    pub fn close(&mut self) {
        self.stats.active = 0;
        if let Ok(mut out) = self.shared.lock() {
            *out = self.stats;
        }
    }
    pub fn report(&mut self, report: AutoDetectRequest) {
        self.report_at(report, now_ms());
    }
    fn report_at(&mut self, report: AutoDetectRequest, now: u64) {
        if let AutoDetectRequest::NetworkCharacteristicsResult {
            base_rtt_ms,
            bandwidth_kbps,
            average_rtt_ms,
            ..
        } = report
        {
            self.stats.report_ms = now;
            self.stats.reports += 1;
            self.stats.average_rtt_ms = average_rtt_ms;
            if let Some(value) = base_rtt_ms {
                self.stats.base_rtt_ms = value;
                self.stats.base_rtt_ms_at = now;
            }
            if let Some(value) = bandwidth_kbps {
                self.stats.bandwidth_kbps = value;
                self.stats.bandwidth_ms_at = now;
            }
        }
    }
    pub fn sample(&mut self, socket: &PollingSocket) {
        let now = now_ms();
        if now - self.stats.sampled_ms >= 500 {
            self.publish(socket, now);
        }
    }
    fn publish(&mut self, socket: &PollingSocket, now: u64) {
        let c = &socket.counters;
        self.stats.sampled_ms = now;
        self.stats.rx_bytes = c.rx;
        self.stats.tx_bytes = c.tx;
        self.stats.last_rx_ms = c.last_rx;
        self.rates(now, c.rx, c.tx);
        tcp_info(&socket.socket, &mut self.stats);
        if let Ok(mut out) = self.shared.lock() {
            *out = self.stats;
        }
        if now - self.last_debug >= 5000 {
            self.last_debug = now;
            tracing::debug!(target: super::LOG_TARGET_SESSION,
                generation = self.stats.generation, rx_bytes = c.rx, tx_bytes = c.tx,
                rx_kbps = self.stats.rx_kbps, tx_kbps = self.stats.tx_kbps,
                reports = self.stats.reports, average_rtt_ms = self.stats.average_rtt_ms,
                bandwidth_kbps = self.stats.bandwidth_kbps, tcp_valid = self.stats.tcp_valid,
                tcp_srtt_us = self.stats.tcp_srtt_us, tcp_retransmits = self.stats.tcp_retransmits,
                "network telemetry (500 ms samples, 5 s rates)");
        }
    }
    fn rates(&mut self, now: u64, rx: u64, tx: u64) {
        while self.history.len() > 1 && now - self.history[1].0 >= 5000 {
            self.history.pop_front();
        }
        self.stats.rate_valid = 0;
        if let Some(&(at, before_rx, before_tx)) = self.history.front() {
            let dt = now - at;
            if (5000..=6000).contains(&dt) {
                self.stats.rate_valid = 1;
                self.stats.rx_kbps = ((rx - before_rx) * 8 / dt).min(u32::MAX as u64) as u32;
                self.stats.tx_kbps = ((tx - before_tx) * 8 / dt).min(u32::MAX as u64) as u32;
            }
        }
        if self.history.back().is_none_or(|p| p.0 != now) {
            self.history.push_back((now, rx, tx));
        }
        while self.history.len() > 12 {
            self.history.pop_front();
        }
    }
}

// Linux UAPI tcp_info's fixed prefix uses native-endian u32s on both ARMv7 and host.
fn parse_tcp_info(bytes: &[u8], stats: &mut RdpNetworkStats) {
    stats.tcp_valid = 0;
    for (bit, offset, out) in [
        (0, 68, &mut stats.tcp_srtt_us),
        (1, 72, &mut stats.tcp_rttvar_us),
        (2, 8, &mut stats.tcp_rto_us),
        (3, 24, &mut stats.tcp_unacked),
        (4, 80, &mut stats.tcp_cwnd),
        (5, 100, &mut stats.tcp_retransmits),
    ] {
        *out = 0;
        if let Some(field) = bytes.get(offset..offset + 4) {
            *out = u32::from_ne_bytes(field.try_into().unwrap());
            stats.tcp_valid |= 1 << bit;
        }
    }
}
#[cfg(target_os = "linux")]
fn tcp_info(socket: &TcpStream, stats: &mut RdpNetworkStats) {
    use std::os::fd::AsRawFd;
    unsafe extern "C" {
        fn getsockopt(
            fd: i32,
            level: i32,
            option: i32,
            value: *mut core::ffi::c_void,
            len: *mut u32,
        ) -> i32;
    }
    let mut bytes = [0u8; 104];
    let mut len = bytes.len() as u32;
    // IPPROTO_TCP=6, TCP_INFO=11; kernel writes at most the supplied buffer length.
    let result = unsafe {
        getsockopt(
            socket.as_raw_fd(),
            6,
            11,
            bytes.as_mut_ptr().cast(),
            &mut len,
        )
    };
    parse_tcp_info(
        if result == 0 {
            &bytes[..(len as usize).min(bytes.len())]
        } else {
            &[]
        },
        stats,
    );
}
#[cfg(not(target_os = "linux"))]
fn tcp_info(_: &TcpStream, stats: &mut RdpNetworkStats) {
    parse_tcp_info(&[], stats);
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn reports_preserve_optional_field_age_and_zero_rtt() {
        use ironrdp_core::{decode, encode_vec};
        use ironrdp_pdu::rdp::autodetect::AutoDetectReqPdu;
        let mut tracker = NetworkTracker::default();
        for (at, kind, base, bandwidth) in [
            (1, 0x08c0, Some(0), Some(1000)),
            (8002, 0x0840, Some(0), None),
            (9003, 0x0880, None, Some(500)),
        ] {
            let request = AutoDetectRequest::NetworkCharacteristicsResult {
                sequence_number: 1,
                request_type: kind,
                base_rtt_ms: base,
                bandwidth_kbps: bandwidth,
                average_rtt_ms: 0,
            };
            let wire = encode_vec(&AutoDetectReqPdu::new(request)).unwrap();
            let decoded: AutoDetectReqPdu = decode(&wire).unwrap();
            tracker.report_at(decoded.request, at);
            if at == 8002 {
                assert_eq!(tracker.stats.bandwidth_ms_at, 1);
            }
        }
        assert_eq!(tracker.stats.reports, 3);
        assert_eq!(tracker.stats.average_rtt_ms, 0);
        assert_eq!(tracker.stats.base_rtt_ms_at, 8002);
        assert_eq!(tracker.stats.bandwidth_ms_at, 9003);
    }
    #[test]
    fn counts_successful_bytes_only() {
        let mut c = SocketCounters::default();
        c.read_result(&Ok(100));
        c.write_result(&Ok(100));
        assert_eq!((c.rx, c.tx), (0, 0));
        c.active = true;
        c.read_result(&Ok(3));
        c.read_result(&Ok(2));
        c.write_result(&Ok(1));
        c.write_result(&Ok(4));
        let last = c.last_rx;
        for kind in [
            io::ErrorKind::TimedOut,
            io::ErrorKind::WouldBlock,
            io::ErrorKind::BrokenPipe,
        ] {
            c.read_result(&Err(io::Error::from(kind)));
            c.write_result(&Err(io::Error::from(kind)));
        }
        c.read_result(&Ok(0));
        assert_eq!((c.rx, c.tx, c.last_rx), (5, 5, last));
    }
    #[test]
    fn short_tcp_info_and_units() {
        let mut stats = RdpNetworkStats::default();
        let mut bytes = [0u8; 104];
        bytes[68..72].copy_from_slice(&1234u32.to_ne_bytes());
        bytes[100..104].copy_from_slice(&7u32.to_ne_bytes());
        parse_tcp_info(&bytes, &mut stats);
        assert_eq!(
            (stats.tcp_valid, stats.tcp_srtt_us, stats.tcp_retransmits),
            (63, 1234, 7)
        );
        parse_tcp_info(&bytes[..71], &mut stats);
        assert_eq!(stats.tcp_valid, (1 << 2) | (1 << 3));
        parse_tcp_info(&[], &mut stats);
        assert_eq!(stats.tcp_valid, 0);
    }
    #[test]
    fn rates_need_five_seconds_and_include_idle() {
        let mut t = NetworkTracker::default();
        t.rates(1, 0, 0);
        t.rates(501, 1000, 500);
        assert_eq!(t.stats.rate_valid, 0);
        t.rates(5001, 625000, 125000);
        assert_eq!(
            (t.stats.rate_valid, t.stats.rx_kbps, t.stats.tx_kbps),
            (1, 1000, 200)
        );
        t.rates(10001, 625000, 125000);
        assert_eq!(
            (t.stats.rate_valid, t.stats.rx_kbps, t.stats.tx_kbps),
            (1, 0, 0)
        );
    }
    #[test]
    fn loopback_and_new_generation() {
        use std::io::{Read, Write};
        let listener = std::net::TcpListener::bind("127.0.0.1:0").unwrap();
        let client = TcpStream::connect(listener.local_addr().unwrap()).unwrap();
        let (mut peer, _) = listener.accept().unwrap();
        let mut socket = PollingSocket {
            socket: client,
            counters: Default::default(),
            stop: Default::default(),
            phase_deadline: None,
            write_deadline: Default::default(),
        };
        let mut t = NetworkTracker::default();
        t.begin(&mut socket);
        let generation = t.stats.generation;
        peer.write_all(b"hello").unwrap();
        let mut data = [0; 5];
        socket.read_exact(&mut data).unwrap();
        socket.write_all(b"ok").unwrap();
        t.publish(&socket, now_ms());
        assert_eq!((t.stats.rx_bytes, t.stats.tx_bytes), (5, 2));
        #[cfg(target_os = "linux")]
        assert_eq!(t.stats.tcp_valid, 63);
        t.close();
        assert_eq!(t.shared.lock().unwrap().active, 0);
        t.begin(&mut socket);
        assert_ne!(t.stats.generation, generation);
        assert_eq!(
            (t.stats.rx_bytes, t.stats.tx_bytes, t.stats.reports),
            (0, 0, 0)
        );
        assert_eq!(core::mem::size_of::<RdpNetworkStats>(), 144);
        assert_eq!(core::mem::offset_of!(RdpNetworkStats, active), 88);
        assert_eq!(core::mem::offset_of!(RdpNetworkStats, tcp_retransmits), 140);
    }
}
