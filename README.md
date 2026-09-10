# lgnome

`lgnome` is a native webOS RDP client for LG TVs, bringing **Windows** and
**Linux desktops (Ubuntu and Fedora with GNOME)** to the TV with hardware-decoded
video and native mixed audio. Hardware-accelerated H.264 streaming has been verified
on remote hosts with **NVIDIA and Intel GPUs**.

Per-version release notes and the packaged `.ipk` live on the
[Releases page](https://github.com/truebest/lgnome/releases). This file describes what
the client does now, not what changed in any one version.

> **For work use:** block the smart TV or monitor's internet access at your router,
> while allowing the local connections needed for RDP, to limit telemetry and the
> risk of third-party apps relaying traffic through your home IP. Allow internet
> access temporarily when needed for updates or Developer Mode renewal.

## Features

- **Video**: AVC420/H.264 over RDPEGFX, decoded by the TV hardware video plane through
  the in-house NDL DirectMedia backend (`backend_ndl`, dlopen of
  `libNDL_directmedia.so.1`). Servers that cannot provide H.264 fall back to native
  RemoteFX Progressive/bitmap decoding in Rust, presented as RGBA through SDL.
- **Multi-session switching**: four RDP sessions map to the remote's red, green, yellow,
  and blue buttons. One session owns the video and KVM-style input at a time while the
  other connected sessions remain backgrounded and keep feeding the audio mix.
- **Camera and microphone redirection**: per-profile RDPECAM/RDPEAI opt-ins use one
  shared USB V4L2 camera and ALSA microphone. The HUB selects stable device IDs,
  camera mode, and microphone gain; camera video is passed through as native Annex-B
  H.264 and capture failures remain best-effort. The HUB lists the sizes and frame rates
  the selected camera itself reports for non-emulated, single-plane `V4L2_PIX_FMT_H264`,
  up to 1920x1080 at 30 fps; sub-VGA modes are not offered. YUYV, MJPEG, and
  `V4L2_PIX_FMT_H264_NO_SC` are not fallback formats. Retained custom settings remain schema-compatible, but capture still requires
  an exact native-H.264 mode.
  In a multi-session setup, media payload is sent only to the on-screen profile; a
  background profile's virtual camera is removed until that profile returns to the screen,
  while its microphone channel remains negotiated but silent. Native-H.264 capture is
  device-verified on the TV; foreground handoff between two simultaneously connected
  capture-enabled profiles is implemented and host-tested.
- **Audio**: MS-RDPEA over the `AUDIO_PLAYBACK_DVC` dynamic channel (the transport
  gnome-remote-desktop uses). Opus 48 kHz stereo is negotiated preferentially and
  decoded in-process through libopus; 16-bit PCM is the fallback. A headless miniaudio
  engine mixes `ma_sound` voices from all sessions at 48 kHz stereo and independently
  resamples 44.1/48 kHz sources. Its always-on adaptive jitter controller tracks
  burst/HOL delay and clock drift; there is no manual buffer setting. NDL remains the
  PCM sink. Audio is strictly best-effort: failures degrade to silent video, never
  to a dropped session.
- **Input**: the USB mouse and keyboard are read straight from the kernel evdev layer
  (`/dev/input`, `EVIOCGRAB`), below the webOS compositor, so physical input reaches the RDP
  server untouched by the TV's SDL/Wayland munging — no synthesized Back on right-click, no
  pointer recenter, no F-key/keypad/NumLock quirks, no IME double-input. Mouse motion, buttons
  and wheel and every key are forwarded as RDP fast-path input, keys as raw AT set-1 scancodes;
  NumLock is synced on connect so the numpad types digits. When no USB mouse is attached the
  compositor pointer (Magic Remote) still drives the session through an SDL fallback. The grab
  is global, so it follows window focus: a webOS overlay (the TV menu) that steals focus
  releases the mouse and keyboard so they drive the overlay, and re-grabs — restoring the
  cursor — on return; held buttons/keys are released to the server first so a mid-press focus
  change never leaves a stuck drag.
- **Server cursor**: the real pointer shapes (I-beam, resize arrows, ...) arrive as RDP
  pointer updates and are applied as the system color cursor, composited by the platform's
  cursor plane above the hardware video with zero extra presents. The grabbed mouse drives it
  by warping the OS pointer to the logical position; visibility follows the server's pointer
  state.
- **Pre-connect UI**: an on-TV LVGL settings screen (host, credentials, fps, audio codec)
  with persisted settings.
- **Network autodetection**: the client answers connect-time and continuous RTT
  measurements over the MCS message channel — gnome-remote-desktop refuses audio
  redirection without it.
- **Display Control (MS-RDPEDISP)**: right after connect the client requests the
  profile's saved desktop resolution (3840×2160 by default). Each profile can select
  its own size; a server mirroring a physical monitor can retain that monitor's size
  (see Known limitations for servers without this channel).
- **Auto-reconnect**: gnome-remote-desktop closes sessions with a provider-initiated
  disconnect as a normal part of daemon handoffs; the client reconnects automatically
  (up to 3 attempts), matching mstsc/FreeRDP behavior.

### Camera purchase recommendation

The camera must produce the H.264 stream itself. Nothing in this client transcodes:
gnome-remote-desktop accepts only `CAM_MEDIA_FORMAT_H264` through RDPECAM, and at
upstream `main` commit `00195e889ad9390d4fa462d291a70fa92fe64678` that is the sole
format accepted by [`has_supported_media_type()`][grd-camera-media-format]. YUYV and
MJPEG are not fallbacks, so a camera without hardware H.264 cannot be redirected.

Two properties have to hold, and marketing copy establishes neither:

- The Linux driver exposes `V4L2_PIX_FMT_H264` as a non-emulated, single-plane format
  at the resolution and frame rate you intend to use.
- That format is on the camera's **first** V4L2 capture node. The webOS app jail exposes
  exactly two camera nodes, `/dev/video0` and `/dev/video1`, as fixed entries, so a camera
  that puts H.264 on a later node — for example on a second VideoStreaming interface,
  behind the metadata node of the first — is unreachable from the TV even though it works
  on a desktop.

Check both on any Linux box before buying — the first node of that camera must list
`H264`:

```sh
v4l2-ctl -d /dev/video0 --list-formats-ext
```

The Adesso CyberTrack H5 is device-verified on the TV at 640x480 at 15 fps. The earlier
Logitech Brio 300 validation exercised the removed YUYV conversion path and says nothing
about this one.

## Remote host

Supported remote hosts:

- **Windows** with its built-in Remote Desktop (RDP) server. H.264 streaming works
  through RDPEGFX; see the AVC420/AVC444 requirements under Known limitations below.
- **Ubuntu and Fedora Linux** with GNOME and `gnome-remote-desktop` RDP enabled.

On Linux, the AVC420 hardware-video path has been verified end to end with both
**NVIDIA/NVENC** and **Intel GPU/VA-API** hosts (the tested Intel path uses the iHD
driver). Windows H.264 hardware encoding has also been verified with **Intel Quick
Sync**. The encoder paths do not use one universal wire framing: lgnome accepts both AVC length-prefixed and Annex-B H.264
access units, including the AUD-prefixed units observed from Intel VA-API, and feeds
Annex-B to the TV decoder. This records tested configurations rather than guaranteeing
every VA-API driver or gnome-remote-desktop version.

The Intel validation uncovered a separate server bug: affected builds still select
VA-API and stream AVC420 into the TV's hardware decoder, but can encode an invalid
duplicate H.264 `frame_num` at each counter wrap. Stricter hardware decoders turn that
broken reference sequence into accumulating picture corruption which never heals.
gnome-remote-desktop does not fall back to RemoteFX here: VA-API reports successful
encoding, so the session keeps sending the malformed AVC420 stream. The resulting
[upstream fix][grd-vaapi-frame-num] is merged into the current gnome-remote-desktop
`main` (`51.alpha`), where the Intel stream works correctly. Reliable Intel VA-API
streaming therefore requires upstream `main`, a backport, or the first released
gnome-remote-desktop version that contains MR !412.

Separately, when the server has no usable H.264 encoder at all it falls back to RemoteFX
Progressive, which lgnome renders in software at a noticeably higher cost on both
ends.

## Known limitations

- **Limit the frame rate on the server to what the TV can actually present.** A
  gnome-remote-desktop host can produce frames faster than the smart display's
  downstream path can scan them out. The decoder may continue accepting every access
  unit while buffering, replacement, or dropping happens in a lower firmware/display
  stage, resulting in uneven motion, growing latency, or uncomfortable interaction.
  The profile's `fps` value controls local loop/timestamp bookkeeping; it does not send
  a frame-rate command to gnome-remote-desktop. Until the host applies a real limit,
  set the GNOME session or virtual monitor to a frame rate the TV can sustain.

  Automatic regulation from the currently available client signals is not reliable.
  RDPEGFX acknowledges a frame at `EndFrame`, before NDL and before panel presentation.
  NDL's render-queue getter and the duration of synchronous `NDL_DirectVideoPlay` are
  also pre-display measurements; on the tested TV the reported queue normally remains
  at 0-1 even when a deeper stage may be the bottleneck. The public facade provides no
  trustworthy "frame reached scanout/panel" timestamp. A controller driven by ACK
  timing or that queue therefore observes fast ingestion rather than panel capacity
  and can raise the rate while the visible path is already overloaded. These metrics
  remain diagnostic only; rate limiting belongs on the server.

- **Some non-standard server resolutions do not play on the tested TV.** The hardware
  video pipeline was observed to stay black with a 2048×1152 virtual display: no errors,
  frames are fed, but playback never begins. Standard resolutions are confirmed good
  (1920×1080, 3840×2160). On servers that support MS-RDPEDISP the client fixes this
  automatically at connect; older gnome-remote-desktop versions never open that channel,
  so their (virtual) display should use a known-good standard resolution server-side —
  and note that a headless server's display can revert to its unusual default after a
  service restart.
- **The native video path presents AVC420 only.** The client requests AVC420 through
  EGFX V8.1 and also advertises V10 because the tested Windows host enables H.264 only
  for a V10 client. That host sends a self-contained 4:2:0 stream when its AVC444
  prioritization policy is disabled. A server that actually sends the AVC444 auxiliary
  view is not supported correctly (see "Chroma subsampling" below). Servers without a
  usable H.264 encoder fall back to software RemoteFX rendering (slower, and rendered on
  the ~1080p UI plane rather than the native-resolution video plane).
- **EGFX surface-composition operations are ignored** (SolidFill, SurfaceToSurface,
  CacheToSurface). Harmless with gnome-remote-desktop — it sends SurfaceToSurface during
  routine AVC420 sessions and the picture is complete since the video plane carries full
  frames — but a server that relied on them for the software RemoteFX path would show
  stale regions.

### Chroma subsampling: why AVC444 cannot use the native video path

The hardware plane presents H.264 4:2:0. V10 remains a Windows compatibility
fallback, but it does not make two-view AVC444 presentable; that is a platform
ceiling rather than a missing pixel conversion:

- RDP's AVC444 carries a luma view plus an auxiliary view with packed chroma.
  The views can share one H.264 reference-picture sequence, so discarding the
  auxiliary access units can also break prediction in later luma pictures. The
  NDL media pipeline renders every access unit it accepts and never exposes
  decoded frames to the application, so it can neither consume an auxiliary
  picture without displaying it nor recombine the two views in the pixel domain.
- On the validated webOS 23/24 targets, the available hardware profiles are H.264
  BP/MP/HP, HEVC Main/Main10, and AV1 Main — no Hi444PP, HEVC RExt, or AV1 High.
- Decoding the AVC444 stream pair in software would forfeit the hardware
  video plane and cannot sustain 4K on TV SoCs.

The same composition ceiling applies to any hardware-plane client that cannot access
decoded frames. The client warns and drops an unexpected auxiliary view; the result is
best-effort and may remain corrupted until a new independent decoder seed. Production
Windows profiles must therefore select a self-contained 4:2:0 stream.

## Layout

- `native/` — C11/CMake shell: webOS lifecycle, raw evdev mouse+keyboard reader
  (`input_evdev.c`, with an SDL pointer fallback), SDL presentation, lgnome-specific
  DirectMedia adapters under `native/src/ndl_adapter/`, RemoteFX RGBA presentation,
  pre-connect UI, and package targets.
- `third_party/backend_ndl/` — standalone MIT C11 DirectMedia git submodule with a public SDK-independent
  header, CMake package, runtime `dlopen`, callbacks, and host tests.
- `webrdp-min/` — Rust static library implementing the RDP client (direct TCP + TLS +
  CredSSP/NTLM, EGFX, rdpsnd-over-DVC), exposed through the C ABI in
  `native/include/rdp_ffi.h`.
- `third_party/` — pinned native dependencies (git submodules): our IronRDP fork
  ([truebest/IronRDP](https://github.com/truebest/IronRDP), branch `gnome-rdp-support`,
  fork delta recorded in `third_party/IronRDP/PROVENANCE.md`), LVGL, miniaudio.
- `Dockerfile` + `bitbucket-pipelines.yml` — containerized build environment and CI that
  produces the webOS `.ipk`. CI runs inside a prebuilt public image
  (`cubicattache/gnomecast-webos-build`); rebuild and push it with
  `tools/push-build-image.sh` after changing the Dockerfile.

The historical JavaScript/browser app, Luna JS service, browser harnesses, and generated
browser WASM bundle were removed. Do not reintroduce Web, MSE, WebCodecs, RDCleanPath, or
browser runtime fallback paths.

## Documentation

- [Documentation index](docs/README.md) — ownership and purpose of each document;
- [native webOS runbook](docs/native-runbook.md) — runtime controls, build, package,
  deploy, acceptance, logging, and triage;
- [build environment](docs/build-environment.md) — reproducible host/container setup;
- [third-party provenance](third_party/PROVENANCE.md) and
  [IronRDP fork provenance](third_party/IronRDP/PROVENANCE.md) — pinned dependencies and
  fork delta.

## License

lgnome's own code is released under the [MIT License](LICENSE).

Bundled dependencies under `third_party/` keep their own licenses: IronRDP
(MIT OR Apache-2.0), LVGL (MIT), miniaudio (MIT-0), and the IBM Plex and JetBrains Mono
fonts (SIL Open Font License 1.1). The `third_party/backend_ndl/`
subproject, including its host-test ABI double, is MIT (see
`third_party/backend_ndl/THIRD_PARTY.md`).
Packaged builds ship dependency provenance and applicable notices under `licenses/`
inside the `.ipk`.

[grd-vaapi-frame-num]: https://gitlab.gnome.org/GNOME/gnome-remote-desktop/-/merge_requests/412
[grd-camera-media-format]: https://gitlab.gnome.org/GNOME/gnome-remote-desktop/-/blob/00195e889ad9390d4fa462d291a70fa92fe64678/src/grd-rdp-dvc-camera-device.c#L1388
