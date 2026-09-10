//! MS-RDPEA over AUDIO_PLAYBACK_DVC. Protocol errors silence audio, not the RDP session.

use ironrdp_core::{impl_as_any, Decode as _, Encode, EncodeResult, ReadCursor, WriteCursor};
use ironrdp_dvc::{
    DvcChannelListener, DvcClientProcessor, DvcEncode, DvcMessage, DvcProcessor, DynamicChannelId,
};
use ironrdp_pdu::PduResult;
use ironrdp_rdpsnd::pdu as sndpdu;

use super::{
    CallbackSink, RdpLogLevel, LOG_TARGET_AUDIO, RDP_AUDIO_CODEC_OPUS, RDP_AUDIO_CODEC_PCM_S16LE,
};

const AUDIO_DVC_CHANNEL_NAME: &str = "AUDIO_PLAYBACK_DVC";

/// Local wrapper adapts the foreign SvcEncode type to DvcEncode.
struct RdpsndDvcMessage(sndpdu::ClientAudioOutputPdu);

impl Encode for RdpsndDvcMessage {
    fn encode(&self, dst: &mut WriteCursor<'_>) -> EncodeResult<()> {
        self.0.encode(dst)
    }

    fn name(&self) -> &'static str {
        self.0.name()
    }

    fn size(&self) -> usize {
        self.0.size()
    }
}

impl DvcEncode for RdpsndDvcMessage {}

pub(super) struct RdpsndDvcHandler {
    callbacks: CallbackSink,
    /// Latched on an undecodable payload; the channel goes silent instead of erroring.
    stopped: bool,
    /// Wave2 wFormatNo indexes this client list, not the server's advertised list.
    client_formats: Vec<sndpdu::AudioFormat>,
    /// (codec, sample_rate, channels) last delivered through on_audio_format.
    last_format: Option<(u32, u32, u16)>,
    prefer_pcm: bool,
    bad_format_logged: bool,
    legacy_wave_logged: bool,
}

impl_as_any!(RdpsndDvcHandler);

impl RdpsndDvcHandler {
    pub(super) fn new(callbacks: CallbackSink, prefer_pcm: bool) -> Self {
        Self {
            callbacks,
            stopped: false,
            client_formats: Vec::new(),
            last_format: None,
            prefer_pcm,
            bad_format_logged: false,
            legacy_wave_logged: false,
        }
    }

    fn rdp_audio_codec_for(format: &sndpdu::AudioFormat) -> Option<u32> {
        match format.format {
            sndpdu::WaveFormat::OPUS
                if format.n_channels == 2 && format.n_samples_per_sec == 48_000 =>
            {
                Some(RDP_AUDIO_CODEC_OPUS)
            }
            sndpdu::WaveFormat::PCM
                if format.n_channels == 2
                    && format.bits_per_sample == 16
                    && matches!(format.n_samples_per_sec, 44_100 | 48_000) =>
            {
                Some(RDP_AUDIO_CODEC_PCM_S16LE)
            }
            _ => None,
        }
    }

    fn reply(pdu: sndpdu::ClientAudioOutputPdu) -> DvcMessage {
        Box::new(RdpsndDvcMessage(pdu))
    }

