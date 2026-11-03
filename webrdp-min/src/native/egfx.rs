//! EGFX graphics pipeline: H.264/HEVC access units and RemoteFX/bitmap RGBA
//! updates, accumulated under a lock by IronRDP's handler callbacks and
//! drained by the worker between socket reads.

#![forbid(unsafe_code)]

use super::graphics::{Event, GraphicsQueue};
use std::collections::{HashMap, VecDeque};
use std::sync::{Arc, Mutex};

use super::abi::{RDP_VIDEO_CODEC_H264, RDP_VIDEO_CODEC_H265, RDP_VIDEO_FLAG_AVC444_AUX};
use super::LOG_TARGET_GRAPHICS;
use ironrdp_egfx::client::{BitmapUpdate, GraphicsPipelineHandler};
use ironrdp_egfx::pdu::{
    CacheToSurfacePdu, CapabilitiesFrdp1Flags, CapabilitiesV107Flags, CapabilitiesV81Flags,
    CapabilitiesV8Flags, CapabilitySet, Codec1Type, GfxPdu, SolidFillPdu, SurfaceToSurfacePdu,
};

#[derive(Default)]
pub(super) struct NativeGfxState {
    pub(super) queue: Arc<GraphicsQueue>,
    pub(super) frames: VecDeque<(u32, u64, u64)>,
    pub(super) unsupported_graphics: Option<String>,
    // ResetGraphics dimensions, independent of the initial MCS/GCC desktop size.
    pub(super) graphics_width: u32,
    pub(super) graphics_height: u32,
    // Add the mapped output origin to surface-local bitmap rectangles.
    pub(super) surface_origins: HashMap<u16, (u32, u32)>,
    pub(super) avc444_aux_frames: u64,
}

#[derive(Clone)]
pub(super) struct NativeVideoUnit {
    pub(super) codec: u32,
    pub(super) flags: u32,
    pub(super) data: Vec<u8>,
}

#[derive(Clone)]
pub(super) struct NativeBitmapUnit {
    pub(super) surface_id: u16,
    pub(super) left: u32,
    pub(super) top: u32,
    pub(super) width: u32,
    pub(super) height: u32,
    pub(super) stride: u32,
    pub(super) data: Vec<u8>,
}

pub(super) struct NativeGfxHandler {
    pub(super) shared: Arc<Mutex<NativeGfxState>>,
    hevc_capable: bool,
    hevc_negotiated: bool,
    undecodable_since_render: u32,
    /// Session-wide log counter; successful renders must not reset the limiter.
    undecodable_total: u32,
    video_seen: bool,
    bitmap_seen: bool,
}

const MAX_UNDECODABLE_UPDATES: u32 = 32;

const UNDECODABLE_WARN_INTERVAL: u32 = 64;

fn gfx_pdu_name(pdu: &GfxPdu) -> &'static str {
    match pdu {
        GfxPdu::WireToSurface1(_) => "WireToSurface1",
        GfxPdu::WireToSurface2(_) => "WireToSurface2",
        GfxPdu::DeleteEncodingContext(_) => "DeleteEncodingContext",
        GfxPdu::SolidFill(_) => "SolidFill",
        GfxPdu::SurfaceToSurface(_) => "SurfaceToSurface",
        GfxPdu::SurfaceToCache(_) => "SurfaceToCache",
        GfxPdu::CacheToSurface(_) => "CacheToSurface",
        GfxPdu::EvictCacheEntry(_) => "EvictCacheEntry",
        GfxPdu::CreateSurface(_) => "CreateSurface",
        GfxPdu::DeleteSurface(_) => "DeleteSurface",
        GfxPdu::StartFrame(_) => "StartFrame",
        GfxPdu::EndFrame(_) => "EndFrame",
        GfxPdu::FrameAcknowledge(_) => "FrameAcknowledge",
        GfxPdu::ResetGraphics(_) => "ResetGraphics",
        GfxPdu::MapSurfaceToOutput(_) => "MapSurfaceToOutput",
        GfxPdu::CacheImportOffer(_) => "CacheImportOffer",
        GfxPdu::CacheImportReply(_) => "CacheImportReply",
        GfxPdu::CapabilitiesAdvertise(_) => "CapabilitiesAdvertise",
        GfxPdu::CapabilitiesConfirm(_) => "CapabilitiesConfirm",
        GfxPdu::MapSurfaceToWindow(_) => "MapSurfaceToWindow",
        GfxPdu::QoeFrameAcknowledge(_) => "QoeFrameAcknowledge",
        GfxPdu::MapSurfaceToScaledOutput(_) => "MapSurfaceToScaledOutput",
        GfxPdu::MapSurfaceToScaledWindow(_) => "MapSurfaceToScaledWindow",
        _ => "unknown",
    }
}

