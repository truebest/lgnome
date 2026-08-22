//! Bounded camera/microphone mailbox and synchronous capture-close barrier.

use std::collections::VecDeque;
use std::sync::{Arc, Condvar, Mutex};

const AUDIO_INPUT_QUEUE_DEPTH: usize = 8;

#[derive(Debug, PartialEq, Eq)]
pub(super) enum CameraSubmission {
    H264 {
        generation: u64,
        data: Vec<u8>,
        capture_epoch: u64,
    },
    Error {
        generation: u64,
        capture_epoch: u64,
    },
    DeferredError {
        generation: u64,
    },
}

#[derive(Debug, PartialEq, Eq)]
pub(super) struct AudioInputSubmission {
    generation: u64,
    data: Vec<u8>,
    capture_epoch: u64,
}

#[derive(Default)]
struct MediaMailbox {
    capture_active: bool,
    capture_epoch: u64,
    camera: Option<CameraSubmission>,
    audio_input: VecDeque<AudioInputSubmission>,
    outbound_payloads: usize,
}

impl MediaMailbox {
    fn set_capture_active(&mut self, active: bool) -> bool {
        if self.capture_active == active {
            return false;
        }
        self.capture_active = active;
        self.capture_epoch = self.capture_epoch.wrapping_add(1);
        self.audio_input.clear();
        if !active {
            if let Some(submission) = self.camera.take() {
                let generation = match submission {
                    CameraSubmission::H264 { generation, .. }
                    | CameraSubmission::Error { generation, .. }
                    | CameraSubmission::DeferredError { generation } => generation,
                };
                self.camera = Some(CameraSubmission::DeferredError { generation });
            }
        }
        true
    }

    fn active_epoch(&self) -> Option<u64> {
        self.capture_active.then_some(self.capture_epoch)
    }

    fn epoch_is_active(&self, capture_epoch: u64) -> bool {
        self.capture_active && self.capture_epoch == capture_epoch
    }
}

#[derive(Default)]
pub(super) struct MediaGate {
    // A close may release the mailbox mutex while waiting for a payload write;
    // serialize transitions so a concurrent reopen cannot overtake that close.
    transition: Mutex<()>,
    mailbox: Mutex<MediaMailbox>,
    outbound_idle: Condvar,
}

pub(super) struct CaptureSendPermit {
    gate: Arc<MediaGate>,
}

pub(super) struct PreparedCameraSend {
    pub(super) submission: CameraSubmission,
    pub(super) permit: CaptureSendPermit,
}

pub(super) struct PreparedAudioSend {
    pub(super) generation: u64,
    pub(super) data: Vec<u8>,
    pub(super) permit: CaptureSendPermit,
}

pub(super) struct PendingMediaBatch {
    pub(super) camera: Option<CameraSubmission>,
    pub(super) audio_input: Vec<AudioInputSubmission>,
}

impl MediaGate {
    pub(super) fn set_capture_active(&self, active: bool) -> Result<bool, ()> {
        let _transition = self.transition.lock().map_err(|_| ())?;
        let mut mailbox = self.mailbox.lock().map_err(|_| ())?;
        let changed = mailbox.set_capture_active(active);
        if !active {
            while mailbox.outbound_payloads != 0 {
                mailbox = self.outbound_idle.wait(mailbox).map_err(|_| ())?;
            }
        }
        Ok(changed)
    }

    pub(super) fn try_enqueue_camera_h264(
        &self,
        generation: u64,
        data: Vec<u8>,
    ) -> Result<bool, ()> {
        let mut mailbox = self.mailbox.lock().map_err(|_| ())?;
        let Some(capture_epoch) = mailbox.active_epoch() else {
            return Ok(false);
        };
        if mailbox.camera.is_some() {
            return Ok(false);
        }
        mailbox.camera = Some(CameraSubmission::H264 {
            generation,
            data,
            capture_epoch,
        });
        Ok(true)
    }

    pub(super) fn try_enqueue_camera_error(&self, generation: u64) -> Result<bool, ()> {
        let mut mailbox = self.mailbox.lock().map_err(|_| ())?;
        let Some(capture_epoch) = mailbox.active_epoch() else {
            return Ok(false);
        };
        if mailbox.camera.is_some() {
            return Ok(false);
        }
        mailbox.camera = Some(CameraSubmission::Error {
            generation,
            capture_epoch,
        });
        Ok(true)
    }

