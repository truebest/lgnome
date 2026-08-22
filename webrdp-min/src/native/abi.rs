//! Data-only C ABI contract shared by the inbound FFI entry points and the
//! outbound callback sink. Keep this module matched 1:1 with
//! `native/include/rdp_ffi.h`.

use std::ffi::c_char;

/// Values shared with the `RdpAudioCodec` enum in `native/include/rdp_ffi.h`.
pub const RDP_AUDIO_CODEC_OPUS: u32 = 1;
pub const RDP_AUDIO_CODEC_PCM_S16LE: u32 = 2;

#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum RdpState {
    Idle = 0,
    Connecting = 1,
    Tls = 2,
    Credssp = 3,
    Active = 4,
    NoAvc420 = 5,
    DecoderError = 6,
    NetworkError = 7,
    ProtocolError = 8,
    Stopped = 9,
}

/// Values shared with the `RdpLogLevel` enum in `native/include/rdp_ffi.h`.
#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum RdpLogLevel {
    Trace = 0,
    Debug = 1,
    Info = 2,
    Notice = 3,
    Warning = 4,
    Error = 5,
    Fatal = 6,
}

#[repr(C)]
pub struct RdpConfig {
    pub host: *const c_char,
    pub port: u16,
    pub username: *const c_char,
    pub password: *const c_char,
    pub domain: *const c_char,
    pub width: u16,
    pub height: u16,
    pub fps: u16,
    /// Non-zero: advertise only PCM to rdpsnd for a lossless stream (grd would otherwise
    /// pick Opus when both are offered). Fits in the struct's tail padding on both ABIs.
    pub prefer_pcm_audio: u8,
    pub enable_camera: u8,
    pub enable_audio_input: u8,
    pub reserved0: u8,
    pub camera_width: u16,
    pub camera_height: u16,
    pub camera_fps: u16,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct RdpCallbacks {
    pub ctx: *mut core::ffi::c_void,
    pub on_state: Option<extern "C" fn(*mut core::ffi::c_void, RdpState, *const c_char)>,
    pub on_log_enabled:
        Option<extern "C" fn(*mut core::ffi::c_void, RdpLogLevel, *const c_char) -> bool>,
    pub on_log:
        Option<extern "C" fn(*mut core::ffi::c_void, RdpLogLevel, *const c_char, *const c_char)>,
    pub on_desktop_size: Option<extern "C" fn(*mut core::ffi::c_void, u16, u16)>,
    /// Raw AVC420 access unit. C classifies AVC/Annex-B framing and IDR state from
    /// these bytes before any snapshot or decoder-ownership decision.
    pub on_video_au: Option<extern "C" fn(*mut core::ffi::c_void, *const u8, usize, u64)>,
    pub on_bitmap_update: Option<
        extern "C" fn(*mut core::ffi::c_void, u16, u32, u32, u32, u32, u32, *const u8, usize),
    >,
    /// (ctx, codec: RDP_AUDIO_CODEC_*, sample_rate, channels). Fired before the first
    /// on_audio_data of a stream and again whenever the negotiated format changes.
    pub on_audio_format: Option<extern "C" fn(*mut core::ffi::c_void, u32, u32, u16)>,
    /// (ctx, data, len, audio_timestamp_ms). One encoded packet (Opus) or PCM chunk per
    /// call; bytes are valid only for the duration of the call.
    pub on_audio_data: Option<extern "C" fn(*mut core::ffi::c_void, *const u8, usize, u32)>,
    /// (ctx, width, height, hotspot_x, hotspot_y, rgba, len). Decoded server cursor shape:
    /// RGBA byte order, top-down rows, tight stride (width * 4), straight (non-premultiplied)
    /// alpha. Bytes are valid only for the duration of the call.
    pub on_pointer_bitmap:
        Option<extern "C" fn(*mut core::ffi::c_void, u16, u16, u16, u16, *const u8, usize)>,
    /// (ctx, x, y). Server-initiated pointer warp, in desktop coordinates.
    pub on_pointer_position: Option<extern "C" fn(*mut core::ffi::c_void, u16, u16)>,
    /// (ctx, state: RDP_POINTER_STATE_*). 0 = hidden, 1 = system default arrow.
    pub on_pointer_state: Option<extern "C" fn(*mut core::ffi::c_void, u32)>,
    /// Camera stream ownership callbacks. `generation` rejects late frames after stop/restart.
    pub on_camera_start: Option<extern "C" fn(*mut core::ffi::c_void, u64, u16, u16, u16)>,
    pub on_camera_stop: Option<extern "C" fn(*mut core::ffi::c_void, u64)>,
    pub on_camera_sample_request: Option<extern "C" fn(*mut core::ffi::c_void, u64)>,
    /// Negotiated PCM capture packet shape for MS-RDPEAI.
    pub on_audio_input_start: Option<extern "C" fn(*mut core::ffi::c_void, u64, u32, u16, u32)>,
    pub on_audio_input_stop: Option<extern "C" fn(*mut core::ffi::c_void, u64)>,
}

pub const RDP_POINTER_STATE_HIDDEN: u32 = 0;
pub const RDP_POINTER_STATE_DEFAULT: u32 = 1;

#[cfg(test)]
mod tests {
    use super::*;
    use std::mem::{align_of, offset_of, size_of};

