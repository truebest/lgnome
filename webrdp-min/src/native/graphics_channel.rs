//! Keep IronRDP's ACK payloads, but release each only after its callback boundary.
use std::collections::VecDeque;
use std::sync::{Arc, Mutex};

use ironrdp_core::{decode, encode_vec, impl_as_any};
use ironrdp_dvc::{DvcClientProcessor, DvcMessage, DvcProcessor};
use ironrdp_egfx::client::{GraphicsPipelineClient, GraphicsPipelineHandler};
use ironrdp_egfx::pdu::GfxPdu;
use ironrdp_pdu::{decode_err, encode_err, pdu_other_err, PduResult};

use super::egfx::NativeGfxState;

pub(super) struct NativeGraphicsChannel {
    inner: GraphicsPipelineClient,
    shared: Arc<Mutex<NativeGfxState>>,
    pending: VecDeque<(u64, u64, DvcMessage)>,
}
impl NativeGraphicsChannel {
    pub(super) fn new(
        handler: Box<dyn GraphicsPipelineHandler>,
        shared: Arc<Mutex<NativeGfxState>>,
    ) -> Self {
        Self {
            inner: GraphicsPipelineClient::new(handler, None),
            shared,
            pending: VecDeque::new(),
        }
    }
    pub(super) fn take_output_reset(&mut self) -> Option<(u16, u16)> {
        self.inner.take_output_reset()
    }
    pub(super) fn request_protocol_reset(&mut self) -> Option<GfxPdu> {
        let pdu = self.inner.request_protocol_reset()?;
        self.pending.clear();
        Some(pdu)
    }
    pub(super) fn take_ready(&mut self) -> Vec<DvcMessage> {
        let (generation, completed) = self.shared.lock().unwrap().queue.progress();
        let mut ready = Vec::new();
        while let Some((epoch, sequence, _)) = self.pending.front() {
            if *epoch != generation {
                self.pending.pop_front();
                continue;
            }
            if *sequence > completed {
                break;
            }
            ready.push(self.pending.pop_front().unwrap().2);
        }
        ready
    }
}
impl_as_any!(NativeGraphicsChannel);
impl DvcProcessor for NativeGraphicsChannel {
    fn channel_name(&self) -> &str {
        self.inner.channel_name()
    }
    fn maximum_message_size(&self) -> usize {
        self.inner.maximum_message_size()
    }
    fn start(&mut self, id: u32) -> PduResult<Vec<DvcMessage>> {
        self.inner.start(id)
    }
    fn close(&mut self, id: u32) {
        self.inner.close(id);
        self.pending.clear();
    }
    fn process(&mut self, id: u32, payload: &[u8]) -> PduResult<Vec<DvcMessage>> {
        let responses = self.inner.process(id, payload)?;
        let mut shared = self.shared.lock().unwrap();
        if let Some(error) = shared.queue.error() {
            return Err(pdu_other_err!(error));
        }
        let mut immediate = Vec::new();
        for response in responses {
            let encoded = encode_vec(response.as_ref()).map_err(|e| encode_err!(e))?;
            let pdu: GfxPdu = decode(&encoded).map_err(|e| decode_err!(e))?;
            if let GfxPdu::FrameAcknowledge(ack) = pdu {
                let Some((frame, generation, sequence)) = shared.frames.pop_front() else {
                    return Err(pdu_other_err!("missing graphics completion boundary"));
                };
                if frame != ack.frame_id || self.pending.len() >= 256 {
                    return Err(pdu_other_err!("graphics ACK order or queue limit violated"));
                }
                self.pending.push_back((generation, sequence, response));
            } else {
                immediate.push(response);
            }
        }
        Ok(immediate)
    }
}
impl DvcClientProcessor for NativeGraphicsChannel {}

#[cfg(test)]
mod tests {
    use super::super::{egfx::NativeGfxHandler, graphics::Event, CallbackSink};
    use super::*;
    use ironrdp_egfx::pdu::{
        CapabilitiesConfirmPdu, CapabilitiesV107Flags, CapabilitySet, EndFramePdu,
    };
    use std::time::Duration;