    fn handle_audio_format(&mut self, pdu: sndpdu::ServerAudioFormatPdu) -> Vec<DvcMessage> {
        let server_count = pdu.formats.len();
        // Preserve the server's format metadata for accepted codecs.
        let mut formats: Vec<sndpdu::AudioFormat> = pdu
            .formats
            .into_iter()
            .filter(|format| Self::rdp_audio_codec_for(format).is_some())
            .collect();
        if self.prefer_pcm {
            let pcm_only: Vec<sndpdu::AudioFormat> = formats
                .iter()
                .filter(|format| {
                    Self::rdp_audio_codec_for(format) == Some(RDP_AUDIO_CODEC_PCM_S16LE)
                })
                .cloned()
                .collect();
            if pcm_only.is_empty() {
                self.callbacks.log(
                    RdpLogLevel::Warning,
                    LOG_TARGET_AUDIO,
                    format_args!(
                        "audio: PCM requested but the server offers no playable PCM; \
                         keeping the default format list"
                    ),
                );
            } else {
                formats = pcm_only;
            }
        }
        if formats.is_empty() {
            self.callbacks.log(
                RdpLogLevel::Warning,
                LOG_TARGET_AUDIO,
                format_args!(
                    "audio: server offered {server_count} formats but none are playable; audio stays disabled"
                ),
            );
        } else {
            self.callbacks.log(
                RdpLogLevel::Info,
                LOG_TARGET_AUDIO,
                format_args!(
                    "audio: accepting {} of {} server formats (protocol version {:?})",
                    formats.len(),
                    server_count,
                    pdu.version
                ),
            );
        }
        self.client_formats = formats.clone();
        self.last_format = None;
        self.bad_format_logged = false;

        let mut messages = vec![Self::reply(sndpdu::ClientAudioOutputPdu::AudioFormat(
            sndpdu::ClientAudioFormatPdu {
                version: pdu.version,
                flags: sndpdu::AudioFormatFlags::ALIVE,
                formats,
                volume_left: 0xFFFF,
                volume_right: 0xFFFF,
                pitch: 0x0001_0000,
                dgram_port: 0,
            },
        ))];
        if pdu.version >= sndpdu::Version::V6 {
            messages.push(Self::reply(sndpdu::ClientAudioOutputPdu::QualityMode(
                sndpdu::QualityModePdu {
                    quality_mode: sndpdu::QualityMode::High,
                },
            )));
        }
        messages
    }

    fn handle_wave2(&mut self, pdu: sndpdu::Wave2Pdu<'_>) -> Vec<DvcMessage> {
        if let Some(format) = self.client_formats.get(usize::from(pdu.format_no)) {
            if let Some(codec) = Self::rdp_audio_codec_for(format) {
                let current = (codec, format.n_samples_per_sec, format.n_channels);
                if self.last_format != Some(current) {
                    self.callbacks
                        .audio_format(codec, format.n_samples_per_sec, format.n_channels);
                    self.last_format = Some(current);
                }
                self.callbacks.audio_data(&pdu.data, pdu.audio_timestamp);
            }
        } else if !self.bad_format_logged {
            self.callbacks.log(
                RdpLogLevel::Warning,
                LOG_TARGET_AUDIO,
                format_args!(
                    "audio: wave references unknown client format {}; dropping audio data",
                    pdu.format_no
                ),
            );
            self.bad_format_logged = true;
        }
        // Confirm even dropped waves, promptly: the server estimates render latency from
        // these confirms and starts discarding audio when it thinks we are >300ms behind.
        vec![Self::reply(sndpdu::ClientAudioOutputPdu::WaveConfirm(
            sndpdu::WaveConfirmPdu {
                timestamp: pdu.timestamp,
                block_no: pdu.block_no,
            },
        ))]
    }
}

impl DvcProcessor for RdpsndDvcHandler {
    fn channel_name(&self) -> &str {
        AUDIO_DVC_CHANNEL_NAME
    }

    fn start(&mut self, _channel_id: u32) -> PduResult<Vec<DvcMessage>> {
        // The server speaks first (Server Audio Formats and Version PDU).
        Ok(Vec::new())
    }

