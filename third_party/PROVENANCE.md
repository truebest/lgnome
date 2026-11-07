# Third-party dependency provenance

This file records native webOS dependencies that are pinned for the native-only
`lgnome` target. The historical browser app, JavaScript service, browser harnesses,
and browser WASM bundle were removed and are not runtime dependencies of the native package.

## backend_ndl (first-party submodule)

- Path: `third_party/backend_ndl` (git submodule)
- Development remote: `git@bitbucket.org:kodavr/backend_ndl.git` (private)
- Public mirror: https://github.com/truebest/backend_ndl — an append-only chain of release
  snapshots. Released `.gitmodules` points here, so a published tag resolves without
  access to the development remote; the two carry the same tree at every release.
- Version: 0.6.0
- Pinned commit: `35a9655a5ddc32061124866be1c2fbe57f4f5414`
- License: MIT; see `third_party/backend_ndl/LICENSE`
- Native usage: runtime-loaded NDL DirectMedia facade wrapper for hardware video and mixed
  PCM audio. Firmware NDL binaries and SDK headers are not redistributed.

## IronRDP

- Path: `third_party/IronRDP` (git submodule — lgnome fork)
- Fork: https://github.com/truebest/IronRDP.
- Branch: `lgnome/upstream-reset`.
- Pinned commit: `1d083b83e2912b759f3a26b671754b9d58017930`
- Upstream: https://github.com/Devolutions/IronRDP
- Upstream base: `e4a0d17b8e5b1b81758202dc131d8fa2aed58a23` (2026-10-23), including
  merged client graphics protocol reset support (#2207).
- Base commit and fork delta provenance: see `third_party/IronRDP/PROVENANCE.md`.
- License: MIT OR Apache-2.0; see `third_party/IronRDP/LICENSE-MIT` and
  `third_party/IronRDP/LICENSE-APACHE`
- Native usage: RDP connector/session/EGFX protocol, AVC420/HEVC passthrough hooks, and
  RemoteFX Progressive decode to RGBA bitmap updates for the native renderer.

## LVGL

- Source cache: `build/deps/lvgl-185ea1fc61dd01fac61867d2d6b56892e80c6058`
  (downloaded by `native/cmake/Lvgl.cmake`)
- Upstream: https://github.com/mariotaku/lvgl.git
- Pinned commit: `185ea1fc61dd01fac61867d2d6b56892e80c6058`
- Archive: https://github.com/mariotaku/lvgl/archive/185ea1fc61dd01fac61867d2d6b56892e80c6058.zip
- Archive SHA-256: `e3c54648b975212d423802c105d654db374d3f09b5938f996cdb4299ca77243b`
- License: MIT; see `build/deps/lvgl-185ea1fc61dd01fac61867d2d6b56892e80c6058/LICENCE.txt`
- Native usage: SDL-rendered pre-connect GUI for host/port entry before native RDP startup.
- Build compatibility fix: `native/cmake/LvglSdlLine.cmake` produces a checked copy of
  the SDL line renderer with the four-argument draw callback ABI, handles texture
  allocation failure, and frees textures rejected by the cache. The cached sources
  remain unchanged; host tests exercise cache miss/hit/failure through the real callback.

## libevdev

- Source: https://www.freedesktop.org/software/libevdev/libevdev-1.13.6.tar.xz
- Version: 1.13.6
- SHA-256: `73f215eccbd8233f414737ac06bca2687e67c44b97d2d7576091aa9718551110`
- License: MIT, with the bundled Linux input header GPL-2.0 notice recorded in
  `third_party/libevdev-COPYING`
- Native usage: statically linked into webOS builds for grabbed USB mouse/keyboard evdev input.

## miniaudio

- Source cache: `build/deps/miniaudio-0.11.25` (downloaded by `native/cmake/Miniaudio.cmake`)
- Upstream: https://github.com/mackron/miniaudio
- Version/tag: `0.11.25`
- Archive: https://github.com/mackron/miniaudio/archive/refs/tags/0.11.25.zip
- Archive SHA-256: `580b1635c99621971c8053cd487fbc905d1c24d4395bb09214ff77026e5608e1`
- Pinned commit: `9634bedb5b5a2ca38c1ee7108a9358a4e233f14d`
- License choice: MIT No Attribution (upstream Alternative 2); see
  `build/deps/miniaudio-0.11.25/LICENSE`
- Native usage: headless 48 kHz float engine with one `ma_sound` voice per RDP session,
  per-session S16 conversion and dynamic resampling. Device I/O, file decoding/encoding,
  the resource manager, generators, runtime backend loading, and miniaudio threading
  primitives are disabled at compile time; the engine is wired once before rendering
  and NDL is the selected production audio sink.

## Certificate roots

- Rust crate: `webpki-roots` 1.0.10, pinned by `webrdp-min/Cargo.lock`.
- Upstream: https://github.com/rustls/webpki-roots
- Data: Mozilla's trusted TLS root certificates, bundled for native RDP server
  verification. Root updates ship with application updates.
- License: CDLA-Permissive-2.0; `third_party/webpki-roots-LICENSE` is included in
  the webOS package's `licenses/` directory.

## Fonts

- IBM Plex Sans: Regular 400 and SemiBold 600.
- IBM Plex Mono: Regular 400 and SemiBold 600.
- JetBrains Mono: SemiBold 600 for the `lgnome_` wordmark.

The compact diagnostics fonts use `@ibm/plex-mono` 2.5.0 (SemiBold, 12 px) and
`@ibm/plex-sans` 1.1.0 (SemiBold, 16 px, including profile-name Latin/Cyrillic).
They are generated from the packages' complete WOFF files with `lv_font_conv` 1.5.3,
4 bpp without compression; exact conversion options are in each generated C file.

License: SIL Open Font License 1.1 — [IBM Plex license](https://github.com/IBM/plex/blob/master/LICENSE.txt),
[JetBrains Mono license](https://github.com/JetBrains/JetBrainsMono/blob/master/OFL.txt).
Packaged builds include the corresponding copyright notices and complete OFL text as
`IBMPlex-OFL-1.1.txt` and `JetBrainsMono-OFL-1.1.txt`.

## Moonlight reference boundary

[Moonlight TV](https://github.com/mariotaku/moonlight-tv) (GPL-3.0) was used as a
read-only reference for webOS toolchain conventions, dependency revisions, and general
GUI behavior on TV. No Moonlight TV
application code has been copied or adapted into this repository, and none may be:
lgnome's own code is MIT-licensed, which is incompatible with incorporating GPL
application code. Only the dependency pins above (which carry their own licenses) are
shared with it.
