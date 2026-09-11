use ironrdp_connector::{ConnectorError, ConnectorErrorKind};
use ironrdp_pdu::rdp::server_error_info::{ErrorInfo, ProtocolIndependentCode};
use ironrdp_session::GracefulDisconnectReason;

use super::{NativeError, RdpDisconnectReason, RdpState};

impl NativeError {
    pub(super) fn should_retry(&self, attempt: u32, stopping: bool) -> bool {
        self.retry_handoff && !stopping && attempt < super::MAX_SESSION_ATTEMPTS
    }

    pub(super) fn disconnect(reason: GracefulDisconnectReason) -> Self {
        match reason {
            GracefulDisconnectReason::ErrorInfo(info) => Self::server_error_info(info),
            other => Self {
                state: RdpState::Disconnected,
                reason: RdpDisconnectReason::PeerDisconnected,
                retry_handoff: matches!(
                    other,
                    GracefulDisconnectReason::UserInitiated
                        | GracefulDisconnectReason::ServerInitiated
                ),
                message: format!("server disconnected: {}", other.description()),
            },
        }
    }

    pub(super) fn connector(error: ConnectorError) -> Self {
        match error.kind() {
            ConnectorErrorKind::ServerErrorInfo(info) => Self::server_error_info(*info),
            ConnectorErrorKind::AccessDenied => Self {
                reason: RdpDisconnectReason::AccessDenied,
                ..Self::protocol(error.to_string())
            },
            _ => Self::protocol(error.to_string()),
        }
    }

    fn server_error_info(info: ErrorInfo) -> Self {
        use ProtocolIndependentCode as Code;
        use RdpDisconnectReason as Reason;
        let (reason, normal) = match info {
            ErrorInfo::ProtocolIndependentCode(code) => match code {
                Code::RpcInitiatedDisconnect => (Reason::AdminDisconnect, true),
                Code::RpcInitiatedLogoff => (Reason::AdminLogoff, true),
                Code::IdleTimeout => (Reason::IdleTimeout, true),
                Code::LogonTimeout => (Reason::SessionTimeout, true),
                Code::DisconnectedByOtherconnection => (Reason::SessionReplaced, true),
                Code::RpcInitiatedDisconnectByuser => (Reason::UserDisconnect, true),
                Code::LogoffByUser => (Reason::UserLogoff, true),
                Code::ServerShutdown => (Reason::ServerShutdown, true),
                Code::ServerReboot => (Reason::ServerReboot, true),
                Code::ServerDeniedConnection
                | Code::ServerInsufficientPrivileges
                | Code::ServerFreshCredentialsRequired => (Reason::AccessDenied, false),
                _ => (Reason::ServerError, false),
            },
            ErrorInfo::ProtocolIndependentLicensingCode(_) => (Reason::LicenseError, false),
            ErrorInfo::ProtocolIndependentConnectionBrokerCode(_) => (Reason::BrokerError, false),
            ErrorInfo::RdpSpecificCode(_) => (Reason::ServerError, false),
        };
        Self {
            state: if normal {
                RdpState::Disconnected
            } else {
                RdpState::ProtocolError
            },
            reason,
            retry_handoff: matches!(
                info,
                ErrorInfo::ProtocolIndependentCode(Code::RpcInitiatedDisconnect)
            ),
            message: format!("server error info {info:?}: {}", info.description()),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn explicit_session_end_never_retries_or_becomes_protocol_error() {
        use ProtocolIndependentCode as Code;
        use RdpDisconnectReason as Reason;
        for (code, reason) in [
            (Code::RpcInitiatedLogoff, Reason::AdminLogoff),
            (Code::IdleTimeout, Reason::IdleTimeout),
            (Code::LogonTimeout, Reason::SessionTimeout),
            (Code::DisconnectedByOtherconnection, Reason::SessionReplaced),
            (Code::RpcInitiatedDisconnectByuser, Reason::UserDisconnect),
            (Code::LogoffByUser, Reason::UserLogoff),
            (Code::ServerShutdown, Reason::ServerShutdown),
            (Code::ServerReboot, Reason::ServerReboot),
        ] {
            let info = ErrorInfo::ProtocolIndependentCode(code);
            for result in [
                NativeError::disconnect(GracefulDisconnectReason::ErrorInfo(info)),
                NativeError::connector(ConnectorError::new(
                    "activation",
                    ConnectorErrorKind::ServerErrorInfo(info),
                )),
            ] {
                assert_eq!(result.state, RdpState::Disconnected);
                assert_eq!(result.reason, reason);
                assert!(!result.retry_handoff);
            }
        }
    }

    #[test]
    fn rpc_handoff_retries_during_activation_and_active_session() {
        let info =
            ErrorInfo::ProtocolIndependentCode(ProtocolIndependentCode::RpcInitiatedDisconnect);
        for result in [
            NativeError::disconnect(GracefulDisconnectReason::ErrorInfo(info)),
            NativeError::connector(ConnectorError::new(
                "activation",
                ConnectorErrorKind::ServerErrorInfo(info),
            )),
        ] {
            assert_eq!(result.state, RdpState::Disconnected);
            assert_eq!(result.reason, RdpDisconnectReason::AdminDisconnect);
            assert!(result.should_retry(1, false));
            assert!(result.should_retry(2, false));
            assert!(!result.should_retry(3, false));
            assert!(!result.should_retry(1, true));
        }
    }

    #[test]
    fn only_ambiguous_mcs_handoffs_retry() {
        for reason in [
            GracefulDisconnectReason::UserInitiated,
            GracefulDisconnectReason::ServerInitiated,
        ] {
            let result = NativeError::disconnect(reason);
            assert!(result.retry_handoff);
            assert!(result.should_retry(1, false));
            assert!(result.should_retry(2, false));
            assert!(!result.should_retry(3, false));
            assert!(!result.should_retry(1, true));
            assert_eq!(result.reason, RdpDisconnectReason::PeerDisconnected);
        }
        assert!(
            !NativeError::disconnect(GracefulDisconnectReason::Other("channel purged".into()))
                .retry_handoff
        );
        for text in [
            "RDP server closed the TLS stream",
            "Broken pipe",
            "disconnect provider ultimatum",
        ] {
            let result = NativeError::network(text);
            assert_eq!(result.reason, RdpDisconnectReason::None);
            assert!(!result.retry_handoff);
        }
    }

    #[test]
    fn server_failures_remain_errors() {
        for code in [
            ProtocolIndependentCode::ServerDwmCrash,
            ProtocolIndependentCode::OutOfMemory,
            ProtocolIndependentCode::ServerDeniedConnection,
        ] {
            let result = NativeError::server_error_info(ErrorInfo::ProtocolIndependentCode(code));
            assert_eq!(result.state, RdpState::ProtocolError);
            assert!(!result.retry_handoff);
        }
    }
}