    #[test]
    fn state_values_match_header() {
        assert_eq!(RdpState::Idle as u32, 0);
        assert_eq!(RdpState::Connecting as u32, 1);
        assert_eq!(RdpState::Tls as u32, 2);
        assert_eq!(RdpState::Credssp as u32, 3);
        assert_eq!(RdpState::Active as u32, 4);
        assert_eq!(RdpState::NoAvc420 as u32, 5);
        assert_eq!(RdpState::DecoderError as u32, 6);
        assert_eq!(RdpState::NetworkError as u32, 7);
        assert_eq!(RdpState::ProtocolError as u32, 8);
        assert_eq!(RdpState::Stopped as u32, 9);
        assert_eq!(size_of::<RdpState>(), size_of::<u32>());
    }

    #[test]
    fn log_level_values_match_header() {
        assert_eq!(RdpLogLevel::Trace as u32, 0);
        assert_eq!(RdpLogLevel::Debug as u32, 1);
        assert_eq!(RdpLogLevel::Info as u32, 2);
        assert_eq!(RdpLogLevel::Notice as u32, 3);
        assert_eq!(RdpLogLevel::Warning as u32, 4);
        assert_eq!(RdpLogLevel::Error as u32, 5);
        assert_eq!(RdpLogLevel::Fatal as u32, 6);
        assert_eq!(size_of::<RdpLogLevel>(), size_of::<u32>());
    }

    #[test]
    fn ffi_struct_layout_is_c_compatible() {
        assert_eq!(align_of::<RdpConfig>(), align_of::<*const c_char>());
        assert_eq!(offset_of!(RdpConfig, host), 0);
        assert_eq!(offset_of!(RdpConfig, port), size_of::<*const c_char>());
        assert!(offset_of!(RdpConfig, username) > offset_of!(RdpConfig, port));
        assert!(offset_of!(RdpConfig, password) > offset_of!(RdpConfig, username));
        assert!(offset_of!(RdpConfig, domain) > offset_of!(RdpConfig, password));
        assert!(offset_of!(RdpConfig, width) > offset_of!(RdpConfig, domain));
        assert!(offset_of!(RdpConfig, prefer_pcm_audio) > offset_of!(RdpConfig, fps));
        assert!(offset_of!(RdpConfig, enable_camera) > offset_of!(RdpConfig, prefer_pcm_audio));
        assert!(offset_of!(RdpConfig, enable_audio_input) > offset_of!(RdpConfig, enable_camera));
        assert!(offset_of!(RdpConfig, camera_width) > offset_of!(RdpConfig, reserved0));
        assert!(offset_of!(RdpConfig, camera_height) > offset_of!(RdpConfig, camera_width));
        assert!(offset_of!(RdpConfig, camera_fps) > offset_of!(RdpConfig, camera_height));
        assert_eq!(size_of::<RdpConfig>(), 5 * size_of::<*const c_char>() + 16);
        assert_eq!(
            align_of::<RdpCallbacks>(),
            align_of::<*mut core::ffi::c_void>()
        );
        assert_eq!(offset_of!(RdpCallbacks, ctx), 0);
        assert!(offset_of!(RdpCallbacks, on_state) > offset_of!(RdpCallbacks, ctx));
        assert!(offset_of!(RdpCallbacks, on_log_enabled) > offset_of!(RdpCallbacks, on_state));
        assert!(offset_of!(RdpCallbacks, on_log) > offset_of!(RdpCallbacks, on_log_enabled));
        assert!(offset_of!(RdpCallbacks, on_desktop_size) > offset_of!(RdpCallbacks, on_log));
        assert!(offset_of!(RdpCallbacks, on_video_au) > offset_of!(RdpCallbacks, on_desktop_size));
        assert!(offset_of!(RdpCallbacks, on_bitmap_update) > offset_of!(RdpCallbacks, on_video_au));
        assert!(
            offset_of!(RdpCallbacks, on_audio_format) > offset_of!(RdpCallbacks, on_bitmap_update)
        );
        assert!(
            offset_of!(RdpCallbacks, on_audio_data) > offset_of!(RdpCallbacks, on_audio_format)
        );
        assert!(
            offset_of!(RdpCallbacks, on_pointer_bitmap) > offset_of!(RdpCallbacks, on_audio_data)
        );
        assert!(
            offset_of!(RdpCallbacks, on_pointer_position)
                > offset_of!(RdpCallbacks, on_pointer_bitmap)
        );
        assert!(
            offset_of!(RdpCallbacks, on_pointer_state)
                > offset_of!(RdpCallbacks, on_pointer_position)
        );
        assert!(
            offset_of!(RdpCallbacks, on_camera_start) > offset_of!(RdpCallbacks, on_pointer_state)
        );
        assert!(
            offset_of!(RdpCallbacks, on_camera_stop) > offset_of!(RdpCallbacks, on_camera_start)
        );
        assert!(
            offset_of!(RdpCallbacks, on_camera_sample_request)
                > offset_of!(RdpCallbacks, on_camera_stop)
        );
        assert!(
            offset_of!(RdpCallbacks, on_audio_input_start)
                > offset_of!(RdpCallbacks, on_camera_sample_request)
        );
        assert!(
            offset_of!(RdpCallbacks, on_audio_input_stop)
                > offset_of!(RdpCallbacks, on_audio_input_start)
        );
        assert_eq!(
            size_of::<RdpCallbacks>(),
            17 * size_of::<*mut core::ffi::c_void>()
        );
    }

    #[test]
    fn pointer_state_constants_match_header() {
        assert_eq!(RDP_POINTER_STATE_HIDDEN, 0);
        assert_eq!(RDP_POINTER_STATE_DEFAULT, 1);
    }

    #[test]
    fn audio_codec_constants_match_header() {
        assert_eq!(RDP_AUDIO_CODEC_OPUS, 1);
        assert_eq!(RDP_AUDIO_CODEC_PCM_S16LE, 2);
    }
}
