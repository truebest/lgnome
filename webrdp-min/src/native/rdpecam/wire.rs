//! MS-RDPECAM message identifiers and wire encoders.

use ironrdp_dvc::DvcMessage;

use super::{DEVICE_CHANNEL_NAME, DEVICE_DISPLAY_NAME};

pub(super) const PROTOCOL_VERSION: u8 = 2;

pub(super) const MSG_SUCCESS: u8 = 1;
pub(super) const MSG_ERROR: u8 = 2;
pub(super) const MSG_SELECT_VERSION_REQUEST: u8 = 3;
pub(super) const MSG_SELECT_VERSION_RESPONSE: u8 = 4;
pub(super) const MSG_DEVICE_ADDED: u8 = 5;
pub(super) const MSG_DEVICE_REMOVED: u8 = 6;
pub(super) const MSG_ACTIVATE_DEVICE: u8 = 7;
pub(super) const MSG_DEACTIVATE_DEVICE: u8 = 8;
pub(super) const MSG_STREAM_LIST_REQUEST: u8 = 9;
pub(super) const MSG_STREAM_LIST_RESPONSE: u8 = 10;
pub(super) const MSG_MEDIA_TYPE_LIST_REQUEST: u8 = 11;
pub(super) const MSG_MEDIA_TYPE_LIST_RESPONSE: u8 = 12;
pub(super) const MSG_CURRENT_MEDIA_TYPE_REQUEST: u8 = 13;
pub(super) const MSG_CURRENT_MEDIA_TYPE_RESPONSE: u8 = 14;
pub(super) const MSG_START_STREAMS: u8 = 15;
pub(super) const MSG_STOP_STREAMS: u8 = 16;
pub(super) const MSG_SAMPLE_REQUEST: u8 = 17;
pub(super) const MSG_SAMPLE_RESPONSE: u8 = 18;
pub(super) const MSG_SAMPLE_ERROR: u8 = 19;
pub(super) const MSG_PROPERTY_LIST_REQUEST: u8 = 20;
pub(super) const MSG_PROPERTY_LIST_RESPONSE: u8 = 21;

pub(super) const ERROR_UNEXPECTED: u32 = 1;
pub(super) const ERROR_INVALID_MESSAGE: u32 = 2;
pub(super) const ERROR_NOT_INITIALIZED: u32 = 3;
pub(super) const ERROR_INVALID_REQUEST: u32 = 4;
pub(super) const ERROR_INVALID_STREAM: u32 = 5;
pub(super) const ERROR_INVALID_MEDIA_TYPE: u32 = 6;

pub(super) fn append_media_type(dst: &mut Vec<u8>, width: u16, height: u16, fps: u16) {
    dst.extend_from_slice(&media_type(width, height, fps));
}

pub(super) fn media_type(width: u16, height: u16, fps: u16) -> [u8; 26] {
    let mut bytes = [0u8; 26];
    bytes[0] = 1; // H264
    bytes[1..5].copy_from_slice(&u32::from(width).to_le_bytes());
    bytes[5..9].copy_from_slice(&u32::from(height).to_le_bytes());
    bytes[9..13].copy_from_slice(&u32::from(fps).to_le_bytes());
    bytes[13..17].copy_from_slice(&1u32.to_le_bytes());
    bytes[17..21].copy_from_slice(&1u32.to_le_bytes());
    bytes[21..25].copy_from_slice(&1u32.to_le_bytes());
    bytes[25] = 1; // DecodingRequired
    bytes
}

pub(super) fn error_response(version: u8, code: u32) -> Vec<u8> {
    let mut response = vec![version, MSG_ERROR];
    response.extend_from_slice(&code.to_le_bytes());
    response
}

pub(super) fn sample_error(version: u8, code: u32) -> Vec<u8> {
    let mut response = vec![version, MSG_SAMPLE_ERROR, 0];
    response.extend_from_slice(&code.to_le_bytes());
    response
}

pub(super) fn device_added(version: u8) -> Vec<u8> {
    let mut response = vec![version, MSG_DEVICE_ADDED];
    for unit in DEVICE_DISPLAY_NAME.encode_utf16() {
        response.extend_from_slice(&unit.to_le_bytes());
    }
    response.extend_from_slice(&[0, 0]);
    response.extend_from_slice(DEVICE_CHANNEL_NAME.as_bytes());
    response.push(0);
    response
}

pub(super) fn device_removed(version: u8) -> Vec<u8> {
    let mut response = vec![version, MSG_DEVICE_REMOVED];
    response.extend_from_slice(DEVICE_CHANNEL_NAME.as_bytes());
    response.push(0);
    response
}

pub(super) fn raw(bytes: Vec<u8>) -> DvcMessage {
    super::super::dvc::RawDvcMessage::boxed("RDPECAM", bytes)
}