    fn process(channel: &mut NativeGraphicsChannel, pdus: &[GfxPdu]) -> Vec<DvcMessage> {
        let bytes: Vec<u8> = pdus
            .iter()
            .flat_map(|pdu| encode_vec(pdu).unwrap())
            .collect();
        channel
            .process(7, &ironrdp_graphics::zgfx::wrap_uncompressed(&bytes))
            .unwrap()
    }
    #[test]
    fn ack_waits_for_its_boundary_and_stale_generations_never_ack() {
        let shared = Arc::new(Mutex::new(NativeGfxState::default()));
        let queue = shared.lock().unwrap().queue.clone();
        let mut channel = NativeGraphicsChannel::new(
            Box::new(NativeGfxHandler::new(shared.clone(), false)),
            shared,
        );
        channel.start(7).unwrap();
        process(
            &mut channel,
            &[GfxPdu::CapabilitiesConfirm(
                CapabilitiesConfirmPdu::from_typed(&CapabilitySet::V10_7 {
                    flags: CapabilitiesV107Flags::SMALL_CACHE,
                }),
            )],
        );
        queue.submit(Event::Desktop(1920, 1080));
        assert!(process(
            &mut channel,
            &[
                GfxPdu::EndFrame(EndFramePdu { frame_id: 11 }),
                GfxPdu::EndFrame(EndFramePdu { frame_id: 12 })
            ]
        )
        .is_empty());
        assert!(channel.take_ready().is_empty());
        let executor = queue.start(CallbackSink::default()).unwrap();
        assert!(queue.barrier(Duration::from_secs(1)));
        let ready = channel.take_ready();
        let frames: Vec<_> = ready
            .iter()
            .map(|m| {
                let GfxPdu::FrameAcknowledge(ack) =
                    decode::<GfxPdu>(&encode_vec(m.as_ref()).unwrap()).unwrap()
                else {
                    panic!()
                };
                assert!(!matches!(
                    ack.queue_depth,
                    ironrdp_egfx::pdu::QueueDepth::Suspend
                ));
                ack.frame_id
            })
            .collect();
        assert_eq!(frames, [11, 12]);
        process(
            &mut channel,
            &[GfxPdu::EndFrame(EndFramePdu { frame_id: 13 })],
        );
        queue.reset(false);
        assert!(channel.take_ready().is_empty());
        drop(executor);
    }

    #[test]
    fn reset_in_one_batch_discards_only_the_old_frame_ack() {
        let shared = Arc::new(Mutex::new(NativeGfxState::default()));
        let queue = shared.lock().unwrap().queue.clone();
        let mut channel = NativeGraphicsChannel::new(
            Box::new(NativeGfxHandler::new(shared.clone(), false)),
            shared,
        );
        channel.start(7).unwrap();
        process(
            &mut channel,
            &[
                GfxPdu::CapabilitiesConfirm(CapabilitiesConfirmPdu::from_typed(
                    &CapabilitySet::V10_7 {
                        flags: CapabilitiesV107Flags::SMALL_CACHE,
                    },
                )),
                GfxPdu::EndFrame(EndFramePdu { frame_id: 11 }),
                GfxPdu::ResetGraphics(ironrdp_egfx::pdu::ResetGraphicsPdu {
                    width: 640,
                    height: 480,
                    monitors: vec![],
                }),
                GfxPdu::EndFrame(EndFramePdu { frame_id: 12 }),
            ],
        );
        assert!(channel.take_ready().is_empty());
        let executor = queue.start(CallbackSink::default()).unwrap();
        assert!(queue.barrier(Duration::from_secs(1)));
        let ready = channel.take_ready();
        assert_eq!(ready.len(), 1);
        let GfxPdu::FrameAcknowledge(ack) =
            decode::<GfxPdu>(&encode_vec(ready[0].as_ref()).unwrap()).unwrap()
        else {
            panic!()
        };
        assert_eq!(ack.frame_id, 12);
        drop(executor);
    }
}
