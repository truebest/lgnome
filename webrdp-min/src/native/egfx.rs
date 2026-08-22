//! EGFX graphics pipeline: AVC420 access units and RemoteFX/bitmap RGBA
//! updates, accumulated under a lock by IronRDP's handler callbacks and
//! drained by the worker between socket reads.

#![forbid(unsafe_code)]

use std::collections::HashMap;
use std::sync::{Arc, Mutex};

use super::LOG_TARGET_GRAPHICS;
use ironrdp_egfx::client::{BitmapUpdate, GraphicsPipelineHandler};
use ironrdp_egfx::pdu::{
    CacheToSurfacePdu, CapabilitiesV81Flags, CapabilitiesV8Flags, CapabilitySet, Codec1Type,
    GfxPdu, SolidFillPdu, SurfaceToSurfacePdu, WireToSurface2Pdu,
};

#[derive(Default)]
pub(super) struct NativeGfxState {
    pub(super) pending_video: Vec<NativeVideoUnit>,
    pub(super) pending_bitmap: Vec<NativeBitmapUnit>,
    pub(super) unsupported_graphics: Option<String>,
    // Real graphics output size from the server's RDPGFX_RESET_GRAPHICS_PDU, which can
    // differ from the negotiated MCS/GCC desktop size (e.g. a TV whose hardware decoder
    // always runs at the panel's native resolution regardless of the requested session size).
    // Applies uniformly to both the AVC420/H.264 and RemoteFX bitmap paths, so it is
    // dispatched through the same on_desktop_size callback both codecs already rely on
    // instead of being attached only to AVC420 access units.
    pub(super) graphics_width: u32,
    pub(super) graphics_height: u32,
    pub(super) graphics_size_pending: bool,
    // Per-surface output origin from RDPGFX_MAP_SURFACE_TO_OUTPUT_PDU. BitmapUpdate rectangles
    // are surface-local, so this must be added before a bitmap update is queued, or updates on
    // a surface mapped away from (0,0) land at the wrong desktop position.
    pub(super) surface_origins: HashMap<u16, (u32, u32)>,
}

pub(super) struct NativeVideoUnit {
    pub(super) data: Vec<u8>,
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
}