impl NativeGfxHandler {
    pub(super) fn new(shared: Arc<Mutex<NativeGfxState>>, hevc_capable: bool) -> Self {
        Self {
            shared,
            hevc_capable,
            hevc_negotiated: false,
            undecodable_since_render: 0,
            undecodable_total: 0,
            video_seen: false,
            bitmap_seen: false,
        }
    }

    fn note_rendered_update(&mut self) {
        self.undecodable_since_render = 0;
    }

    fn note_undecodable_update(&mut self, pdu: &GfxPdu) {
        let codec = match pdu {
            GfxPdu::WireToSurface1(wire) => format!("{:?}", wire.codec_id),
            GfxPdu::WireToSurface2(_) => "RemoteFxProgressive".to_owned(),
            _ => "n/a".to_owned(),
        };
        self.undecodable_since_render = self.undecodable_since_render.saturating_add(1);
        self.undecodable_total = self.undecodable_total.saturating_add(1);
        if self.undecodable_total == 1
            || self
                .undecodable_total
                .is_multiple_of(UNDECODABLE_WARN_INTERVAL)
        {
            tracing::warn!(
                target: LOG_TARGET_GRAPHICS,
                pdu = gfx_pdu_name(pdu),
                codec,
                dropped = self.undecodable_total,
                "dropping an EGFX update this client cannot decode"
            );
        }
        if self.undecodable_since_render >= MAX_UNDECODABLE_UPDATES {
            self.mark_unsupported_graphics(
                "server sent only EGFX updates this client cannot decode",
            );
        }
    }

    fn mark_unsupported_graphics(&self, detail: impl Into<String>) {
        if let Ok(mut shared) = self.shared.lock() {
            if shared.unsupported_graphics.is_none() {
                shared.unsupported_graphics = Some(detail.into());
            }
        }
    }
}

impl GraphicsPipelineHandler for NativeGfxHandler {
    fn capabilities(&self) -> Vec<CapabilitySet> {
        // Tested Windows hosts need V10 for H.264; AVC_THIN_CLIENT prefers YUV444.
        let mut caps = vec![
            CapabilitySet::V8 {
                flags: CapabilitiesV8Flags::SMALL_CACHE,
            },
            CapabilitySet::V8_1 {
                flags: CapabilitiesV81Flags::AVC420_ENABLED | CapabilitiesV81Flags::SMALL_CACHE,
            },
            CapabilitySet::V10_7 {
                flags: CapabilitiesV107Flags::SMALL_CACHE | CapabilitiesV107Flags::AVC_THIN_CLIENT,
            },
        ];
        if self.hevc_capable {
            caps.insert(
                0,
                CapabilitySet::Frdp1 {
                    flags: CapabilitiesFrdp1Flags::HEVC_SUPPORTED
                        | CapabilitiesFrdp1Flags::SMALL_CACHE
                        | CapabilitiesFrdp1Flags::AVC_THIN_CLIENT,
                },
            );
        }
        tracing::info!(target: LOG_TARGET_GRAPHICS, offered = ?caps,
            "EGFX offer: experimental AVC444 main+aux hardware passthrough; no chroma reconstruction");
        caps
    }

    fn on_capabilities_confirmed(&mut self, caps: &CapabilitySet) {
        self.hevc_negotiated = self.hevc_capable
            && matches!(caps,
            CapabilitySet::Frdp1 { flags } if flags.contains(CapabilitiesFrdp1Flags::HEVC_SUPPORTED));
        tracing::info!(
            target: LOG_TARGET_GRAPHICS,
            caps = ?caps,
            "EGFX capabilities confirmed"
        );
    }

