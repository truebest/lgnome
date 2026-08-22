//! Minimal MS-RDPEAI audio-input client.
//!
//! The native capture side always packetizes S16LE stereo at 44.1 kHz.  This
//! keeps the wire format deterministic and matches gnome-remote-desktop while
//! allowing ALSA capture devices to run at their native rate/channels.

#![forbid(unsafe_code)]

use std::sync::{Arc, Mutex};

use ironrdp_core::impl_as_any;
use ironrdp_dvc::{DvcMessage, DvcProcessor};
use ironrdp_pdu::PduResult;

use super::{CallbackSink, RdpLogLevel, LOG_TARGET_AUDIO_INPUT};

pub(super) const CHANNEL_NAME: &str = "AUDIO_INPUT";
pub(super) const SAMPLE_RATE: u32 = 44_100;
pub(super) const CHANNELS: u16 = 2;
pub(super) const BITS_PER_SAMPLE: u16 = 16;

const VERSION_MAX: u32 = 2;
const MSG_VERSION: u8 = 1;
const MSG_FORMATS: u8 = 2;
const MSG_OPEN: u8 = 3;
const MSG_OPEN_REPLY: u8 = 4;
const MSG_DATA_INCOMING: u8 = 5;
const MSG_DATA: u8 = 6;
const MSG_FORMAT_CHANGE: u8 = 7;
const WAVE_FORMAT_PCM: u16 = 0x0001;
const WAVE_FORMAT_EXTENSIBLE: u16 = 0xfffe;

#[derive(Clone)]
pub(super) struct AudioInputBridge {
    inner: Arc<Mutex<AudioInputState>>,
}

struct AudioInputState {
    callbacks: CallbackSink,
    channel_id: Option<u32>,
    version: u32,
    open: bool,
    generation: u64,
    frames_per_packet: u32,
}

impl AudioInputBridge {
    pub(super) fn new(callbacks: CallbackSink) -> Self {
        Self {
            inner: Arc::new(Mutex::new(AudioInputState {
                callbacks,
                channel_id: None,
                version: VERSION_MAX,
                open: false,
                generation: 0,
                frames_per_packet: 0,
            })),
        }
    }

    pub(super) fn submit_pcm(
        &self,
        generation: u64,
        data: Vec<u8>,
    ) -> Option<(u32, Vec<DvcMessage>)> {
        let state = self.inner.lock().ok()?;
        let expected = usize::try_from(state.frames_per_packet)
            .ok()?
            .checked_mul(usize::from(CHANNELS))?
            .checked_mul(usize::from(BITS_PER_SAMPLE / 8))?;
        if !state.open || state.generation != generation || data.len() != expected || expected == 0
        {
            return None;
        }
        let channel_id = state.channel_id?;
        drop(state);
        let mut packet = Vec::with_capacity(1 + data.len());
        packet.push(MSG_DATA);
        packet.extend_from_slice(&data);
        Some((channel_id, vec![raw(vec![MSG_DATA_INCOMING]), raw(packet)]))
    }
}

pub(super) struct AudioInputHandler {
    bridge: AudioInputBridge,
    offered_format: bool,
}

impl AudioInputHandler {
    pub(super) fn new(bridge: AudioInputBridge) -> Self {
        Self {
            bridge,
            offered_format: false,
        }
    }

    fn process_payload(&mut self, channel_id: u32, payload: &[u8]) -> Vec<DvcMessage> {
        let Some(&message_id) = payload.first() else {
            return Vec::new();
        };
        match message_id {
            MSG_VERSION if payload.len() == 5 => {
                let server_version =
                    u32::from_le_bytes(payload[1..5].try_into().expect("four bytes"));
                let version = server_version.min(VERSION_MAX).max(1);
                if let Ok(mut state) = self.bridge.inner.lock() {
                    state.channel_id = Some(channel_id);
                    state.version = version;
                }
                let mut response = vec![MSG_VERSION];
                response.extend_from_slice(&version.to_le_bytes());
                vec![raw(response)]
            }
            MSG_FORMATS if server_offers_pcm(payload) => {
                self.offered_format = true;
                vec![raw(vec![MSG_DATA_INCOMING]), raw(client_formats())]
            }
            MSG_OPEN if payload.len() >= 27 => self.handle_open(channel_id, payload),
            MSG_FORMAT_CHANGE if payload.len() == 5 => {
                let index = u32::from_le_bytes(payload[1..5].try_into().expect("four bytes"));
                if index == 0 && self.offered_format {
                    vec![raw(payload.to_vec())]
                } else {
                    Vec::new()
                }
            }
            _ => {
                if let Ok(state) = self.bridge.inner.lock() {
                    state.callbacks.log(
                        RdpLogLevel::Warning,
                        LOG_TARGET_AUDIO_INPUT,
                        format_args!(
                            "microphone: ignoring malformed/unknown RDPEAI PDU id={} len={}",
                            message_id,
                            payload.len()
                        ),
                    );
                }
                Vec::new()
            }
        }
    }

