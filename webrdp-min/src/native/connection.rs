//! TCP/TLS/CredSSP bootstrap and RDP connector construction.

use std::io::Write;
use std::net::{SocketAddr, TcpStream, ToSocketAddrs};
use std::sync::Arc;

use ironrdp_connector::{
    ClientConnector, ClientConnectorState, Config, ConnectionResult, Credentials, DesktopSize,
    Sequence as _,
};
use ironrdp_core::WriteBuf;
use ironrdp_displaycontrol::client::DisplayControlClient;
use ironrdp_dvc::DrdynvcClient;
use ironrdp_egfx::client::GraphicsPipelineClient;
use ironrdp_pdu::gcc::KeyboardType;
use ironrdp_pdu::rdp::capability_sets::MajorPlatformType;
use ironrdp_pdu::rdp::client_info::{PerformanceFlags, TimezoneInfo};
use x509_cert::der::Decode as _;

use super::egfx::NativeGfxHandler;
use super::logging::LOG_TARGET_TRANSPORT;
use super::transport::{
    install_rustls_provider, is_timeout, random_array, NoCertificateVerification, TlsStream,
    CONNECT_TIMEOUT, IO_POLL_TIMEOUT, WRITE_TIMEOUT,
};
use super::{rdpeai, rdpecam, rdpedisp, rdpsnd, NativeError, NativeWorker, RdpLogLevel};
use crate::credssp::CredsspClient;

impl NativeWorker {
    pub(super) fn connect_tcp(&mut self) -> Result<Option<TcpStream>, NativeError> {
        let addrs: Vec<SocketAddr> = (self.config.host.as_str(), self.config.port)
            .to_socket_addrs()
            .map_err(|e| {
                NativeError::network(format!(
                    "resolve {}:{}: {e}",
                    self.config.host, self.config.port
                ))
            })?
            .collect();
        if addrs.is_empty() {
            return Err(NativeError::network(format!(
                "resolve {}:{} returned no addresses",
                self.config.host, self.config.port
            )));
        }

        let mut last_error = None;
        for addr in addrs {
            if self.drain_commands() {
                return Ok(None);
            }
            match TcpStream::connect_timeout(&addr, CONNECT_TIMEOUT) {
                Ok(stream) => {
                    stream
                        .set_nodelay(true)
                        .map_err(|e| NativeError::network(format!("set TCP_NODELAY: {e}")))?;
                    stream
                        .set_read_timeout(Some(IO_POLL_TIMEOUT))
                        .map_err(|e| NativeError::network(format!("set read timeout: {e}")))?;
                    stream
                        .set_write_timeout(Some(WRITE_TIMEOUT))
                        .map_err(|e| NativeError::network(format!("set write timeout: {e}")))?;
                    self.callbacks.log(
                        RdpLogLevel::Info,
                        LOG_TARGET_TRANSPORT,
                        format_args!("connected TCP to {addr}"),
                    );
                    return Ok(Some(stream));
                }
                Err(e) => last_error = Some(format!("{addr}: {e}")),
            }
        }

        Err(NativeError::network(format!(
            "TCP connect failed: {}",
            last_error.unwrap_or_else(|| "no attempted address".to_owned())
        )))
    }