    fn on_reset_graphics(&mut self, width: u32, height: u32) {
        // Log the first payload again after each graphics reset, not every frame.
        tracing::info!(target: LOG_TARGET_GRAPHICS, width, height, "EGFX ResetGraphics received");
        self.video_seen = false;
        self.bitmap_seen = false;
        self.undecodable_since_render = 0;
        if let Ok(mut shared) = self.shared.lock() {
            shared.queue.reset(false);
            // ResetGraphics implicitly destroys all surfaces, so any tracked mapping origin is
            // stale afterward.
            shared.surface_origins.clear();
            shared.graphics_width = width;
            shared.graphics_height = height;
            if width == 0
                || height == 0
                || width > u32::from(u16::MAX)
                || height > u32::from(u16::MAX)
                || u64::from(width) * u64::from(height) * 4 > 64 * 1024 * 1024
            {
                shared.unsupported_graphics = Some("invalid graphics dimensions".into());
            } else {
                shared
                    .queue
                    .submit(Event::Desktop(width as u16, height as u16));
            }
        }
    }

    fn on_surface_mapped(&mut self, surface_id: u16, origin_x: u32, origin_y: u32) {
        tracing::debug!(target: LOG_TARGET_GRAPHICS, surface_id, origin_x, origin_y,
            "EGFX surface mapped to output");
        if let Ok(mut shared) = self.shared.lock() {
            shared
                .surface_origins
                .insert(surface_id, (origin_x, origin_y));
        }
    }

    fn on_surface_deleted(&mut self, surface_id: u16) {
        // DeleteSurface destroys the surface just like ResetGraphics does; a later surface
        // reusing this id must not inherit the stale mapping origin before its own
        // MapSurfaceToOutput arrives.
        if let Ok(mut shared) = self.shared.lock() {
            shared.surface_origins.remove(&surface_id);
        }
    }

    fn on_bitmap_updated(&mut self, update: &BitmapUpdate) {
        if update.data.is_empty() || update.width == 0 || update.height == 0 {
            self.mark_unsupported_graphics(format!(
                "server produced empty bitmap update ({:?})",
                update.codec_id
            ));
            return;
        }
        if !self.bitmap_seen {
            tracing::info!(target: LOG_TARGET_GRAPHICS, codec = ?update.codec_id,
                surface_id = update.surface_id, width = update.width, height = update.height,
                "first decoded bitmap rectangle since graphics reset (not full desktop size)");
            self.bitmap_seen = true;
        }
        let stride = u32::from(update.width) * 4;
        // u64: `stride * height` overflows usize on the 32-bit webOS target for
        // dimensions a hostile server can still claim (e.g. 32768x32768).
        let expected = u64::from(stride) * u64::from(update.height);
        if (update.data.len() as u64) < expected {
            self.mark_unsupported_graphics(format!(
                "server produced short bitmap update ({:?}, {} < {})",
                update.codec_id,
                update.data.len(),
                expected
            ));
            return;
        }
        if let Ok(shared) = self.shared.lock() {
            // `destination_rectangle` is surface-local; a surface mapped away from (0,0) via
            // RDPGFX_MAP_SURFACE_TO_OUTPUT_PDU needs that origin added before this lands on the
            // desktop-relative native RGBA canvas.
            let (origin_x, origin_y) = shared
                .surface_origins
                .get(&update.surface_id)
                .copied()
                .unwrap_or((0, 0));
            shared.queue.submit(Event::Bitmap(NativeBitmapUnit {
                surface_id: update.surface_id,
                left: u32::from(update.destination_rectangle.left) + origin_x,
                top: u32::from(update.destination_rectangle.top) + origin_y,
                width: u32::from(update.width),
                height: u32::from(update.height),
                stride,
                data: update.data.clone(),
            }));
        }
        self.note_rendered_update();
    }