    fn handle_open(&mut self, channel_id: u32, payload: &[u8]) -> Vec<DvcMessage> {
        let frames = u32::from_le_bytes(payload[1..5].try_into().expect("four bytes"));
        let format_index = u32::from_le_bytes(payload[5..9].try_into().expect("four bytes"));
        let format_tag = u16::from_le_bytes(payload[9..11].try_into().expect("two bytes"));
        let channels = u16::from_le_bytes(payload[11..13].try_into().expect("two bytes"));
        let sample_rate = u32::from_le_bytes(payload[13..17].try_into().expect("four bytes"));
        let bits = u16::from_le_bytes(payload[23..25].try_into().expect("two bytes"));
        let cb_size = u16::from_le_bytes(payload[25..27].try_into().expect("two bytes"));
        let capture_format_valid = match format_tag {
            WAVE_FORMAT_PCM => cb_size == 0,
            WAVE_FORMAT_EXTENSIBLE => {
                payload.len() >= 49
                    && cb_size == 22
                    && u16::from_le_bytes(payload[27..29].try_into().expect("two bytes"))
                        == BITS_PER_SAMPLE
                    && u32::from_le_bytes(payload[29..33].try_into().expect("four bytes")) == 3
                    && payload[33..49]
                        == [
                            1, 0, 0, 0, 0, 0, 0x10, 0, 0x80, 0, 0, 0xaa, 0, 0x38, 0x9b, 0x71,
                        ]
            }
            _ => false,
        };
        let valid = self.offered_format
            && format_index == 0
            && capture_format_valid
            && channels == CHANNELS
            && sample_rate == SAMPLE_RATE
            && bits == BITS_PER_SAMPLE
            && (1..=4096).contains(&frames);

        let mut open_reply = vec![MSG_OPEN_REPLY];
        if !valid {
            // E_INVALIDARG
            open_reply.extend_from_slice(&0x8007_0057u32.to_le_bytes());
            return vec![raw(open_reply)];
        }

        let (callbacks, generation) = match self.bridge.inner.lock() {
            Ok(mut state) => {
                if state.open {
                    let old_generation = state.generation;
                    state.callbacks.audio_input_stop(old_generation);
                }
                state.channel_id = Some(channel_id);
                state.open = true;
                state.frames_per_packet = frames;
                state.generation = state.generation.wrapping_add(1).max(1);
                (state.callbacks, state.generation)
            }
            Err(_) => {
                open_reply.extend_from_slice(&0x8000_4005u32.to_le_bytes());
                return vec![raw(open_reply)];
            }
        };
        callbacks.audio_input_start(generation, SAMPLE_RATE, CHANNELS, frames);
        let mut format_change = vec![MSG_FORMAT_CHANGE];
        format_change.extend_from_slice(&0u32.to_le_bytes());
        open_reply.extend_from_slice(&0u32.to_le_bytes());
        vec![raw(format_change), raw(open_reply)]
    }

    fn stop(&self) {
        if let Ok(mut state) = self.bridge.inner.lock() {
            if state.open {
                let generation = state.generation;
                state.open = false;
                state.frames_per_packet = 0;
                state.generation = state.generation.wrapping_add(1).max(1);
                state.callbacks.audio_input_stop(generation);
            }
            state.channel_id = None;
        }
    }
}

impl_as_any!(AudioInputHandler);

impl DvcProcessor for AudioInputHandler {
    fn channel_name(&self) -> &str {
        CHANNEL_NAME
    }