    pub(super) fn new_connector(&self, client_addr: SocketAddr) -> ClientConnector {
        let config = Config {
            credentials: Credentials::UsernamePassword {
                username: self.config.username.clone(),
                password: self.config.password.clone(),
            },
            domain: if self.config.domain.is_empty() {
                None
            } else {
                Some(self.config.domain.clone())
            },
            enable_tls: true,
            enable_credssp: true,
            keyboard_type: KeyboardType::IbmEnhanced,
            keyboard_subtype: 0,
            keyboard_layout: 0,
            keyboard_functional_keys_count: 12,
            ime_file_name: String::new(),
            dig_product_id: String::new(),
            desktop_size: DesktopSize {
                width: self.config.width,
                height: self.config.height,
            },
            bitmap: None,
            client_build: 0,
            client_name: "gnomecast-native".to_owned(),
            client_dir: "C:\\Windows\\System32\\mstscax.dll".to_owned(),
            platform: MajorPlatformType::UNSPECIFIED,
            compression_type: None,
            // The server ships cursor shapes as pointer updates (gnome-remote-desktop never
            // embeds them into the video); they are forwarded to the C shell for rendering.
            enable_server_pointer: true,
            autologon: false,
            // Clears the NO_AUDIO_PLAYBACK client-info flag; without this the server
            // never streams audio regardless of the AUDIO_PLAYBACK_DVC channel below.
            enable_audio_playback: true,
            // MS-RDPEAI is additionally gated by INFO_AUDIOCAPTURE in Client Info.
            enable_audio_capture: self.config.enable_audio_input,
            request_data: None,
            pointer_software_rendering: false,
            multitransport_flags: None,
            performance_flags: PerformanceFlags::ENABLE_FONT_SMOOTHING
                | PerformanceFlags::ENABLE_DESKTOP_COMPOSITION,
            desktop_scale_factor: 0,
            hardware_id: None,
            license_cache: None,
            timezone_info: TimezoneInfo::default(),
            alternate_shell: String::new(),
            work_dir: String::new(),
        };

        let mut connector = ClientConnector::new(config, client_addr);
        let mut drdynvc = DrdynvcClient::new()
            .with_dynamic_channel(GraphicsPipelineClient::new(
                Box::new(NativeGfxHandler {
                    shared: Arc::clone(&self.gfx),
                }),
                None,
            ))
            .with_dynamic_channel(rdpsnd::RdpsndDvcHandler::new(
                self.callbacks,
                self.config.prefer_pcm_audio,
            ))
            .with_typed_listener::<DisplayControlClient, _>(rdpedisp::DisplayControlFactory {
                width: self.config.width,
                height: self.config.height,
                sink: self.callbacks,
            });
        if self.config.enable_camera {
            drdynvc = drdynvc
                .with_dynamic_channel(rdpecam::CameraEnumerator::new(self.camera.clone()))
                .with_typed_listener::<rdpecam::CameraDevice, _>(
                    rdpecam::CameraDeviceFactory::new(self.camera.clone()),
                );
        }
        if self.config.enable_audio_input {
            drdynvc = drdynvc
                .with_dynamic_channel(rdpeai::AudioInputHandler::new(self.audio_input.clone()));
        }
        connector.attach_static_channel(drdynvc);
        connector
    }

    pub(super) fn send_x224_request(
        &mut self,
        tcp: &mut TcpStream,
        connector: &mut ClientConnector,
    ) -> Result<(), NativeError> {
        let mut out = WriteBuf::new();
        connector
            .step_no_input(&mut out)
            .map_err(|e| NativeError::protocol(format!("X.224 negotiation request: {e}")))?;
        self.write_all(tcp, out.filled(), "X.224 negotiation request")?;

        let confirm = self.read_connector_pdu(tcp, connector, "X.224 negotiation response")?;
        let mut out = WriteBuf::new();
        connector
            .step(&confirm, &mut out)
            .map_err(|e| NativeError::protocol(format!("X.224 negotiation response: {e}")))?;
        if !out.filled().is_empty() {
            self.write_all(tcp, out.filled(), "X.224 negotiation follow-up")?;
        }
        if !connector.should_perform_security_upgrade() {
            return Err(NativeError::protocol(
                "server did not select enhanced TLS security",
            ));
        }
        Ok(())
    }