    fn on_avc420_frame(
        &mut self,
        _surface_id: u16,
        _left: u16,
        _top: u16,
        _width: u16,
        _height: u16,
        nal: &[u8],
    ) -> bool {
        // Frame rectangles are not desktop dimensions; ResetGraphics supplies those.
        if !nal.is_empty() {
            if !self.video_seen {
                tracing::info!(target: LOG_TARGET_GRAPHICS, surface_id = _surface_id,
                    left = _left, top = _top, width = _width, height = _height, bytes = nal.len(),
                    "first H.264 YUV420 AU since graphics reset; rectangle is not desktop size; may be AVC444 base view");
                self.video_seen = true;
            }
            if let Ok(shared) = self.shared.lock() {
                shared.queue.submit_video(RDP_VIDEO_CODEC_H264, 0, nal);
            }
            self.note_rendered_update();
        }
        true
    }

    fn on_hevc_frame(
        &mut self,
        _surface_id: u16,
        _left: u16,
        _top: u16,
        _width: u16,
        _height: u16,
        nal: &[u8],
    ) -> bool {
        if !self.hevc_negotiated {
            self.mark_unsupported_graphics("server sent HEVC without negotiating it");
            return true;
        }
        if !nal.is_empty() {
            if !self.video_seen {
                tracing::info!(target: LOG_TARGET_GRAPHICS, bytes = nal.len(),
                    "first HEVC AU since graphics reset");
                self.video_seen = true;
            }
            if let Ok(shared) = self.shared.lock() {
                shared.queue.submit_video(RDP_VIDEO_CODEC_H265, 0, nal);
            }
            self.note_rendered_update();
        }
        true
    }

    // Main and auxiliary pictures share the same H.264 reference chain.
    fn on_avc444_aux_frame(&mut self, nal: &[u8]) -> bool {
        if let Ok(mut shared) = self.shared.lock() {
            shared.avc444_aux_frames += 1;
            shared
                .queue
                .submit_video(RDP_VIDEO_CODEC_H264, RDP_VIDEO_FLAG_AVC444_AUX, nal);
            if shared.avc444_aux_frames == 1 {
                tracing::warn!(
                    target: LOG_TARGET_GRAPHICS,
                    "AVC444 experiment: forwarding all main/aux AUs to one decoder in wire order; native output filtering required"
                );
            }
        }
        true
    }

    fn on_frame_complete(&mut self, frame_id: u32) {
        if let Ok(mut shared) = self.shared.lock() {
            if shared.frames.len() >= 256 {
                shared.unsupported_graphics = Some("graphics frame boundary overflow".into());
            } else if let Some((generation, sequence)) = shared.queue.submit(Event::Frame) {
                shared.frames.push_back((frame_id, generation, sequence));
            }
        }
    }

    fn on_close(&mut self) {
        if let Ok(mut shared) = self.shared.lock() {
            shared.queue.reset(false);
            shared.frames.clear();
        }
    }

    fn wants_avc420_passthrough(&self) -> bool {
        true
    }

    /// Callbacks present directly; composited surfaces are unused.
    fn wants_composited_output(&self) -> bool {
        false
    }

    // Hardware H.264 ignores surface composition. Bitmap support requires canvas operations, not session failure.
    fn on_solid_fill(&mut self, pdu: &SolidFillPdu) {
        tracing::debug!(
            target: LOG_TARGET_GRAPHICS,
            surface_id = pdu.surface_id,
            "ignoring EGFX SolidFill"
        );
    }

    fn on_surface_to_surface(&mut self, pdu: &SurfaceToSurfacePdu) {
        tracing::debug!(
            target: LOG_TARGET_GRAPHICS,
            src = pdu.source_surface_id,
            dst = pdu.destination_surface_id,
            "ignoring EGFX SurfaceToSurface"
        );
    }

    fn on_cache_to_surface(&mut self, pdu: &CacheToSurfacePdu) {
        tracing::debug!(
            target: LOG_TARGET_GRAPHICS,
            slot = pdu.cache_slot,
            surface_id = pdu.surface_id,
            "ignoring EGFX CacheToSurface"
        );
    }