    fn process(&mut self, _channel_id: u32, payload: &[u8]) -> PduResult<Vec<DvcMessage>> {
        if self.stopped {
            return Ok(Vec::new());
        }
        let mut cursor = ReadCursor::new(payload);
        let pdu = match sndpdu::ServerAudioOutputPdu::decode(&mut cursor) {
            Ok(pdu) => pdu,
            Err(e) => {
                self.callbacks.log(
                    RdpLogLevel::Error,
                    LOG_TARGET_AUDIO,
                    format_args!("audio: failed to decode audio output PDU: {e}; audio disabled"),
                );
                self.stopped = true;
                return Ok(Vec::new());
            }
        };

        let messages = match pdu {
            sndpdu::ServerAudioOutputPdu::AudioFormat(pdu) => self.handle_audio_format(pdu),
            sndpdu::ServerAudioOutputPdu::Training(pdu) => {
                // Echo wPackSize, restoring the 8-byte header removed by IronRDP decoding.
                let pack_size = if pdu.data.is_empty() {
                    0
                } else {
                    u16::try_from(pdu.data.len().saturating_add(8)).unwrap_or(0)
                };
                self.callbacks.log(
                    RdpLogLevel::Debug,
                    LOG_TARGET_AUDIO,
                    format_args!(
                        "audio: training received ({} payload bytes)",
                        pdu.data.len()
                    ),
                );
                vec![Self::reply(sndpdu::ClientAudioOutputPdu::TrainingConfirm(
                    sndpdu::TrainingConfirmPdu {
                        timestamp: pdu.timestamp,
                        pack_size,
                    },
                ))]
            }
            sndpdu::ServerAudioOutputPdu::Wave2(pdu) => self.handle_wave2(pdu),
            sndpdu::ServerAudioOutputPdu::Volume(pdu) => {
                self.callbacks.log(
                    RdpLogLevel::Debug,
                    LOG_TARGET_AUDIO,
                    format_args!(
                        "audio: ignoring server volume change {:#06x}/{:#06x} (TV remote controls volume)",
                        pdu.volume_left, pdu.volume_right
                    ),
                );
                Vec::new()
            }
            sndpdu::ServerAudioOutputPdu::Pitch(pdu) => {
                self.callbacks.log(
                    RdpLogLevel::Debug,
                    LOG_TARGET_AUDIO,
                    format_args!("audio: ignoring server pitch change {:#010x}", pdu.pitch),
                );
                Vec::new()
            }
            sndpdu::ServerAudioOutputPdu::Close => {
                self.callbacks.log(
                    RdpLogLevel::Info,
                    LOG_TARGET_AUDIO,
                    format_args!("audio: server closed the audio stream"),
                );
                // Re-fire on_audio_format when a new stream starts later.
                self.last_format = None;
                Vec::new()
            }
            sndpdu::ServerAudioOutputPdu::Wave(_)
            | sndpdu::ServerAudioOutputPdu::WaveEncrypt(_)
            | sndpdu::ServerAudioOutputPdu::CryptKey(_) => {
                if !self.legacy_wave_logged {
                    self.callbacks.log(
                        RdpLogLevel::Warning,
                        LOG_TARGET_AUDIO,
                        format_args!(
                            "audio: ignoring legacy/encrypted wave PDU (protocol version < 8)"
                        ),
                    );
                    self.legacy_wave_logged = true;
                }
                Vec::new()
            }
        };
        Ok(messages)
    }

    fn close(&mut self, _channel_id: u32) {
        self.callbacks.log(
            RdpLogLevel::Notice,
            LOG_TARGET_AUDIO,
            format_args!("audio: AUDIO_PLAYBACK_DVC channel closed"),
        );
    }
}

impl DvcClientProcessor for RdpsndDvcHandler {}

/// Windows re-creates the audio DVC; each create needs a fresh processor.
pub(super) struct RdpsndDvcFactory {
    pub(super) callbacks: CallbackSink,
    pub(super) prefer_pcm: bool,
}

impl DvcChannelListener for RdpsndDvcFactory {
    fn channel_name(&self) -> &str {
        AUDIO_DVC_CHANNEL_NAME
    }

    fn create(&mut self, _channel_id: DynamicChannelId) -> Option<Box<dyn DvcClientProcessor>> {
        Some(Box::new(RdpsndDvcHandler::new(
            self.callbacks,
            self.prefer_pcm,
        )))
    }
}

#[cfg(test)]
mod tests {
    use super::super::RdpCallbacks;
    use super::*;
    use std::sync::Mutex;

    // ---- rdpsnd-over-DVC state machine ----

    /// Captures on_audio_format/on_audio_data invocations behind the C ABI ctx pointer.
    #[derive(Default)]
    struct AudioCapture {
        formats: Vec<(u32, u32, u16)>,
        data: Vec<(Vec<u8>, u32)>,
    }

    extern "C" fn capture_audio_format(
        ctx: *mut core::ffi::c_void,
        codec: u32,
        sample_rate: u32,
        channels: u16,
    ) {
        let capture = unsafe { &*(ctx.cast::<Mutex<AudioCapture>>()) };
        capture
            .lock()
            .unwrap()
            .formats
            .push((codec, sample_rate, channels));
    }

    extern "C" fn capture_audio_data(
        ctx: *mut core::ffi::c_void,
        data: *const u8,
        len: usize,
        ts_ms: u32,
    ) {
        let capture = unsafe { &*(ctx.cast::<Mutex<AudioCapture>>()) };
        let bytes = unsafe { core::slice::from_raw_parts(data, len) }.to_vec();
        capture.lock().unwrap().data.push((bytes, ts_ms));
    }