    pub(super) fn try_enqueue_audio_pcm(&self, generation: u64, data: Vec<u8>) -> Result<bool, ()> {
        let mut mailbox = self.mailbox.lock().map_err(|_| ())?;
        let Some(capture_epoch) = mailbox.active_epoch() else {
            return Ok(false);
        };
        if mailbox.audio_input.len() == AUDIO_INPUT_QUEUE_DEPTH {
            mailbox.audio_input.pop_front();
        }
        mailbox.audio_input.push_back(AudioInputSubmission {
            generation,
            data,
            capture_epoch,
        });
        Ok(true)
    }

    pub(super) fn take_pending(&self) -> Result<PendingMediaBatch, ()> {
        let mut mailbox = self.mailbox.lock().map_err(|_| ())?;
        Ok(PendingMediaBatch {
            camera: mailbox.camera.take(),
            audio_input: mailbox.audio_input.drain(..).collect(),
        })
    }

    pub(super) fn prepare_camera_send(
        self: &Arc<Self>,
        submission: CameraSubmission,
    ) -> Result<Option<PreparedCameraSend>, ()> {
        let mut mailbox = self.mailbox.lock().map_err(|_| ())?;
        let submission = match submission {
            CameraSubmission::H264 {
                generation,
                data,
                capture_epoch,
            } if mailbox.epoch_is_active(capture_epoch) => CameraSubmission::H264 {
                generation,
                data,
                capture_epoch,
            },
            CameraSubmission::Error {
                generation,
                capture_epoch,
            } if mailbox.epoch_is_active(capture_epoch) => CameraSubmission::Error {
                generation,
                capture_epoch,
            },
            CameraSubmission::H264 { generation, .. }
            | CameraSubmission::Error { generation, .. }
            | CameraSubmission::DeferredError { generation } => {
                let Some(capture_epoch) = mailbox.active_epoch() else {
                    if mailbox.camera.is_none() {
                        mailbox.camera = Some(CameraSubmission::DeferredError { generation });
                    }
                    return Ok(None);
                };
                CameraSubmission::Error {
                    generation,
                    capture_epoch,
                }
            }
        };
        mailbox.outbound_payloads += 1;
        Ok(Some(PreparedCameraSend {
            submission,
            permit: CaptureSendPermit {
                gate: Arc::clone(self),
            },
        }))
    }

    pub(super) fn prepare_audio_send(
        self: &Arc<Self>,
        submission: AudioInputSubmission,
    ) -> Result<Option<PreparedAudioSend>, ()> {
        let mut mailbox = self.mailbox.lock().map_err(|_| ())?;
        if !mailbox.epoch_is_active(submission.capture_epoch) {
            return Ok(None);
        }
        mailbox.outbound_payloads += 1;
        Ok(Some(PreparedAudioSend {
            generation: submission.generation,
            data: submission.data,
            permit: CaptureSendPermit {
                gate: Arc::clone(self),
            },
        }))
    }

    pub(super) fn discard_camera_submission(&self) -> Result<bool, ()> {
        let mut mailbox = self.mailbox.lock().map_err(|_| ())?;
        Ok(mailbox.camera.take().is_some())
    }

    pub(super) fn capture_is_active(&self) -> Result<bool, ()> {
        self.mailbox
            .lock()
            .map(|mailbox| mailbox.capture_active)
            .map_err(|_| ())
    }
}