    // Unhandled AVC444 cannot be presented; other codecs may coexist with decodable updates.
    fn on_unhandled_pdu(&mut self, pdu: &GfxPdu) {
        match pdu {
            GfxPdu::WireToSurface1(wire)
                if matches!(wire.codec_id, Codec1Type::Avc444 | Codec1Type::Avc444v2) =>
            {
                self.mark_unsupported_graphics("server produced unsupported AVC444 EGFX PDU");
            }
            GfxPdu::WireToSurface1(_) | GfxPdu::WireToSurface2(_) => {
                self.note_undecodable_update(pdu);
            }
            other => {
                tracing::warn!(
                    target: LOG_TARGET_GRAPHICS,
                    pdu = gfx_pdu_name(other),
                    "ignoring unhandled EGFX PDU"
                );
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn auxiliary_aus_retain_wire_order_including_delayed_chroma() {
        let shared = Arc::new(Mutex::new(NativeGfxState::default()));
        let mut handler = NativeGfxHandler::new(Arc::clone(&shared), false);
        handler.on_avc420_frame(0, 0, 0, 16, 16, &[1]);
        handler.on_avc444_aux_frame(&[2]);
        handler.on_avc420_frame(0, 0, 0, 16, 16, &[3]);
        handler.on_avc444_aux_frame(&[4]);
        handler.on_avc444_aux_frame(&[5]);
        let state = shared.lock().unwrap();
        assert_eq!(state.queue.videos().len(), 5);
        for (index, unit) in state.queue.videos().iter().enumerate() {
            assert_eq!(unit.codec, RDP_VIDEO_CODEC_H264);
            assert_eq!(
                unit.flags,
                if index == 1 || index >= 3 {
                    RDP_VIDEO_FLAG_AVC444_AUX
                } else {
                    0
                }
            );
            assert_eq!(unit.data, vec![index as u8 + 1]);
        }
    }
    use ironrdp_pdu::geometry::ExclusiveRectangle;

    #[test]
    fn surface_mutating_egfx_ops_stay_non_fatal() {
        use ironrdp_egfx::pdu::{Color, Point};

        let cases: [(&str, fn(&mut NativeGfxHandler)); 3] = [
            ("SolidFill", |handler| {
                handler.on_solid_fill(&SolidFillPdu {
                    surface_id: 1,
                    fill_pixel: Color {
                        b: 0,
                        g: 0,
                        r: 0,
                        xa: 0xff,
                    },
                    rectangles: Vec::new(),
                })
            }),
            ("SurfaceToSurface", |handler| {
                handler.on_surface_to_surface(&SurfaceToSurfacePdu {
                    source_surface_id: 1,
                    destination_surface_id: 2,
                    source_rectangle: ExclusiveRectangle {
                        left: 0,
                        top: 0,
                        right: 1,
                        bottom: 1,
                    },
                    destination_points: Vec::new(),
                })
            }),
            ("CacheToSurface", |handler| {
                handler.on_cache_to_surface(&CacheToSurfacePdu {
                    cache_slot: 1,
                    surface_id: 1,
                    destination_points: vec![Point { x: 0, y: 0 }],
                })
            }),
        ];
        for (name, invoke) in cases {
            let mut handler =
                NativeGfxHandler::new(Arc::new(Mutex::new(NativeGfxState::default())), false);
            invoke(&mut handler);
            // gnome-remote-desktop sends these during routine AVC420 sessions; they must
            // never take the session down (regression: they briefly did).
            assert!(
                handler
                    .shared
                    .lock()
                    .unwrap()
                    .unsupported_graphics
                    .is_none(),
                "{name} must not mark graphics unsupported"
            );
        }
    }

    fn undecodable_wire_to_surface1() -> GfxPdu {
        use ironrdp_egfx::pdu::{PixelFormat, WireToSurface1Pdu};

        GfxPdu::WireToSurface1(WireToSurface1Pdu {
            surface_id: 1,
            codec_id: Codec1Type::RemoteFx,
            pixel_format: PixelFormat::XRgb,
            destination_rectangle: ExclusiveRectangle {
                left: 0,
                top: 0,
                right: 64,
                bottom: 64,
            },
            bitmap_data: vec![0xFF; 16],
        })
    }

    #[test]
    fn reset_graphics_invalidates_queued_data_even_at_the_same_size() {
        let shared = Arc::new(Mutex::new(NativeGfxState::default()));
        let mut handler = NativeGfxHandler::new(shared.clone(), false);
        handler.on_reset_graphics(3840, 2160);
        let generation = shared.lock().unwrap().queue.progress().0;
        handler.on_avc420_frame(0, 0, 0, 16, 16, &[1]);
        handler.on_reset_graphics(3840, 2160);
        let state = shared.lock().unwrap();
        assert_eq!(state.queue.progress().0, generation + 1);
        assert!(state.queue.videos().is_empty());
        assert_eq!((state.graphics_width, state.graphics_height), (3840, 2160));
    }

    #[test]
    fn a_stream_that_only_ever_drops_updates_fails_the_session_over() {
        let mut handler =
            NativeGfxHandler::new(Arc::new(Mutex::new(NativeGfxState::default())), false);
        let pdu = undecodable_wire_to_surface1();

        for _ in 0..MAX_UNDECODABLE_UPDATES - 1 {
            handler.on_unhandled_pdu(&pdu);
        }
        assert!(
            handler
                .shared
                .lock()
                .unwrap()
                .unsupported_graphics
                .is_none(),
            "a server may open with updates this client skips"
        );

        handler.on_unhandled_pdu(&pdu);
        assert!(
            handler
                .shared
                .lock()
                .unwrap()
                .unsupported_graphics
                .is_some(),
            "a server that never renders must not hold a blank session"
        );
    }

    #[test]
    fn dropped_updates_stay_non_fatal_while_the_session_renders() {
        let mut handler =
            NativeGfxHandler::new(Arc::new(Mutex::new(NativeGfxState::default())), false);
        let pdu = undecodable_wire_to_surface1();

        // Windows mixes tiles this client cannot decode into a working AVC420 stream.
        let alternating = MAX_UNDECODABLE_UPDATES * 4;
        for _ in 0..alternating {
            handler.on_unhandled_pdu(&pdu);
            handler.on_avc420_frame(1, 0, 0, 64, 64, &[0u8; 4]);
        }
        assert!(handler
            .shared
            .lock()
            .unwrap()
            .unsupported_graphics
            .is_none());

        // The rendered updates in between must not restart the warning interval, or this
        // per-frame path warns on every single drop.
        assert_eq!(handler.undecodable_total, alternating);
        assert_eq!(handler.undecodable_since_render, 0);

        // A server that switches to a codec this client cannot decode freezes the picture
        // no matter how much it rendered before, so the run alone decides.
        for _ in 0..MAX_UNDECODABLE_UPDATES {
            handler.on_unhandled_pdu(&pdu);
        }
        assert!(handler
            .shared
            .lock()
            .unwrap()
            .unsupported_graphics
            .is_some());
    }

    #[test]
    fn bitmap_update_offsets_by_mapped_surface_origin() {
        let mut handler =
            NativeGfxHandler::new(Arc::new(Mutex::new(NativeGfxState::default())), false);

        // Server maps surface 7 at a non-zero desktop position (e.g. a second monitor).
        handler.on_surface_mapped(7, 1920, 100);

        handler.on_bitmap_updated(&BitmapUpdate::new(
            7,
            ExclusiveRectangle {
                left: 10,
                top: 20,
                right: 12,
                bottom: 22,
            },
            Codec1Type::Uncompressed,
            vec![0u8; 2 * 2 * 4],
            2,
            2,
        ));

        let shared = handler.shared.lock().unwrap();
        assert_eq!(shared.queue.bitmaps().len(), 1);
        assert_eq!(shared.queue.bitmaps()[0].left, 1920 + 10);
        assert_eq!(shared.queue.bitmaps()[0].top, 100 + 20);
        drop(shared);

        // A ResetGraphics implicitly destroys all surfaces; a stale origin must not leak into
        // a later bitmap update on a reused surface id that hasn't been re-mapped.
        handler.on_reset_graphics(3840, 2160);
        {
            let shared = handler.shared.lock().unwrap();
            shared.queue.reset(false);
        }
        handler.on_bitmap_updated(&BitmapUpdate::new(
            7,
            ExclusiveRectangle {
                left: 10,
                top: 20,
                right: 12,
                bottom: 22,
            },
            Codec1Type::Uncompressed,
            vec![0u8; 2 * 2 * 4],
            2,
            2,
        ));
        {
            let shared = handler.shared.lock().unwrap();
            assert_eq!(shared.queue.bitmaps()[0].left, 10);
            assert_eq!(shared.queue.bitmaps()[0].top, 20);
        }

        // DeleteSurface destroys a single surface the same way; a reused id must start
        // unmapped rather than inherit the deleted surface's origin.
        handler.on_surface_mapped(7, 500, 600);
        handler.on_surface_deleted(7);
        {
            let shared = handler.shared.lock().unwrap();
            shared.queue.reset(false);
        }
        handler.on_bitmap_updated(&BitmapUpdate::new(
            7,
            ExclusiveRectangle {
                left: 10,
                top: 20,
                right: 12,
                bottom: 22,
            },
            Codec1Type::Uncompressed,
            vec![0u8; 2 * 2 * 4],
            2,
            2,
        ));
        let shared = handler.shared.lock().unwrap();
        assert_eq!(
            shared.queue.bitmaps()[0].left,
            10,
            "deleted surface's origin must not leak"
        );
        assert_eq!(shared.queue.bitmaps()[0].top, 20);
    }

    #[test]
    fn gfx_capabilities_retain_v10_avc444_preference() {
        let handler = NativeGfxHandler::new(Arc::new(Mutex::new(NativeGfxState::default())), false);

        let capabilities = handler.capabilities();
        assert_eq!(
            capabilities[1],
            CapabilitySet::V8_1 {
                flags: CapabilitiesV81Flags::AVC420_ENABLED | CapabilitiesV81Flags::SMALL_CACHE,
            }
        );
        assert_eq!(capabilities.len(), 3);
        assert_eq!(
            capabilities[2],
            CapabilitySet::V10_7 {
                flags: CapabilitiesV107Flags::SMALL_CACHE | CapabilitiesV107Flags::AVC_THIN_CLIENT,
            }
        );
        assert!(handler.wants_avc420_passthrough());
    }

    #[test]
    fn hevc_offer_retains_fallbacks_and_frames_keep_their_codec() {
        let shared = Arc::new(Mutex::new(NativeGfxState::default()));
        let mut handler = NativeGfxHandler::new(Arc::clone(&shared), true);
        let fallback = NativeGfxHandler::new(Arc::clone(&shared), false).capabilities();
        let caps = handler.capabilities();
        assert_eq!(&caps[1..], fallback.as_slice());
        assert!(matches!(caps[0], CapabilitySet::Frdp1 { flags }
            if flags.contains(CapabilitiesFrdp1Flags::HEVC_SUPPORTED | CapabilitiesFrdp1Flags::AVC_THIN_CLIENT)));
        handler.on_capabilities_confirmed(&caps[0]);
        assert!(handler.on_hevc_frame(1, 0, 0, 16, 16, &[0, 0, 1, 38, 1]));
        handler.on_capabilities_confirmed(&fallback[1]);
        assert!(handler.on_avc420_frame(1, 0, 0, 16, 16, &[0, 0, 1, 0x65]));
        let frames = &shared.lock().unwrap().queue.videos();
        assert_eq!(frames.len(), 2);
        assert_eq!(frames[0].codec, RDP_VIDEO_CODEC_H265);
        assert_eq!(frames[1].codec, RDP_VIDEO_CODEC_H264);
    }

    #[test]
    fn hevc_requires_both_local_support_and_server_confirmation() {
        for enabled in [false, true] {
            let shared = Arc::new(Mutex::new(NativeGfxState::default()));
            let mut handler = NativeGfxHandler::new(Arc::clone(&shared), enabled);
            if !enabled {
                handler.on_capabilities_confirmed(&CapabilitySet::Frdp1 {
                    flags: CapabilitiesFrdp1Flags::HEVC_SUPPORTED,
                });
            }
            handler.on_hevc_frame(1, 0, 0, 16, 16, &[0, 0, 1, 38, 1]);
            let state = shared.lock().unwrap();
            assert!(state.queue.videos().is_empty());
            assert!(state.unsupported_graphics.is_some());
        }
    }
}