    fn audio_test_handler(capture: &Mutex<AudioCapture>) -> RdpsndDvcHandler {
        let callbacks = RdpCallbacks {
            ctx: (capture as *const Mutex<AudioCapture>).cast_mut().cast(),
            on_audio_format: Some(capture_audio_format),
            on_audio_data: Some(capture_audio_data),
            ..RdpCallbacks::default()
        };
        RdpsndDvcHandler::new(CallbackSink::new(callbacks), false)
    }

    /// The three formats gnome-remote-desktop advertises, in its order.
    fn grd_server_formats() -> Vec<sndpdu::AudioFormat> {
        vec![
            sndpdu::AudioFormat {
                format: sndpdu::WaveFormat::AAC_MS,
                n_channels: 2,
                n_samples_per_sec: 44_100,
                n_avg_bytes_per_sec: 12_000,
                n_block_align: 4,
                bits_per_sample: 16,
                data: None,
            },
            sndpdu::AudioFormat {
                format: sndpdu::WaveFormat::OPUS,
                n_channels: 2,
                n_samples_per_sec: 48_000,
                n_avg_bytes_per_sec: 12_000,
                n_block_align: 4,
                bits_per_sample: 16,
                data: None,
            },
            sndpdu::AudioFormat {
                format: sndpdu::WaveFormat::PCM,
                n_channels: 2,
                n_samples_per_sec: 44_100,
                n_avg_bytes_per_sec: 176_400,
                n_block_align: 4,
                bits_per_sample: 16,
                data: None,
            },
        ]
    }

    fn encode_server_pdu(pdu: sndpdu::ServerAudioOutputPdu<'_>) -> Vec<u8> {
        ironrdp_core::encode_vec(&pdu).expect("encode server audio PDU")
    }

    fn decode_reply(message: &DvcMessage) -> sndpdu::ClientAudioOutputPdu {
        let bytes = ironrdp_core::encode_vec(message.as_ref()).expect("encode reply");
        let mut cursor = ReadCursor::new(&bytes);
        sndpdu::ClientAudioOutputPdu::decode(&mut cursor).expect("decode reply")
    }

    fn negotiate_and_train(handler: &mut RdpsndDvcHandler) {
        let payload = encode_server_pdu(sndpdu::ServerAudioOutputPdu::AudioFormat(
            sndpdu::ServerAudioFormatPdu {
                version: sndpdu::Version::V8,
                formats: grd_server_formats(),
            },
        ));
        handler.process(0, &payload).expect("negotiation");
        let payload = encode_server_pdu(sndpdu::ServerAudioOutputPdu::Training(
            sndpdu::TrainingPdu {
                timestamp: 42,
                data: Vec::new(),
            },
        ));
        handler.process(0, &payload).expect("training");
    }

    #[test]
    fn rdpsnd_prefer_pcm_drops_opus_from_client_list() {
        let capture = Mutex::new(AudioCapture::default());
        let mut handler = audio_test_handler(&capture);
        handler.prefer_pcm = true;

        let payload = encode_server_pdu(sndpdu::ServerAudioOutputPdu::AudioFormat(
            sndpdu::ServerAudioFormatPdu {
                version: sndpdu::Version::V8,
                formats: grd_server_formats(),
            },
        ));
        let replies = handler.process(0, &payload).expect("process");
        match decode_reply(&replies[0]) {
            sndpdu::ClientAudioOutputPdu::AudioFormat(pdu) => {
                // Only the PCM entry survives, so grd cannot pick Opus.
                assert_eq!(pdu.formats, grd_server_formats()[2..].to_vec());
            }
            other => panic!("expected AudioFormat reply, got {other:?}"),
        }

        // A server with no PCM keeps the playable list instead of going silent.
        let mut handler = audio_test_handler(&capture);
        handler.prefer_pcm = true;
        let opus_only = vec![grd_server_formats()[1].clone()];
        let payload = encode_server_pdu(sndpdu::ServerAudioOutputPdu::AudioFormat(
            sndpdu::ServerAudioFormatPdu {
                version: sndpdu::Version::V8,
                formats: opus_only.clone(),
            },
        ));
        let replies = handler.process(0, &payload).expect("process");
        match decode_reply(&replies[0]) {
            sndpdu::ClientAudioOutputPdu::AudioFormat(pdu) => {
                assert_eq!(pdu.formats, opus_only);
            }
            other => panic!("expected AudioFormat reply, got {other:?}"),
        }
    }

