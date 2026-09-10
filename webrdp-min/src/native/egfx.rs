//! EGFX graphics pipeline: AVC420 access units and RemoteFX/bitmap RGBA
//! updates, accumulated under a lock by IronRDP's handler callbacks and
//! drained by the worker between socket reads.

#![forbid(unsafe_code)]

use std::collections::HashMap;
use std::sync::{Arc, Mutex};

use super::LOG_TARGET_GRAPHICS;
use ironrdp_egfx::client::{BitmapUpdate, GraphicsPipelineHandler};
use ironrdp_egfx::pdu::{
    CacheToSurfacePdu, CapabilitiesV107Flags, CapabilitiesV81Flags, CapabilitiesV8Flags,
    CapabilitySet, Codec1Type, GfxPdu, SolidFillPdu, SurfaceToSurfacePdu,
};

#[derive(Default)]
pub(super) struct NativeGfxState {
    pub(super) pending_video: Vec<Vec<u8>>,
    pub(super) pending_bitmap: Vec<NativeBitmapUnit>,
    pub(super) unsupported_graphics: Option<String>,
    // ResetGraphics dimensions, independent of the initial MCS/GCC desktop size.
    pub(super) graphics_width: u32,
    pub(super) graphics_height: u32,
    pub(super) graphics_size_pending: bool,
    // Add the mapped output origin to surface-local bitmap rectangles.
    pub(super) surface_origins: HashMap<u16, (u32, u32)>,
    pub(super) avc444_aux_frames: u64,
}

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
    pub(super) fn new(shared: Arc<Mutex<NativeGfxState>>) -> Self {
        Self {
            shared,
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
        // AVC_THIN_CLIENT requests the YUV420 view; tested Windows hosts need V10 for H.264.
        let caps = vec![
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
        tracing::info!(target: LOG_TARGET_GRAPHICS, offered = ?caps,
            "EGFX offer: H.264 passthrough, native bitmap fallback, no full AVC444 presentation");
        caps
    }

    fn on_capabilities_confirmed(&mut self, caps: &CapabilitySet) {
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
            shared.pending_video.clear();
            shared.pending_bitmap.clear();
            // ResetGraphics implicitly destroys all surfaces, so any tracked mapping origin is
            // stale afterward.
            shared.surface_origins.clear();
            if width != shared.graphics_width || height != shared.graphics_height {
                shared.graphics_width = width;
                shared.graphics_height = height;
                shared.graphics_size_pending = true;
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
        if let Ok(mut shared) = self.shared.lock() {
            // `destination_rectangle` is surface-local; a surface mapped away from (0,0) via
            // RDPGFX_MAP_SURFACE_TO_OUTPUT_PDU needs that origin added before this lands on the
            // desktop-relative native RGBA canvas.
            let (origin_x, origin_y) = shared
                .surface_origins
                .get(&update.surface_id)
                .copied()
                .unwrap_or((0, 0));
            shared.pending_bitmap.push(NativeBitmapUnit {
                surface_id: update.surface_id,
                left: u32::from(update.destination_rectangle.left) + origin_x,
                top: u32::from(update.destination_rectangle.top) + origin_y,
                width: u32::from(update.width),
                height: u32::from(update.height),
                stride,
                data: update.data.clone(),
            });
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
            if let Ok(mut shared) = self.shared.lock() {
                shared.pending_video.push(nal.to_vec());
            }
            self.note_rendered_update();
        }
        true
    }

    // NDL presents every decoded AU; submitting the auxiliary view would display chroma as luma.
    fn on_avc444_aux_frame(&mut self, _nal: &[u8]) -> bool {
        if let Ok(mut shared) = self.shared.lock() {
            shared.avc444_aux_frames += 1;
            let dropped_aux = shared.avc444_aux_frames;
            if dropped_aux == 1 || dropped_aux % 100 == 0 {
                tracing::warn!(
                    target: LOG_TARGET_GRAPHICS,
                    dropped_aux,
                    "server sent AVC444; rendering its YUV420 view only, chroma refinement dropped"
                );
            }
        }
        true
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
                NativeGfxHandler::new(Arc::new(Mutex::new(NativeGfxState::default())));
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

    #[test]
    fn reset_graphics_marks_size_pending_once_per_distinct_value() {
        let mut handler = NativeGfxHandler::new(Arc::new(Mutex::new(NativeGfxState::default())));

        // ResetGraphics dimensions may change independently of the initial handshake.
        handler.on_reset_graphics(3840, 2160);
        {
            let shared = handler.shared.lock().unwrap();
            assert!(shared.graphics_size_pending);
            assert_eq!(shared.graphics_width, 3840);
            assert_eq!(shared.graphics_height, 2160);
        }

        // Repeating the same size should not re-flag it as pending (drain_gfx already
        // cleared the flag after dispatching it once).
        {
            let mut shared = handler.shared.lock().unwrap();
            shared.graphics_size_pending = false;
        }
        handler.on_reset_graphics(3840, 2160);
        assert!(!handler.shared.lock().unwrap().graphics_size_pending);

        // A genuinely new size must be re-flagged.
        handler.on_reset_graphics(1920, 1080);
        let shared = handler.shared.lock().unwrap();
        assert!(shared.graphics_size_pending);
        assert_eq!(shared.graphics_width, 1920);
        assert_eq!(shared.graphics_height, 1080);
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
    fn a_stream_that_only_ever_drops_updates_fails_the_session_over() {
        let mut handler = NativeGfxHandler::new(Arc::new(Mutex::new(NativeGfxState::default())));
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
        let mut handler = NativeGfxHandler::new(Arc::new(Mutex::new(NativeGfxState::default())));
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
        let mut handler = NativeGfxHandler::new(Arc::new(Mutex::new(NativeGfxState::default())));

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
        assert_eq!(shared.pending_bitmap.len(), 1);
        assert_eq!(shared.pending_bitmap[0].left, 1920 + 10);
        assert_eq!(shared.pending_bitmap[0].top, 100 + 20);
        drop(shared);

        // A ResetGraphics implicitly destroys all surfaces; a stale origin must not leak into
        // a later bitmap update on a reused surface id that hasn't been re-mapped.
        handler.on_reset_graphics(3840, 2160);
        {
            let mut shared = handler.shared.lock().unwrap();
            shared.pending_bitmap.clear();
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
            assert_eq!(shared.pending_bitmap[0].left, 10);
            assert_eq!(shared.pending_bitmap[0].top, 20);
        }

        // DeleteSurface destroys a single surface the same way; a reused id must start
        // unmapped rather than inherit the deleted surface's origin.
        handler.on_surface_mapped(7, 500, 600);
        handler.on_surface_deleted(7);
        {
            let mut shared = handler.shared.lock().unwrap();
            shared.pending_bitmap.clear();
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
            shared.pending_bitmap[0].left, 10,
            "deleted surface's origin must not leak"
        );
        assert_eq!(shared.pending_bitmap[0].top, 20);
    }

    #[test]
    fn gfx_capabilities_offer_avc420_and_windows_v10_fallback() {
        let handler = NativeGfxHandler::new(Arc::new(Mutex::new(NativeGfxState::default())));

        let capabilities = handler.capabilities();
        assert_eq!(
            capabilities[1],
            CapabilitySet::V8_1 {
                flags: CapabilitiesV81Flags::AVC420_ENABLED | CapabilitiesV81Flags::SMALL_CACHE,
            }
        );
        assert_eq!(capabilities.len(), 3);
        assert!(handler.wants_avc420_passthrough());
    }
}