impl Drop for CaptureSendPermit {
    fn drop(&mut self) {
        let mut mailbox = match self.gate.mailbox.lock() {
            Ok(mailbox) => mailbox,
            Err(poisoned) => poisoned.into_inner(),
        };
        debug_assert!(mailbox.outbound_payloads > 0);
        mailbox.outbound_payloads = mailbox.outbound_payloads.saturating_sub(1);
        if mailbox.outbound_payloads == 0 {
            self.gate.outbound_idle.notify_all();
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn active_epoch(gate: &MediaGate) -> u64 {
        gate.mailbox.lock().unwrap().capture_epoch
    }

    #[test]
    fn capture_close_waits_for_validated_payload_write() {
        let gate = Arc::new(MediaGate::default());
        assert!(gate.set_capture_active(true).unwrap());
        let prepared = gate
            .prepare_camera_send(CameraSubmission::H264 {
                generation: 7,
                data: vec![1, 2, 3],
                capture_epoch: active_epoch(&gate),
            })
            .unwrap()
            .unwrap();
        let close_gate = Arc::clone(&gate);
        let close = std::thread::spawn(move || close_gate.set_capture_active(false).unwrap());
        loop {
            let mailbox = gate.mailbox.lock().unwrap();
            if !mailbox.capture_active {
                assert_eq!(mailbox.outbound_payloads, 1);
                break;
            }
            drop(mailbox);
            std::thread::yield_now();
        }
        assert!(!close.is_finished());
        drop(prepared.permit);
        assert!(close.join().unwrap());
        assert!(!gate.capture_is_active().unwrap());
    }

    #[test]
    fn capture_close_waits_for_prepared_error_write() {
        let gate = Arc::new(MediaGate::default());
        assert!(gate.set_capture_active(true).unwrap());
        let prepared = gate
            .prepare_camera_send(CameraSubmission::Error {
                generation: 11,
                capture_epoch: active_epoch(&gate),
            })
            .unwrap()
            .unwrap();
        let close_gate = Arc::clone(&gate);
        let close = std::thread::spawn(move || close_gate.set_capture_active(false).unwrap());
        while gate.mailbox.lock().unwrap().capture_active {
            std::thread::yield_now();
        }
        assert!(!close.is_finished());
        drop(prepared.permit);
        assert!(close.join().unwrap());
    }

    #[test]
    fn inactive_gate_defers_camera_answer_until_reopen() {
        for submission in [
            CameraSubmission::H264 {
                generation: 17,
                data: vec![1, 2, 3],
                capture_epoch: 1,
            },
            CameraSubmission::Error {
                generation: 19,
                capture_epoch: 1,
            },
        ] {
            let gate = Arc::new(MediaGate::default());
            assert!(gate.set_capture_active(true).unwrap());
            assert!(gate.set_capture_active(false).unwrap());
            assert!(gate.prepare_camera_send(submission).unwrap().is_none());
            assert!(matches!(
                gate.take_pending().unwrap().camera,
                Some(CameraSubmission::DeferredError { .. })
            ));
        }
    }

    #[test]
    fn stale_camera_answer_becomes_one_error_after_reopen() {
        let gate = Arc::new(MediaGate::default());
        assert!(gate.set_capture_active(true).unwrap());
        let taken = CameraSubmission::H264 {
            generation: 23,
            data: vec![1, 2, 3],
            capture_epoch: active_epoch(&gate),
        };
        assert!(gate.set_capture_active(false).unwrap());
        assert!(gate.prepare_camera_send(taken).unwrap().is_none());
        assert!(gate.set_capture_active(true).unwrap());
        let deferred = gate.take_pending().unwrap().camera.unwrap();
        let prepared = gate.prepare_camera_send(deferred).unwrap().unwrap();
        assert!(matches!(
            prepared.submission,
            CameraSubmission::Error { generation: 23, .. }
        ));
    }

    #[test]
    fn retirement_discards_every_camera_answer_kind() {
        for submission in [
            CameraSubmission::H264 {
                generation: 29,
                data: vec![1, 2, 3],
                capture_epoch: 1,
            },
            CameraSubmission::Error {
                generation: 31,
                capture_epoch: 1,
            },
            CameraSubmission::DeferredError { generation: 37 },
        ] {
            let gate = MediaGate::default();
            gate.mailbox.lock().unwrap().camera = Some(submission);
            assert!(gate.discard_camera_submission().unwrap());
            assert!(!gate.discard_camera_submission().unwrap());
        }
    }

    #[test]
    fn audio_queue_drops_oldest_packet_at_capacity() {
        let gate = MediaGate::default();
        assert!(gate.set_capture_active(true).unwrap());
        for generation in 0..=AUDIO_INPUT_QUEUE_DEPTH as u64 {
            assert!(gate
                .try_enqueue_audio_pcm(generation, vec![generation as u8])
                .unwrap());
        }
        let batch = gate.take_pending().unwrap();
        assert_eq!(batch.audio_input.len(), AUDIO_INPUT_QUEUE_DEPTH);
        assert_eq!(batch.audio_input[0].generation, 1);
        assert_eq!(batch.audio_input.last().unwrap().generation, 8);
    }

    #[test]
    fn camera_mailbox_retains_only_one_pending_response() {
        let gate = MediaGate::default();
        assert!(gate.set_capture_active(true).unwrap());
        assert!(gate.try_enqueue_camera_h264(41, vec![1]).unwrap());
        assert!(!gate.try_enqueue_camera_error(43).unwrap());
        assert!(matches!(
            gate.take_pending().unwrap().camera,
            Some(CameraSubmission::H264 { generation: 41, .. })
        ));
    }
}