    pub(super) fn upgrade_tls(
        &mut self,
        tcp: TcpStream,
    ) -> Result<(TlsStream, Vec<u8>), NativeError> {
        install_rustls_provider();
        let mut config = rustls::ClientConfig::builder()
            .dangerous()
            .with_custom_certificate_verifier(Arc::new(NoCertificateVerification))
            .with_no_client_auth();
        config.key_log = Arc::new(rustls::KeyLogFile::new());
        config.resumption = rustls::client::Resumption::disabled();

        let server_name = rustls::pki_types::ServerName::try_from(self.config.host.clone())
            .map_err(|e| {
                NativeError::network(format!("invalid TLS server name {}: {e}", self.config.host))
            })?;
        let conn = rustls::ClientConnection::new(Arc::new(config), server_name)
            .map_err(|e| NativeError::network(format!("create TLS client: {e}")))?;
        let mut tls = rustls::StreamOwned::new(conn, tcp);

        while tls.conn.is_handshaking() {
            if self.drain_commands() {
                return Err(NativeError::network("TLS handshake stopped"));
            }
            match tls.conn.complete_io(&mut tls.sock) {
                Ok(_) => {}
                Err(e) if is_timeout(&e) => {}
                Err(e) => return Err(NativeError::network(format!("TLS handshake: {e}"))),
            }
        }
        tls.flush()
            .map_err(|e| NativeError::network(format!("TLS flush after handshake: {e}")))?;

        let cert = tls
            .conn
            .peer_certificates()
            .and_then(|certs| certs.first())
            .ok_or_else(|| NativeError::network("TLS peer certificate is missing"))?;
        let parsed = x509_cert::Certificate::from_der(cert.as_ref())
            .map_err(|e| NativeError::network(format!("TLS peer certificate parse: {e}")))?;
        let public_key = parsed
            .tbs_certificate
            .subject_public_key_info
            .subject_public_key
            .as_bytes()
            .ok_or_else(|| NativeError::network("TLS subject public key is not byte-aligned"))?
            .to_vec();
        Ok((tls, public_key))
    }

    pub(super) fn run_credssp(
        &mut self,
        tls: &mut TlsStream,
        connector: &mut ClientConnector,
        public_key: Vec<u8>,
    ) -> Result<(), NativeError> {
        let client_challenge = random_array::<8>()?;
        let key_exch_key = random_array::<16>()?;
        let client_nonce = random_array::<32>()?;
        let mut credssp = CredsspClient::new(
            public_key,
            &self.config.username,
            &self.config.domain,
            &self.config.password,
            client_challenge,
            key_exch_key,
            client_nonce,
        );

        let first = credssp.first_message();
        self.write_all(tls, &first, "CredSSP negotiate")?;

        while !credssp.is_done() {
            let server_ts = self.read_ts_request(tls)?;
            let (next, done) = credssp
                .process_server(&server_ts)
                .map_err(|e| NativeError::protocol(format!("CredSSP: {e}")))?;
            self.write_all(tls, &next, "CredSSP response")?;
            if done {
                connector.mark_credssp_as_done();
                return Ok(());
            }
        }
        Ok(())
    }

    pub(super) fn pump_connector(
        &mut self,
        tls: &mut TlsStream,
        mut connector: ClientConnector,
    ) -> Result<ConnectionResult, NativeError> {
        loop {
            if self.drain_commands() {
                return Err(NativeError::network("stopped before ActiveStage"));
            }
            if matches!(connector.state, ClientConnectorState::Connected { .. }) {
                break;
            }

            let mut out = WriteBuf::new();
            if connector.next_pdu_hint().is_some() {
                let pdu = self.read_connector_pdu(tls, &connector, "connector input")?;
                connector
                    .step(&pdu, &mut out)
                    .map_err(|e| NativeError::protocol(format!("connector step: {e}")))?;
            } else {
                connector
                    .step_no_input(&mut out)
                    .map_err(|e| NativeError::protocol(format!("connector step_no_input: {e}")))?;
            }
            if !out.filled().is_empty() {
                self.write_all(tls, out.filled(), "connector output")?;
            }
        }

        match connector.state {
            ClientConnectorState::Connected { result } => Ok(result),
            other => Err(NativeError::protocol(format!(
                "connector ended in unexpected state {other:?}"
            ))),
        }
    }
}
