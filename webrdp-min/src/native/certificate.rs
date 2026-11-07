//! Server identity checks and cancellable user consent, before any CredSSP traffic.

use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Condvar, Mutex};
use std::time::Duration;

use rustls::client::danger::ServerCertVerifier;
use rustls::pki_types::{CertificateDer, ServerName, UnixTime};

use super::{NativeError, RdpDisconnectReason, RdpState};

pub(super) fn fingerprint(cert: &[u8]) -> String {
    super::transport::hex_prefix(&crate::crypto::sha256(cert), 32)
}

pub(super) fn valid_fingerprint(value: &str) -> bool {
    value.len() == 64 && value.bytes().all(|b| b.is_ascii_hexdigit())
}

pub(super) fn warning(
    chain: &[CertificateDer<'_>],
    host: &str,
    trusted: &str,
    verifier: &dyn ServerCertVerifier,
    now: UnixTime,
) -> Result<Option<String>, NativeError> {
    let cert = chain
        .first()
        .ok_or_else(|| NativeError::protocol("missing server certificate"))?;
    if !trusted.is_empty() {
        return Ok((!trusted.eq_ignore_ascii_case(&fingerprint(cert)))
            .then(|| "The server certificate has changed since you last trusted it.".to_owned()));
    }
    let name = ServerName::try_from(host)
        .map_err(|_| NativeError::protocol("invalid certificate server name"))?;
    Ok(verifier
        .verify_server_cert(cert, &chain[1..], &name, &[], now)
        .err()
        .map(|error| match error {
            rustls::Error::InvalidCertificate(rustls::CertificateError::UnknownIssuer) => {
                "The certificate was not issued by a trusted authority.".to_owned()
            }
            rustls::Error::InvalidCertificate(rustls::CertificateError::NotValidForName)
            | rustls::Error::InvalidCertificate(
                rustls::CertificateError::NotValidForNameContext { .. },
            ) => "The certificate does not match the server address.".to_owned(),
            rustls::Error::InvalidCertificate(rustls::CertificateError::Expired)
            | rustls::Error::InvalidCertificate(rustls::CertificateError::ExpiredContext {
                ..
            }) => "The server certificate has expired.".to_owned(),
            _ => "The server certificate could not be verified.".to_owned(),
        }))
}

#[derive(Default)]
struct Decision {
    fingerprint: String,
    accepted: Option<bool>,
}

#[derive(Default)]
pub(super) struct CertificateGate {
    decision: Mutex<Decision>,
    changed: Condvar,
}

impl CertificateGate {
    pub(super) fn confirm(&self, fingerprint: &str, accept: bool) -> bool {
        let Ok(mut decision) = self.decision.lock() else {
            return false;
        };
        if decision.fingerprint.is_empty()
            || decision.fingerprint != fingerprint
            || decision.accepted.is_some()
        {
            return false;
        }
        decision.accepted = Some(accept);
        self.changed.notify_one();
        true
    }

    pub(super) fn wait(
        &self,
        fingerprint: &str,
        stop: &AtomicBool,
        notify: impl FnOnce() -> bool,
    ) -> Result<(), NativeError> {
        {
            let mut decision = self
                .decision
                .lock()
                .map_err(|_| NativeError::protocol("certificate decision lock failed"))?;
            *decision = Decision {
                fingerprint: fingerprint.to_owned(),
                accepted: None,
            };
        }
        // Publish the request before invoking C: an immediate response is valid.
        let available = notify();
        let mut decision = self
            .decision
            .lock()
            .map_err(|_| NativeError::protocol("certificate decision lock failed"))?;
        while available && decision.accepted.is_none() && !stop.load(Ordering::SeqCst) {
            (decision, _) = self
                .changed
                .wait_timeout(decision, Duration::from_millis(50))
                .map_err(|_| NativeError::protocol("certificate decision wait failed"))?;
        }
        let accepted = available && decision.accepted == Some(true) && !stop.load(Ordering::SeqCst);
        *decision = Decision::default();
        if accepted {
            return Ok(());
        }
        let mut error = NativeError::with_state(
            RdpState::Disconnected,
            "Server certificate was not accepted.",
        );
        error.reason = RdpDisconnectReason::UserDisconnect;
        Err(error)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::Arc;

    fn verifier(trusted: bool) -> Arc<rustls::client::WebPkiServerVerifier> {
        super::super::transport::install_rustls_provider();
        let mut roots = rustls::RootCertStore {
            roots: webpki_roots::TLS_SERVER_ROOTS.to_vec(),
        };
        if trusted {
            roots
                .add(CertificateDer::from(
                    include_bytes!("testdata/identity-ca.der").as_slice(),
                ))
                .unwrap();
        }
        rustls::client::WebPkiServerVerifier::builder(Arc::new(roots))
            .build()
            .unwrap()
    }

    #[test]
    fn pki_checks_name_date_and_issuer_and_pins_detect_replacement() {
        let chain = [CertificateDer::from(
            include_bytes!("testdata/identity-cert.der").as_slice(),
        )];
        let now = UnixTime::since_unix_epoch(Duration::from_secs(1_800_000_000));
        let trusted = verifier(true);
        assert!(
            warning(&chain, "lgnome-test.invalid", "", trusted.as_ref(), now)
                .unwrap()
                .is_none()
        );
        assert!(warning(&chain, "127.0.0.1", "", trusted.as_ref(), now)
            .unwrap()
            .is_none());
        assert!(warning(&chain, "wrong.invalid", "", trusted.as_ref(), now)
            .unwrap()
            .is_some());
        assert!(warning(
            &chain,
            "lgnome-test.invalid",
            "",
            verifier(false).as_ref(),
            now
        )
        .unwrap()
        .is_some());
        let expired = UnixTime::since_unix_epoch(Duration::from_secs(2_200_000_000));
        assert!(
            warning(&chain, "lgnome-test.invalid", "", trusted.as_ref(), expired)
                .unwrap()
                .is_some()
        );
        let pin = fingerprint(chain[0].as_ref());
        assert!(valid_fingerprint(&pin));
        assert!(
            warning(&chain, "127.0.0.1", &pin, verifier(false).as_ref(), now)
                .unwrap()
                .is_none()
        );
        assert!(warning(
            &chain,
            "lgnome-test.invalid",
            &"0".repeat(64),
            trusted.as_ref(),
            now
        )
        .unwrap()
        .unwrap()
        .contains("changed"));
        assert!(warning(&[], "test", "", trusted.as_ref(), now).is_err());
        assert!(!valid_fingerprint("always trust"));
    }

    #[test]
    fn consent_rejects_stale_replies_and_defaults_to_rejection() {
        let gate = CertificateGate::default();
        let stop = AtomicBool::new(false);
        assert!(!gate.confirm("fingerprint", true));
        assert!(gate.wait("fingerprint", &stop, || false).is_err());
        assert!(gate
            .wait("fingerprint", &stop, || {
                assert!(!gate.confirm("other", true));
                assert!(gate.confirm("fingerprint", false));
                true
            })
            .is_err());
        assert!(gate
            .wait("fingerprint", &stop, || {
                assert!(gate.confirm("fingerprint", true));
                assert!(!gate.confirm("fingerprint", true));
                true
            })
            .is_ok());
        assert!(!gate.confirm("fingerprint", true));
    }

    #[test]
    fn stopping_a_waiting_connection_does_not_require_ui() {
        let gate = Arc::new(CertificateGate::default());
        let stop = Arc::new(AtomicBool::new(false));
        let (tx, rx) = std::sync::mpsc::channel();
        let worker_gate = Arc::clone(&gate);
        let worker_stop = Arc::clone(&stop);
        let worker = std::thread::spawn(move || {
            worker_gate.wait("fingerprint", &worker_stop, || {
                tx.send(()).unwrap();
                true
            })
        });
        rx.recv_timeout(Duration::from_secs(2)).unwrap();
        stop.store(true, Ordering::SeqCst);
        assert!(worker.join().unwrap().is_err());
        assert!(!gate.confirm("fingerprint", true));
    }

    #[test]
    fn tls_waits_for_consent_and_rechecks_the_pin_on_a_fresh_connection() {
        use std::ffi::{c_char, c_void, CStr};
        use std::io::{Read, Write};
        use std::net::{TcpListener, TcpStream};
        use std::sync::mpsc;

        extern "C" fn requested(ctx: *mut c_void, sha256: *const c_char, _: *const c_char) {
            // Both pointers belong to the worker for the duration of this callback.
            let sender = unsafe { &*ctx.cast::<mpsc::Sender<String>>() };
            let pin = unsafe { CStr::from_ptr(sha256) }.to_str().unwrap();
            sender.send(pin.to_owned()).unwrap();
        }

        for accept in [false, true] {
            super::super::transport::install_rustls_provider();
            let listener = TcpListener::bind("127.0.0.1:0").unwrap();
            let address = listener.local_addr().unwrap();
            let (quiet_tx, quiet_rx) = mpsc::channel();
            let server = std::thread::spawn(move || {
                use rustls::pki_types::{PrivateKeyDer, PrivatePkcs8KeyDer};
                let config = Arc::new(
                    rustls::ServerConfig::builder()
                        .with_no_client_auth()
                        .with_single_cert(
                            vec![CertificateDer::from(
                                include_bytes!("testdata/identity-cert.der").as_slice(),
                            )],
                            PrivateKeyDer::Pkcs8(PrivatePkcs8KeyDer::from(
                                include_bytes!("testdata/identity-key.der").as_slice(),
                            )),
                        )
                        .unwrap(),
                );
                let (socket, _) = listener.accept().unwrap();
                socket
                    .set_read_timeout(Some(Duration::from_secs(2)))
                    .unwrap();
                let mut tls = rustls::StreamOwned::new(
                    rustls::ServerConnection::new(Arc::clone(&config)).unwrap(),
                    socket,
                );
                while tls.conn.is_handshaking() {
                    tls.conn.complete_io(&mut tls.sock).unwrap();
                }
                tls.sock
                    .set_read_timeout(Some(Duration::from_millis(100)))
                    .unwrap();
                let mut byte = [0];
                let error = tls.read(&mut byte).unwrap_err();
                assert!(super::super::transport::is_timeout(&error));
                quiet_tx.send(()).unwrap();
                tls.sock
                    .set_read_timeout(Some(Duration::from_secs(2)))
                    .unwrap();
                assert!(!matches!(tls.read(&mut byte), Ok(1)));
                if accept {
                    let (socket, _) = listener.accept().unwrap();
                    socket
                        .set_read_timeout(Some(Duration::from_secs(2)))
                        .unwrap();
                    let mut tls = rustls::StreamOwned::new(
                        rustls::ServerConnection::new(config).unwrap(),
                        socket,
                    );
                    tls.read_exact(&mut byte).unwrap();
                    assert_eq!(byte, [42]);
                }
            });
            let gate = Arc::new(CertificateGate::default());
            let worker_gate = Arc::clone(&gate);
            let (request_tx, request_rx) = mpsc::channel::<String>();
            let worker = std::thread::spawn(move || {
                let (mut worker, _commands) = super::super::tests::test_worker();
                worker.config.host = "127.0.0.1".to_owned();
                worker.config.certificate_gate = worker_gate;
                worker.callbacks.callbacks.on_certificate = Some(requested);
                worker.callbacks.callbacks.ctx = (&request_tx as *const mpsc::Sender<String>)
                    .cast_mut()
                    .cast();
                let result = worker.upgrade_tls(TcpStream::connect(address).unwrap());
                let Err(error) = result else {
                    panic!("unapproved TLS stream escaped")
                };
                assert_eq!(error.retry_certificate, accept);
                if accept {
                    assert!(!worker.config.trusted_certificate_sha256.is_empty());
                    let (mut tls, _) = worker
                        .upgrade_tls(TcpStream::connect(address).unwrap())
                        .unwrap();
                    tls.write_all(&[42]).unwrap();
                    tls.flush().unwrap();
                }
            });
            let pin = request_rx.recv_timeout(Duration::from_secs(2)).unwrap();
            quiet_rx.recv_timeout(Duration::from_secs(2)).unwrap();
            assert!(gate.confirm(&pin, accept));
            worker.join().unwrap();
            server.join().unwrap();
            assert!(request_rx.try_recv().is_err());
        }
    }
}
