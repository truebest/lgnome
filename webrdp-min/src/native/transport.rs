//! Transport primitives: socket timeouts, TLS plumbing, and byte-level
//! helpers shared by the connect/CredSSP/session read paths.
//!
//! Self-signed certificates are accepted; TLS handshake signatures still prove
//! possession of the presented key used for CredSSP channel binding.

#![forbid(unsafe_code)]

use std::io::{self, Read, Write};
use std::net::TcpStream;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

use super::NativeError;
use ironrdp_core::MonotonicInstant;

pub(super) const IO_POLL_TIMEOUT: Duration = Duration::from_millis(10);
pub(super) const CONNECT_TIMEOUT: Duration = Duration::from_secs(5);
pub(super) const WRITE_TIMEOUT: Duration = Duration::from_secs(5);
pub(super) const ESTABLISH_TIMEOUT: Duration = Duration::from_secs(30);
pub(super) const REACTIVATE_TIMEOUT: Duration = Duration::from_secs(15);
pub(super) const MAX_TS_REQUEST_SIZE: usize = 1024 * 1024;
pub(super) const MAX_TRANSPORT_BUFFER_SIZE: usize = MAX_TS_REQUEST_SIZE + 8192;

/// The one owned TLS stream type every read/write path shares.
pub(super) type TlsStream = rustls::StreamOwned<rustls::ClientConnection, PollingSocket>;

pub(super) struct PollingSocket {
    pub(super) socket: TcpStream,
    pub(super) stop: Arc<AtomicBool>,
    pub(super) phase_deadline: Option<Instant>,
    pub(super) write_deadline: Arc<Mutex<Option<Instant>>>,
}

impl Read for PollingSocket {
    fn read(&mut self, bytes: &mut [u8]) -> io::Result<usize> {
        if self.stop.load(Ordering::SeqCst) {
            return Err(io::Error::new(
                io::ErrorKind::ConnectionAborted,
                "session stopped",
            ));
        }
        if self.phase_deadline.is_some_and(|at| Instant::now() >= at) {
            return Err(io::Error::new(
                io::ErrorKind::TimedOut,
                "connection phase deadline exceeded",
            ));
        }
        self.socket.read(bytes)
    }
}

impl Write for PollingSocket {
    fn write(&mut self, bytes: &[u8]) -> io::Result<usize> {
        if self.stop.load(Ordering::SeqCst) {
            return Err(io::Error::new(
                io::ErrorKind::ConnectionAborted,
                "session stopped",
            ));
        }
        let deadline = self
            .write_deadline
            .lock()
            .map_err(|_| io::Error::other("write deadline lock poisoned"))?;
        if deadline.is_some_and(|at| Instant::now() >= at)
            || self.phase_deadline.is_some_and(|at| Instant::now() >= at)
        {
            return Err(io::Error::new(
                io::ErrorKind::TimedOut,
                "write deadline exceeded",
            ));
        }
        drop(deadline);
        self.socket.write(bytes)
    }

    fn flush(&mut self) -> io::Result<()> {
        self.socket.flush()
    }
}

pub(super) fn monotonic_now() -> MonotonicInstant {
    static EPOCH: std::sync::LazyLock<std::time::Instant> =
        std::sync::LazyLock::new(std::time::Instant::now);
    MonotonicInstant::from_millis(u64::try_from(EPOCH.elapsed().as_millis()).unwrap_or(u64::MAX))
}

pub(super) fn random_array<const N: usize>() -> Result<[u8; N], NativeError> {
    let mut a = [0u8; N];
    getrandom::fill(&mut a)
        .map_err(|e| NativeError::network(format!("entropy unavailable: {e}")))?;
    Ok(a)
}

pub(super) fn hex_prefix(bytes: &[u8], max: usize) -> String {
    use std::fmt::Write as _;
    let shown = &bytes[..bytes.len().min(max)];
    let mut hex = String::with_capacity(2 * shown.len() + 2);
    for b in shown {
        let _ = write!(hex, "{b:02x}");
    }
    if bytes.len() > max {
        hex.push_str("..");
    }
    hex
}

