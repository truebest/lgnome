//! Shared raw-bytes DVC message wrapper.
//!
//! The DVC writer wants `DvcEncode` implementors; handlers that build wire
//! bytes by hand (RDPECAM, RDPEAI) wrap them here instead of each carrying a
//! private copy of the same impl.

#![forbid(unsafe_code)]

use ironrdp_core::{Encode, EncodeResult, WriteCursor};
use ironrdp_dvc::{DvcEncode, DvcMessage};

pub(super) struct RawDvcMessage {
    name: &'static str,
    bytes: Vec<u8>,
}

impl RawDvcMessage {
    pub(super) fn boxed(name: &'static str, bytes: Vec<u8>) -> DvcMessage {
        Box::new(Self { name, bytes })
    }
}

impl Encode for RawDvcMessage {
    fn encode(&self, dst: &mut WriteCursor<'_>) -> EncodeResult<()> {
        ironrdp_core::ensure_size!(in: dst, size: self.size());
        dst.write_slice(&self.bytes);
        Ok(())
    }

    fn name(&self) -> &'static str {
        self.name
    }

    fn size(&self) -> usize {
        self.bytes.len()
    }
}

impl DvcEncode for RawDvcMessage {}
