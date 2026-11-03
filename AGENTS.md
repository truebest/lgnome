# AGENTS.md

Guidance for AI agents working in this repo. Keep changes minimal, tested, and in the style
of the surrounding code; do not over-engineer.

## Project

`lgnome` is a native webOS RDP client for LG TVs that casts up to four simultaneous
GNOME desktops (gnome-remote-desktop) to the TV with hardware-decoded video and native
mixed audio. It is a C11/CMake shell (`native/`) around a Rust RDP core (`webrdp-min/`),
linked through a C ABI (`native/include/rdp_ffi.h`).

- One session owns video and input; background sessions remain connected with graphics
  suppressed and audio mixed.
- Video uses the NDL hardware backend, with native RemoteFX/bitmap decoding through SDL
  as the fallback. If neither path works, report a terminal native error.
- Audio is best-effort: failures may silence audio, never drop the RDP session.
- USB input uses evdev; SDL handles the compositor pointer. Input grabs follow window focus.

## Key paths

- `native/src/main.c` — bootstrap; runtime and session state live in `native_*` modules
  and `native_app.h`.
- `webrdp-min/src/native.rs` and `webrdp-min/src/native/` — Rust session worker, RDP
  protocols and FFI (`ffi.rs`, `abi.rs`, `sink.rs`).
- `native/include/rdp_ffi.h` — C/Rust ABI contract.
- `native/src/ndl_adapter/` — application adapters for `third_party/backend_ndl/`.
- `native/src/audio_pipeline*.c` — audio mixing and per-source buffering.
- `native/src/ui_preconnect*.c`, `ui_mixer.c` — LVGL UI and mixer.
- `native/src/input_evdev.c`, `input_sdl.c`, `cursor_sdl.c` — input and cursor handling.
- `native/src/capture_*.c`, `camera_*.c` — camera and microphone redirection.
- `native/CMakeLists.txt` — native build, tests and packaging.
- `docs/native-runbook.md` — runtime behavior, deployment and troubleshooting.
- `docs/build-environment.md` — toolchain setup; `third_party/PROVENANCE.md` and
  `third_party/IronRDP/PROVENANCE.md` — dependencies and fork changes.

## Commands

Local checks before committing native changes (these mirror CI):

```sh
./tools/syntax-check-native.sh
cargo test --manifest-path webrdp-min/Cargo.toml --features native --locked
cmake -S native -B /tmp/lgnome-native-build && cmake --build /tmp/lgnome-native-build
ctest --test-dir /tmp/lgnome-native-build --output-on-failure
```

Add `-DLGNOME_WITH_OPUS=ON` to the cmake configure to also run the `audio-opus` ctest;
it is off by default on host builds because ExternalOPUS downloads the libopus tarball
(needs network), and defaults to ON only for webOS cross-builds.

The host CMake/ctest build above is SDL-off: `main.c` and the split `native_*` modules compile
with their SDL bands empty, while `ui_preconnect*.c` and the evdev readers do not compile at
all — only the full webOS cross-build exercises those paths. Cross-build,
package, install, and launch for the TV:

```sh
./tools/build-native-webos.sh
ARES_DEVICE=<tv-device> ./tools/deploy-native-webos.sh --ipk /path/to/verified.ipk
```

Use the `armv7-unknown-linux-gnueabi` Rust target, NOT `arm-unknown-linux-gnueabi`: the generic
ARM target can emit CP15 barrier instructions that crash as `Illegal instruction` on ARMv8
webOS. Toolchain default
`/opt/arm-webos-linux-gnueabi_sdk-buildroot/share/buildroot/toolchainfile.cmake`; override with
`WEBOS_TOOLCHAIN_FILE=...`.

## Conventions

- Treat `native/include/rdp_ffi.h` as the ABI contract: a change there must update C, Rust,
  tests, and docs together.
- Callback byte lifetimes are synchronous — copy `on_video_au` / bitmap bytes before returning
  if you retain them.