pub(super) fn ts_request_len(buf: &[u8]) -> Result<Option<usize>, NativeError> {
    if buf.is_empty() {
        return Ok(None);
    }
    if buf[0] != 0x30 {
        return Err(NativeError::protocol(
            "CredSSP TSRequest has an invalid DER tag",
        ));
    }
    if buf.len() < 2 {
        return Ok(None);
    }
    let (header_len, length): (usize, usize) = if buf[1] < 0x80 {
        (2, usize::from(buf[1]))
    } else {
        let n = usize::from(buf[1] & 0x7f);
        if n == 0 || n > 4 {
            return Err(NativeError::protocol(
                "CredSSP TSRequest has an invalid DER length",
            ));
        }
        if buf.len() < 2 + n {
            return Ok(None);
        }
        if buf[2] == 0 {
            return Err(NativeError::protocol(
                "CredSSP TSRequest has a nonminimal DER length",
            ));
        }
        let mut length = 0usize;
        for &byte in &buf[2..2 + n] {
            length = length
                .checked_mul(256)
                .and_then(|v| v.checked_add(usize::from(byte)))
                .ok_or_else(|| NativeError::protocol("CredSSP TSRequest length overflow"))?;
        }
        if length < 128 {
            return Err(NativeError::protocol(
                "CredSSP TSRequest has a nonminimal DER length",
            ));
        }
        (2 + n, length)
    };
    let total = header_len
        .checked_add(length)
        .filter(|&total| total <= MAX_TS_REQUEST_SIZE)
        .ok_or_else(|| NativeError::protocol("CredSSP TSRequest exceeds the message size limit"))?;
    Ok(Some(total))
}

pub(super) fn is_timeout(e: &io::Error) -> bool {
    matches!(
        e.kind(),
        io::ErrorKind::WouldBlock | io::ErrorKind::TimedOut | io::ErrorKind::Interrupted
    )
}

pub(super) fn install_rustls_provider() {
    static ONCE: std::sync::Once = std::sync::Once::new();
    ONCE.call_once(|| {
        let _ = rustls::crypto::ring::default_provider().install_default();
    });
}

#[derive(Debug)]
pub(super) struct NoCertificateVerification;