    #[test]
    fn rdpsnd_negotiation_filters_grd_formats() {
        let capture = Mutex::new(AudioCapture::default());
        let mut handler = audio_test_handler(&capture);

        let payload = encode_server_pdu(sndpdu::ServerAudioOutputPdu::AudioFormat(
            sndpdu::ServerAudioFormatPdu {
                version: sndpdu::Version::V8,
                formats: grd_server_formats(),
            },
        ));
        let replies = handler.process(0, &payload).expect("process");
        assert_eq!(replies.len(), 2, "expected ClientAudioFormat + QualityMode");

        match decode_reply(&replies[0]) {
            sndpdu::ClientAudioOutputPdu::AudioFormat(pdu) => {
                // AAC must be filtered out (the TV cannot play it and the server would
                // otherwise prefer it); the OPUS and PCM entries are echoed verbatim —
                // Opus decodes in-process for the mixer, PCM is the fallback.
                assert_eq!(pdu.formats, grd_server_formats()[1..].to_vec());
                assert_eq!(pdu.version, sndpdu::Version::V8);
                assert!(pdu.flags.contains(sndpdu::AudioFormatFlags::ALIVE));
                assert_eq!(pdu.volume_left, 0xFFFF);
                assert_eq!(pdu.volume_right, 0xFFFF);
                assert_eq!(pdu.pitch, 0x0001_0000);
                assert_eq!(pdu.dgram_port, 0);
            }
            other => panic!("expected AudioFormat reply, got {other:?}"),
        }
        match decode_reply(&replies[1]) {
            sndpdu::ClientAudioOutputPdu::QualityMode(pdu) => {
                assert_eq!(pdu.quality_mode, sndpdu::QualityMode::High);
            }
            other => panic!("expected QualityMode reply, got {other:?}"),
        }
        // Negotiation alone must not touch the C callbacks.
        assert!(capture.lock().unwrap().formats.is_empty());
    }

    #[test]
    fn rdpsnd_negotiation_falls_back_to_opus_without_pcm() {
        let capture = Mutex::new(AudioCapture::default());
        let mut handler = audio_test_handler(&capture);

        let payload = encode_server_pdu(sndpdu::ServerAudioOutputPdu::AudioFormat(
            sndpdu::ServerAudioFormatPdu {
                version: sndpdu::Version::V8,
                formats: grd_server_formats()[..2].to_vec(), // AAC + OPUS, no PCM
            },
        ));
        let replies = handler.process(0, &payload).expect("process");
        match decode_reply(&replies[0]) {
            sndpdu::ClientAudioOutputPdu::AudioFormat(pdu) => {
                assert_eq!(pdu.formats, grd_server_formats()[1..2].to_vec());
            }
            other => panic!("expected AudioFormat reply, got {other:?}"),
        }
    }

    #[test]
    fn rdpsnd_training_confirm_echoes_timestamp() {
        let capture = Mutex::new(AudioCapture::default());
        let mut handler = audio_test_handler(&capture);

        let payload = encode_server_pdu(sndpdu::ServerAudioOutputPdu::Training(
            sndpdu::TrainingPdu {
                timestamp: 0x1234,
                data: Vec::new(),
            },
        ));
        let replies = handler.process(0, &payload).expect("process");
        assert_eq!(replies.len(), 1);
        match decode_reply(&replies[0]) {
            sndpdu::ClientAudioOutputPdu::TrainingConfirm(pdu) => {
                assert_eq!(pdu.timestamp, 0x1234);
                assert_eq!(pdu.pack_size, 0);
            }
            other => panic!("expected TrainingConfirm reply, got {other:?}"),
        }
    }

    #[test]
    fn rdpsnd_training_confirm_restores_wire_pack_size() {
        let capture = Mutex::new(AudioCapture::default());
        let mut handler = audio_test_handler(&capture);

        // Training confirmation must echo the original wPackSize (1024), not decoded data length.
        let payload = encode_server_pdu(sndpdu::ServerAudioOutputPdu::Training(
            sndpdu::TrainingPdu {
                timestamp: 0,
                data: vec![0u8; 1016],
            },
        ));
        let replies = handler.process(0, &payload).expect("process");
        assert_eq!(replies.len(), 1);
        match decode_reply(&replies[0]) {
            sndpdu::ClientAudioOutputPdu::TrainingConfirm(pdu) => {
                assert_eq!(pdu.timestamp, 0);
                assert_eq!(pdu.pack_size, 1024);
            }
            other => panic!("expected TrainingConfirm reply, got {other:?}"),
        }
    }