    fn start(&mut self, channel_id: u32) -> PduResult<Vec<DvcMessage>> {
        if let Ok(mut state) = self.bridge.inner.lock() {
            state.channel_id = Some(channel_id);
        }
        Ok(Vec::new())
    }

    fn process(&mut self, channel_id: u32, payload: &[u8]) -> PduResult<Vec<DvcMessage>> {
        Ok(self.process_payload(channel_id, payload))
    }

    fn close(&mut self, _channel_id: u32) {
        self.stop();
    }
}

fn client_formats() -> Vec<u8> {
    const FORMAT_SIZE: u32 = 18;
    let mut response = vec![MSG_FORMATS];
    response.extend_from_slice(&1u32.to_le_bytes());
    response.extend_from_slice(&(9 + FORMAT_SIZE).to_le_bytes());
    response.extend_from_slice(&WAVE_FORMAT_PCM.to_le_bytes());
    response.extend_from_slice(&CHANNELS.to_le_bytes());
    response.extend_from_slice(&SAMPLE_RATE.to_le_bytes());
    response.extend_from_slice(&(SAMPLE_RATE * u32::from(CHANNELS) * 2).to_le_bytes());
    response.extend_from_slice(&(CHANNELS * 2).to_le_bytes());
    response.extend_from_slice(&BITS_PER_SAMPLE.to_le_bytes());
    response.extend_from_slice(&0u16.to_le_bytes());
    response
}

fn server_offers_pcm(payload: &[u8]) -> bool {
    if payload.len() < 9 || payload[0] != MSG_FORMATS {
        return false;
    }
    let count = u32::from_le_bytes(payload[1..5].try_into().expect("four bytes"));
    if count == 0 || count > 1000 {
        return false;
    }
    // cbSizeFormatsPacket is reserved and arbitrary in the server-to-client PDU.
    // gnome-remote-desktop currently sends zero here, so parse the advertised
    // AUDIO_FORMAT array against the actual DVC payload and ignore trailing ExtraData.
    let formats_end = payload.len();
    let mut offset = 9usize;
    for _ in 0..count {
        if match offset.checked_add(18) {
            Some(end) => end > formats_end,
            None => true,
        } {
            return false;
        }
        let format_tag =
            u16::from_le_bytes(payload[offset..offset + 2].try_into().expect("two bytes"));
        let channels = u16::from_le_bytes(
            payload[offset + 2..offset + 4]
                .try_into()
                .expect("two bytes"),
        );
        let rate = u32::from_le_bytes(
            payload[offset + 4..offset + 8]
                .try_into()
                .expect("four bytes"),
        );
        let average = u32::from_le_bytes(
            payload[offset + 8..offset + 12]
                .try_into()
                .expect("four bytes"),
        );
        let align = u16::from_le_bytes(
            payload[offset + 12..offset + 14]
                .try_into()
                .expect("two bytes"),
        );
        let bits = u16::from_le_bytes(
            payload[offset + 14..offset + 16]
                .try_into()
                .expect("two bytes"),
        );
        let extra = usize::from(u16::from_le_bytes(
            payload[offset + 16..offset + 18]
                .try_into()
                .expect("two bytes"),
        ));
        let Some(next) = offset
            .checked_add(18)
            .and_then(|value| value.checked_add(extra))
        else {
            return false;
        };
        if next > formats_end {
            return false;
        }
        if format_tag == WAVE_FORMAT_PCM
            && channels == CHANNELS
            && rate == SAMPLE_RATE
            && average == SAMPLE_RATE * u32::from(CHANNELS) * 2
            && align == CHANNELS * 2
            && bits == BITS_PER_SAMPLE
            && extra == 0
        {
            return true;
        }
        offset = next;
    }
    false
}

fn raw(bytes: Vec<u8>) -> DvcMessage {
    super::dvc::RawDvcMessage::boxed("RDPEAI", bytes)
}

#[cfg(test)]
mod tests {
    use super::*;
    use ironrdp_core::encode_vec;

    fn encoded(message: &DvcMessage) -> Vec<u8> {
        encode_vec(message.as_ref()).unwrap()
    }

