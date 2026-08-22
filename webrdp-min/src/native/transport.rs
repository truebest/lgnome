//! Transport primitives: socket timeouts, TLS plumbing, and byte-level
//! helpers shared by the connect/CredSSP/session read paths.
//!
//! Certificate verification is deliberately disabled: the client pins nothing
//! and talks to self-signed gnome-remote-desktop endpoints on a LAN; CredSSP
//! binds the channel to the presented key instead.

#![forbid(unsafe_code)]

use std::io;
use std::net::TcpStream;
use std::time::Duration;

use super::NativeError;

pub(super) const IO_POLL_TIMEOUT: Duration = Duration::from_millis(10);
pub(super) const CONNECT_TIMEOUT: Duration = Duration::from_secs(5);
pub(super) const WRITE_TIMEOUT: Duration = Duration::from_secs(5);

/// The one owned TLS stream type every read/write path shares.
pub(super) type TlsStream = rustls::StreamOwned<rustls::ClientConnection, TcpStream>;

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

pub(super) fn ts_request_len(buf: &[u8]) -> Option<usize> {
    if buf.len() < 2 || buf[0] != 0x30 {
        return None;
    }
    let b1 = buf[1];
    if b1 < 0x80 {
        return Some(2 + b1 as usize);
    }
    let n = (b1 & 0x7f) as usize;
    if n == 0 || n > 4 || buf.len() < 2 + n {
        return None;
    }
    let mut len = 0usize;
    for i in 0..n {
        len = (len << 8) | buf[2 + i] as usize;
    }
    Some(2 + n + len)
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
        _: &[u8],
        _: &rustls::pki_types::CertificateDer<'_>,
        _: &rustls::DigitallySignedStruct,
    ) -> Result<rustls::client::danger::HandshakeSignatureValid, rustls::Error> {
        Ok(rustls::client::danger::HandshakeSignatureValid::assertion())
    }

    fn verify_tls13_signature(
        &self,
        _: &[u8],
        _: &rustls::pki_types::CertificateDer<'_>,
        _: &rustls::DigitallySignedStruct,
    ) -> Result<rustls::client::danger::HandshakeSignatureValid, rustls::Error> {
        Ok(rustls::client::danger::HandshakeSignatureValid::assertion())
    }

    fn supported_verify_schemes(&self) -> Vec<rustls::SignatureScheme> {
        vec![
            rustls::SignatureScheme::RSA_PKCS1_SHA1,
            rustls::SignatureScheme::ECDSA_SHA1_Legacy,
            rustls::SignatureScheme::RSA_PKCS1_SHA256,
            rustls::SignatureScheme::ECDSA_NISTP256_SHA256,
            rustls::SignatureScheme::RSA_PKCS1_SHA384,
            rustls::SignatureScheme::ECDSA_NISTP384_SHA384,
            rustls::SignatureScheme::RSA_PKCS1_SHA512,
            rustls::SignatureScheme::ECDSA_NISTP521_SHA512,
            rustls::SignatureScheme::RSA_PSS_SHA256,
            rustls::SignatureScheme::RSA_PSS_SHA384,
            rustls::SignatureScheme::RSA_PSS_SHA512,
            rustls::SignatureScheme::ED25519,
            rustls::SignatureScheme::ED448,
        ]
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
}