- RDPEGFX AVC data arrives in BOTH framings: nominally length-prefixed H.264, but
  gnome-remote-desktop delivers Annex-B directly. Feed the NDL backend Annex-B (convert or
  pass through); classify AUs only with the `h264_annexb.h` scanners, which handle both.
- RDPECAM accepts only non-emulated single-plane `V4L2_PIX_FMT_H264` with Annex-B start
  codes, so the camera hardware has to encode: do not add YUYV, MJPEG, or
  `V4L2_PIX_FMT_H264_NO_SC` fallbacks or any transcode, and treat the jail's
  `/dev/video0`/`/dev/video1` window as fixed. The YUYV camera preview is diagnostic only.
  Keep camera AUs within the 4 MiB C/Rust boundary. A V4L2 sequence jump means dropped
  buffers, not a broken node: re-gate the decoder seed instead of reopening capture.
- Product webOS builds set `LGNOME_WITH_NDL=ON` and `LGNOME_LINK_RDP_FFI=ON`; SDL/LVGL/evdev
  are selected automatically by the webOS toolchain. The binary dlopens `libNDL_directmedia.so.1` at runtime — it
  must never carry a DT_NEEDED on it (verify_package_root enforces this).
- C: C11, 4-space indent, small file-local helpers, explicit error returns, ASCII. Native
  symbols use `native_*` / subsystem prefixes; exported ABI symbols use `rdp_*`. Rust: `cargo
  fmt`. Shell: `#!/usr/bin/env bash` + `set -euo pipefail`.
- Production C files that log declare exactly one file-scoped `clog_define(...)`.
  Pure helpers need no category or artificial log call. The standalone `backend_ndl`
  library keeps its callback logger; the native adapter forwards it as `media.ndl`.
- Do not bypass `clog` with direct stderr writes, `perror`, `SDL_Log`, Rust `eprintln!`, or a
  stderr tracing subscriber. Rust logging crosses the synchronous RDP callback bridge as
  `rdp.rust`. `LGNOME_LOG` is the only runtime level control; do not restore
  `WEBRDP_LOG`, `GNOMECAST_LOG`, `GNOMECAST_NDL_LOG`, or `/tmp/gnomecast-ndl-debug`.
- Do not log per-frame, per-pointer-motion, or per-audio-block success. Put noisy diagnostics
  below `info` and rate-limit repeated hot-path failures with `clog_limited`.
- Keep generated artifacts out of the repo (`native/config.local.json`, build dirs, logs,
  `*.ipk`).

## Git / release workflow

- Dev history lives in Bitbucket (`origin`, `kodavr/gnomecast`); pushes auto-trigger CI on
  self-hosted runners. `main` is protected — land changes via `bkt pr` (create → `bkt pr checks
  --wait` → `bkt pr merge`), not direct pushes.
- GitHub (`truebest/lgnome`) is an append-only, releases-only mirror: one snapshot commit +
  `vX.Y.Z` tag per version. Prepare with `tools/release-github.sh <version> /path/to/verified.ipk`,
  merge the candidate through a reviewed PR, then tag the merged commit. Never
  force-push or move tags there.
- Bump `native/deploy/webos/appinfo.json` `version` for a release. Never add Claude/AI
  attribution trailers to commit messages.

## Bitbucket review follow-up

- Read `bkt pr comments <pr> --json`, verify findings against the code, and test fixes
  appropriately. Documentation-only changes do not need a full test run.
- Fetch before pushing: preserve fixes already committed by bots or contributors.
  Check that the PR is still open before reusing its branch.
- After pushing a fix, resolve only its addressed comments and verify the API response:

  ```sh
  bkt api /repositories/<workspace>/<repo>/pullrequests/<pr>/comments/<comment-id>/resolve --method POST
  ```

- Review and CI are separate: before merging, check new findings on the current head
  and run `bkt pr checks <pr> --wait`.

## Do not

- Do not reintroduce Web/MSE/WebCodecs/RDCleanPath or the deleted browser/Luna trees, and do
  not package `app/` or `service/` into the native target.
- Do not substitute a software H.264 decoder for acceptance; the NDL hardware path is
  required on the TV.
- Do not log passwords or local config contents.