    fn open_pdu(frames: u32) -> Vec<u8> {
        let mut pdu = vec![MSG_OPEN];
        pdu.extend_from_slice(&frames.to_le_bytes());
        pdu.extend_from_slice(&0u32.to_le_bytes());
        pdu.extend_from_slice(&WAVE_FORMAT_PCM.to_le_bytes());
        pdu.extend_from_slice(&CHANNELS.to_le_bytes());
        pdu.extend_from_slice(&SAMPLE_RATE.to_le_bytes());
        pdu.extend_from_slice(&(SAMPLE_RATE * 4).to_le_bytes());
        pdu.extend_from_slice(&4u16.to_le_bytes());
        pdu.extend_from_slice(&BITS_PER_SAMPLE.to_le_bytes());
        pdu.extend_from_slice(&0u16.to_le_bytes());
        pdu
    }

    fn extensible_open_pdu(frames: u32) -> Vec<u8> {
        let mut pdu = open_pdu(frames);
        pdu[9..11].copy_from_slice(&WAVE_FORMAT_EXTENSIBLE.to_le_bytes());
        pdu[25..27].copy_from_slice(&22u16.to_le_bytes());
        pdu.extend_from_slice(&BITS_PER_SAMPLE.to_le_bytes());
        pdu.extend_from_slice(&3u32.to_le_bytes());
        pdu.extend_from_slice(&[
            1, 0, 0, 0, 0, 0, 0x10, 0, 0x80, 0, 0, 0xaa, 0, 0x38, 0x9b, 0x71,
        ]);
        pdu
    }

    fn server_formats() -> Vec<u8> {
        // The server field cbSizeFormatsPacket is reserved/arbitrary. GRD leaves it
        // zero even though the PDU contains a normal AUDIO_FORMAT array.
        let mut formats = client_formats();
        formats[5..9].copy_from_slice(&0u32.to_le_bytes());
        formats
    }

    #[test]
    fn handshake_offers_one_pcm_format() {
        let bridge = AudioInputBridge::new(CallbackSink::empty());
        let mut handler = AudioInputHandler::new(bridge);
        let version = handler.process_payload(9, &[1, 2, 0, 0, 0]);
        assert_eq!(encoded(&version[0]), [1, 2, 0, 0, 0]);
        let formats = handler.process_payload(9, &server_formats());
        assert_eq!(encoded(&formats[0]), [5]);
        let bytes = encoded(&formats[1]);
        assert_eq!(bytes[0], 2);
        assert_eq!(u32::from_le_bytes(bytes[1..5].try_into().unwrap()), 1);
        assert_eq!(u16::from_le_bytes(bytes[9..11].try_into().unwrap()), 1);
        assert_eq!(
            u32::from_le_bytes(bytes[13..17].try_into().unwrap()),
            SAMPLE_RATE
        );
    }

    #[test]
    fn handshake_rejects_server_without_exact_pcm_format() {
        let bridge = AudioInputBridge::new(CallbackSink::empty());
        let mut handler = AudioInputHandler::new(bridge);
        let mut formats = server_formats();
        formats[9..11].copy_from_slice(&6u16.to_le_bytes()); // WAVE_FORMAT_ALAW
        assert!(handler.process_payload(9, &formats).is_empty());
        assert!(!handler.offered_format);
    }

    #[test]
    fn open_and_submit_exact_pcm_packet() {
        let bridge = AudioInputBridge::new(CallbackSink::empty());
        let mut handler = AudioInputHandler::new(bridge.clone());
        handler.process_payload(9, &server_formats());
        let replies = handler.process_payload(9, &extensible_open_pdu(441));
        assert_eq!(encoded(&replies[0]), [7, 0, 0, 0, 0]);
        assert_eq!(encoded(&replies[1]), [4, 0, 0, 0, 0]);
        let generation = bridge.inner.lock().unwrap().generation;
        let (_, messages) = bridge.submit_pcm(generation, vec![0; 441 * 2 * 2]).unwrap();
        assert_eq!(encoded(&messages[0]), [5]);
        assert_eq!(encoded(&messages[1]).len(), 1 + 441 * 2 * 2);
        assert!(bridge.submit_pcm(generation, vec![0; 441 * 2]).is_none());
    }
}