    #[test]
    fn rdpsnd_wave2_fires_format_once_then_data_and_confirms() {
        let capture = Mutex::new(AudioCapture::default());
        let mut handler = audio_test_handler(&capture);
        negotiate_and_train(&mut handler);

        // format_no indexes the CLIENT's filtered list: 0 = OPUS 48k (AAC was dropped).
        for (block_no, payload_byte) in [(7u8, 0xAAu8), (8u8, 0xBBu8)] {
            let payload =
                encode_server_pdu(sndpdu::ServerAudioOutputPdu::Wave2(sndpdu::Wave2Pdu {
                    timestamp: 100 + u16::from(block_no),
                    format_no: 0,
                    block_no,
                    audio_timestamp: 5000 + u32::from(block_no),
                    data: vec![payload_byte; 16].into(),
                }));
            let replies = handler.process(0, &payload).expect("process");
            assert_eq!(replies.len(), 1);
            match decode_reply(&replies[0]) {
                sndpdu::ClientAudioOutputPdu::WaveConfirm(pdu) => {
                    assert_eq!(pdu.timestamp, 100 + u16::from(block_no));
                    assert_eq!(pdu.block_no, block_no);
                }
                other => panic!("expected WaveConfirm reply, got {other:?}"),
            }
        }

        let capture = capture.lock().unwrap();
        assert_eq!(
            capture.formats,
            vec![(RDP_AUDIO_CODEC_OPUS, 48_000, 2)],
            "on_audio_format must fire exactly once for an unchanged format"
        );
        assert_eq!(capture.data.len(), 2);
        assert_eq!(capture.data[0], (vec![0xAA; 16], 5007));
        assert_eq!(capture.data[1], (vec![0xBB; 16], 5008));
    }

    #[test]
    fn rdpsnd_malformed_payload_is_not_fatal() {
        let capture = Mutex::new(AudioCapture::default());
        let mut handler = audio_test_handler(&capture);
        negotiate_and_train(&mut handler);

        // A decode failure must silence the channel, never error the session.
        let replies = handler
            .process(0, &[0xFF, 0x00, 0x02, 0x00, 0x99])
            .expect("must not error");
        assert!(replies.is_empty());

        // Latched off: even a valid wave afterward is ignored.
        let payload = encode_server_pdu(sndpdu::ServerAudioOutputPdu::Wave2(sndpdu::Wave2Pdu {
            timestamp: 1,
            format_no: 0,
            block_no: 1,
            audio_timestamp: 1,
            data: vec![0u8; 4].into(),
        }));
        let replies = handler.process(0, &payload).expect("must not error");
        assert!(replies.is_empty());
        assert!(capture.lock().unwrap().data.is_empty());
    }

    #[test]
    fn rdpsnd_renegotiation_refires_format() {
        let capture = Mutex::new(AudioCapture::default());
        let mut handler = audio_test_handler(&capture);
        negotiate_and_train(&mut handler);

        let wave = |block_no: u8| {
            encode_server_pdu(sndpdu::ServerAudioOutputPdu::Wave2(sndpdu::Wave2Pdu {
                timestamp: u16::from(block_no),
                format_no: 0,
                block_no,
                audio_timestamp: u32::from(block_no),
                data: vec![0u8; 4].into(),
            }))
        };
        handler.process(0, &wave(1)).expect("wave");
        negotiate_and_train(&mut handler);
        handler
            .process(0, &wave(2))
            .expect("wave after renegotiation");

        let capture = capture.lock().unwrap();
        assert_eq!(
            capture.formats,
            vec![
                (RDP_AUDIO_CODEC_OPUS, 48_000, 2),
                (RDP_AUDIO_CODEC_OPUS, 48_000, 2)
            ],
            "renegotiation must re-fire on_audio_format even for the same format"
        );
        assert_eq!(capture.data.len(), 2);
    }
}