impl rustls::client::danger::ServerCertVerifier for NoCertificateVerification {
    fn verify_server_cert(
        &self,
        _: &rustls::pki_types::CertificateDer<'_>,
        _: &[rustls::pki_types::CertificateDer<'_>],
        _: &rustls::pki_types::ServerName<'_>,
        _: &[u8],
        _: rustls::pki_types::UnixTime,
    ) -> Result<rustls::client::danger::ServerCertVerified, rustls::Error> {
        Ok(rustls::client::danger::ServerCertVerified::assertion())
    }

    fn verify_tls12_signature(
        &self,
        message: &[u8],
        cert: &rustls::pki_types::CertificateDer<'_>,
        dss: &rustls::DigitallySignedStruct,
    ) -> Result<rustls::client::danger::HandshakeSignatureValid, rustls::Error> {
        rustls::crypto::verify_tls12_signature(
            message,
            cert,
            dss,
            &rustls::crypto::ring::default_provider().signature_verification_algorithms,
        )
    }

    fn verify_tls13_signature(
        &self,
        message: &[u8],
        cert: &rustls::pki_types::CertificateDer<'_>,
        dss: &rustls::DigitallySignedStruct,
    ) -> Result<rustls::client::danger::HandshakeSignatureValid, rustls::Error> {
        rustls::crypto::verify_tls13_signature(
            message,
            cert,
            dss,
            &rustls::crypto::ring::default_provider().signature_verification_algorithms,
        )
    }

    fn supported_verify_schemes(&self) -> Vec<rustls::SignatureScheme> {
        rustls::crypto::ring::default_provider()
            .signature_verification_algorithms
            .supported_schemes()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn hex_prefix_truncates_and_marks_overflow() {
        assert_eq!(hex_prefix(&[0x01, 0xab, 0xff], 4), "01abff");
        assert_eq!(hex_prefix(&[0x01, 0xab, 0xff], 2), "01ab..");
        assert_eq!(hex_prefix(&[], 4), "");
    }
    #[test]
    fn ts_request_framing_distinguishes_fragmented_headers_from_invalid_der() {
        for bytes in [&[][..], &[0x30], &[0x30, 0x82], &[0x30, 0x82, 0x01]] {
            assert_eq!(ts_request_len(bytes).unwrap(), None);
        }
        assert_eq!(ts_request_len(&[0x30, 0x03]).unwrap(), Some(5));
        assert_eq!(
            ts_request_len(&[0x30, 0x82, 0x01, 0x00]).unwrap(),
            Some(260)
        );
        for bytes in [
            &[0x31][..],
            &[0x30, 0x80],
            &[0x30, 0x85],
            &[0x30, 0x81, 0x7f],
            &[0x30, 0x82, 0x00, 0x80],
            &[0x30, 0x84, 0xff, 0xff, 0xff, 0xff],
        ] {
            assert!(ts_request_len(bytes).is_err(), "{bytes:?}");
        }
    }

    #[derive(Debug)]
    struct CorruptKey(Arc<dyn rustls::sign::SigningKey>);

    impl rustls::sign::SigningKey for CorruptKey {
        fn choose_scheme(
            &self,
            offered: &[rustls::SignatureScheme],
        ) -> Option<Box<dyn rustls::sign::Signer>> {
            self.0
                .choose_scheme(offered)
                .map(|signer| Box::new(CorruptSigner(signer)) as Box<dyn rustls::sign::Signer>)
        }
        fn algorithm(&self) -> rustls::SignatureAlgorithm {
            self.0.algorithm()
        }
    }

    #[derive(Debug)]
    struct CorruptSigner(Box<dyn rustls::sign::Signer>);

    impl rustls::sign::Signer for CorruptSigner {
        fn sign(&self, message: &[u8]) -> Result<Vec<u8>, rustls::Error> {
            let mut signature = self.0.sign(message)?;
            *signature.last_mut().unwrap() ^= 1;
            Ok(signature)
        }
        fn scheme(&self) -> rustls::SignatureScheme {
            self.0.scheme()
        }
    }

    fn tls_handshake(
        version: &'static rustls::SupportedProtocolVersion,
        mismatched_key: bool,
        corrupt_signature: bool,
    ) -> Result<(), rustls::Error> {
        use rustls::pki_types::{CertificateDer, PrivateKeyDer, PrivatePkcs8KeyDer, ServerName};
        install_rustls_provider();
        let key = if mismatched_key {
            include_bytes!("testdata/tls-other-key.der").as_slice()
        } else {
            include_bytes!("testdata/tls-key.der").as_slice()
        };
        let signing = rustls::crypto::ring::sign::any_supported_type(&PrivateKeyDer::Pkcs8(
            PrivatePkcs8KeyDer::from(key),
        ))
        .unwrap();
        let signing = if corrupt_signature {
            Arc::new(CorruptKey(signing)) as Arc<dyn rustls::sign::SigningKey>
        } else {
            signing
        };
        let cert = rustls::sign::CertifiedKey::new(
            vec![CertificateDer::from(
                include_bytes!("testdata/tls-cert.der").as_slice(),
            )],
            signing,
        );
        let server_config = rustls::ServerConfig::builder_with_protocol_versions(&[version])
            .with_no_client_auth()
            .with_cert_resolver(Arc::new(rustls::sign::SingleCertAndKey::from(cert)));
        let client_config = rustls::ClientConfig::builder_with_protocol_versions(&[version])
            .dangerous()
            .with_custom_certificate_verifier(Arc::new(NoCertificateVerification))
            .with_no_client_auth();
        let mut server = rustls::ServerConnection::new(Arc::new(server_config)).unwrap();
        let mut client = rustls::ClientConnection::new(
            Arc::new(client_config),
            ServerName::try_from("lgnome-test.invalid").unwrap(),
        )
        .unwrap();
        for _ in 0..16 {
            let mut bytes = Vec::new();
            client.write_tls(&mut bytes).unwrap();
            server.read_tls(&mut io::Cursor::new(bytes)).unwrap();
            server.process_new_packets()?;
            let mut bytes = Vec::new();
            server.write_tls(&mut bytes).unwrap();
            client.read_tls(&mut io::Cursor::new(bytes)).unwrap();
            client.process_new_packets()?;
            if !client.is_handshaking() && !server.is_handshaking() {
                assert_eq!(client.protocol_version(), Some(version.version));
                return Ok(());
            }
        }
        panic!("TLS handshake made no progress");
    }

    #[test]
    fn tls12_and_tls13_accept_self_signed_keys_and_reject_invalid_signatures() {
        for version in [&rustls::version::TLS12, &rustls::version::TLS13] {
            tls_handshake(version, false, false).unwrap();
            assert!(tls_handshake(version, true, false).is_err());
            assert!(tls_handshake(version, false, true).is_err());
        }
    }
    #[test]
    fn socket_rechecks_deadline_and_stop_inside_tls_io() {
        let listener = std::net::TcpListener::bind("127.0.0.1:0").unwrap();
        let socket = TcpStream::connect(listener.local_addr().unwrap()).unwrap();
        let (_peer, _) = listener.accept().unwrap();
        let stop = Arc::new(AtomicBool::new(false));
        let deadline = Arc::new(Mutex::new(Some(Instant::now())));
        let mut socket = PollingSocket {
            socket,
            stop: Arc::clone(&stop),
            phase_deadline: None,
            write_deadline: Arc::clone(&deadline),
        };
        assert_eq!(
            socket.write(&[1]).unwrap_err().kind(),
            io::ErrorKind::TimedOut
        );
        *deadline.lock().unwrap() = None;
        stop.store(true, Ordering::SeqCst);
        assert_eq!(
            socket.write(&[1]).unwrap_err().kind(),
            io::ErrorKind::ConnectionAborted
        );
        assert_eq!(
            socket.read(&mut [0]).unwrap_err().kind(),
            io::ErrorKind::ConnectionAborted
        );
    }
}