impl NativeGfxHandler {
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
        vec![
            CapabilitySet::V8_1 {
                flags: CapabilitiesV81Flags::AVC420_ENABLED | CapabilitiesV81Flags::SMALL_CACHE,
            },
            CapabilitySet::V8 {
                flags: CapabilitiesV8Flags::SMALL_CACHE,
            },
        ]
    }

    fn on_capabilities_confirmed(&mut self, _caps: &CapabilitySet) {}

    fn on_reset_graphics(&mut self, width: u32, height: u32) {
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
    }

    fn on_wire_to_surface2(&mut self, _pdu: &WireToSurface2Pdu) {
        // RemoteFX Progressive is decoded by IronRDP into `on_bitmap_updated` RGBA tiles.
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
        // `_width`/`_height` above are the frame's destination rectangle on the surface,
        // not the surface's full resolution; the real graphics output size is dispatched
        // separately via on_reset_graphics/on_desktop_size (see drain_gfx).
        if !nal.is_empty() {
            if let Ok(mut shared) = self.shared.lock() {
                shared
                    .pending_video
                    .push(NativeVideoUnit { data: nal.to_vec() });
            }
        }
        true
    }

    fn wants_avc420_passthrough(&self) -> bool {
        true
    }

    // Surface-mutating EGFX operations the client deliberately IGNORES — do not turn these
    // into mark_unsupported_graphics (a review once did; every live session then died with
    // "server used unsupported EGFX SurfaceToSurface" right after Active):
    //
    // - gnome-remote-desktop sends SurfaceToSurface as part of ROUTINE AVC420 sessions.
    // - On the hardware path the TV's video plane shows the complete decoded H.264 stream;
    //   server-side surface composition never reaches the screen, so ignoring these ops is
    //   visually correct there — confirmed by every live session to date.
    // - They matter only for the RemoteFX/RGBA software fallback, where ignoring them CAN
    //   leave stale pixels on servers that use them for that path (grd does not today). If
    //   such a server appears, implement the ops on the RGBA canvas instead of failing.
    //
    // The dispatcher matches these PDUs explicitly, so they never reach on_unhandled_pdu;
    // the trait defaults are silent no-ops. Trace them through the C logging bridge to stay
    // observable when the corresponding target is enabled.
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

    fn on_unhandled_pdu(&mut self, pdu: &GfxPdu) {
        let codec = match pdu {
            GfxPdu::WireToSurface1(pdu) => match pdu.codec_id {
                Codec1Type::Avc444 | Codec1Type::Avc444v2 => "AVC444",
                _ => "unsupported",
            },
            _ => "unsupported",
        };
        self.mark_unsupported_graphics(format!("server produced unsupported {codec} EGFX PDU"));
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
            let mut handler = NativeGfxHandler {
                shared: Arc::new(Mutex::new(NativeGfxState::default())),
            };
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
        let mut handler = NativeGfxHandler {
            shared: Arc::new(Mutex::new(NativeGfxState::default())),
        };

        // The server's real graphics output can be larger than the negotiated MCS/GCC
        // desktop size (e.g. a TV whose hardware decoder always runs at panel resolution).
        // This must be dispatched to both the ss4s/H.264 and RemoteFX paths uniformly, so
        // it goes through on_desktop_size (see drain_gfx) rather than being attached only
        // to AVC420 access units.
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

    #[test]
    fn bitmap_update_offsets_by_mapped_surface_origin() {
        let mut handler = NativeGfxHandler {
            shared: Arc::new(Mutex::new(NativeGfxState::default())),
        };

        // Server maps surface 7 at a non-zero desktop position (e.g. a second monitor).
        handler.on_surface_mapped(7, 1920, 100);

        handler.on_bitmap_updated(&BitmapUpdate {
            surface_id: 7,
            destination_rectangle: ExclusiveRectangle {
                left: 10,
                top: 20,
                right: 12,
                bottom: 22,
            },
            codec_id: Codec1Type::Uncompressed,
            data: vec![0u8; 2 * 2 * 4],
            width: 2,
            height: 2,
        });

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
        handler.on_bitmap_updated(&BitmapUpdate {
            surface_id: 7,
            destination_rectangle: ExclusiveRectangle {
                left: 10,
                top: 20,
                right: 12,
                bottom: 22,
            },
            codec_id: Codec1Type::Uncompressed,
            data: vec![0u8; 2 * 2 * 4],
            width: 2,
            height: 2,
        });
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
        handler.on_bitmap_updated(&BitmapUpdate {
            surface_id: 7,
            destination_rectangle: ExclusiveRectangle {
                left: 10,
                top: 20,
                right: 12,
                bottom: 22,
            },
            codec_id: Codec1Type::Uncompressed,
            data: vec![0u8; 2 * 2 * 4],
            width: 2,
            height: 2,
        });
        let shared = handler.shared.lock().unwrap();
        assert_eq!(
            shared.pending_bitmap[0].left, 10,
            "deleted surface's origin must not leak"
        );
        assert_eq!(shared.pending_bitmap[0].top, 20);
    }

    #[test]
    fn gfx_capabilities_do_not_advertise_avc444() {
        let handler = NativeGfxHandler {
            shared: Arc::new(Mutex::new(NativeGfxState::default())),
        };

        let capabilities = handler.capabilities();
        assert_eq!(capabilities.len(), 2);
        assert_eq!(
            capabilities[0],
            CapabilitySet::V8_1 {
                flags: CapabilitiesV81Flags::AVC420_ENABLED | CapabilitiesV81Flags::SMALL_CACHE,
            }
        );
        assert_eq!(
            capabilities[1],
            CapabilitySet::V8 {
                flags: CapabilitiesV8Flags::SMALL_CACHE,
            }
        );
        assert!(!capabilities
            .iter()
            .any(|capability| matches!(capability, CapabilitySet::V10_7 { .. })));
        assert!(handler.wants_avc420_passthrough());
    }
}
