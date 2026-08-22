# gnomecast

`gnomecast` is a native webOS RDP client for LG TVs, built to put GNOME desktops
(`gnome-remote-desktop`) on the TV with hardware-decoded video and native mixed audio.

## 0.5.3 highlights

Compared with 0.5.2, this release adds and hardens the following user-visible paths:

- **USB camera and microphone redirection**: profiles can opt into RDPECAM and
  RDPEAI independently. A TV-connected V4L2 camera supplies native Annex-B H.264
  directly; ALSA microphone input is gain-adjusted, resampled, and packetized for
  gnome-remote-desktop. Device, native-H.264 mode, and microphone-gain selection live
  in the HUB. Camera consumers can close and reopen the redirected device without
  permanently losing it from PipeWire or reconnecting RDP: RDPECAM stop is idempotent
  and completes accepted sample requests before acknowledging the stop. Corrupt or
  malformed buffers, queue overflow, handoff, and submission failure discard the camera
  reference chain and reopen capture until a self-contained SPS+PPS+IDR decoder seed is
  available. Dropped buffers are not treated that way: a jump in the V4L2 sequence
  counter re-gates the decoder seed but keeps the device, the queued access units, and
  the cached parameter sets, because reopening would additionally restart mid-GOP. A
  bounded wait answers an accepted sample credit with `SampleError` without reopening
  capture, so a camera that starts inside a long GOP, or loses buffers mid-stream, can
  continue to its next decoder seed.
- **Capture privacy across sessions**: outgoing camera and microphone payload is sent
  only to the session currently on screen. A switch closes and drains the old
  session's payload gate before publishing the new owner and purges queued microphone
  data. The old session's camera is removed from RDPECAM/PipeWire while it is in the
  background, then advertised again with a fresh device channel when it returns.
- **Broader H.264 host compatibility**: the hardware-video path is now verified with
  both NVIDIA/NVENC and Intel GPU/VA-API gnome-remote-desktop hosts. Decoder restart,
  session handoff, and snapshot recovery use one parser for both AVC length-prefixed
  and Annex-B access units, including the AUD-first framing from the tested Intel
  encoder. Stable Intel streaming also needs the VA-API `frame_num` wrap fix from
  [gnome-remote-desktop MR !412][grd-vaapi-frame-num], which was reported from this
  testing. Current upstream `main` includes the fix and works normally with this
  hardware-decode path; so will the first release containing MR !412.
- **NDL sink diagnostics**: with `GNOMECAST_LOG='media.ndl=debug'`, every 300
  accepted video access units produce the NDL render-buffer value plus synchronous
  `NDL_DirectVideoPlay` timing and errors. These are intentionally diagnostic rather
  than automatic pacing inputs; see the display-rate limitation below.
- **Interaction and compatibility fixes**: extra mouse buttons open the HUB and
  mixer, Display Control channel recreation is more robust, capture-device fallback
  is improved, and the native/Rust lifecycle has received further concurrency
  hardening.

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
- **Display Control (MS-RDPEDISP)**: right after connect the client tells the server to
  switch its (virtual) monitor to the client's standard target resolution (currently
  3840×2160), so headless hosts with unusual display defaults (e.g. 2048×1152) stream
  at a resolution the TV pipeline can decode (see Known limitations for servers without
  this channel).
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

The server side is gnome-remote-desktop with RDP enabled. The AVC420 hardware-video
path has been verified end to end with both **NVIDIA/NVENC** and **Intel GPU/VA-API**
hosts (the tested Intel path uses the iHD driver). The encoder paths do not use one
universal wire framing: gnomecast accepts both AVC length-prefixed and Annex-B H.264
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
Progressive, which gnomecast renders in software at a noticeably higher cost on both
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
- **AVC444 is not negotiated** — the client advertises AVC420 (4:2:0) only (see
  "Chroma subsampling" below). Servers without a usable H.264 encoder fall back to
  software RemoteFX rendering (slower, and rendered on the ~1080p UI plane rather than
  the native-resolution video plane).
- **EGFX surface-composition operations are ignored** (SolidFill, SurfaceToSurface,
  CacheToSurface). Harmless with gnome-remote-desktop — it sends SurfaceToSurface during
  routine AVC420 sessions and the picture is complete since the video plane carries full
  frames — but a server that relied on them for the software RemoteFX path would show
  stale regions.

### Chroma subsampling: why there is no AVC444 mode

The client negotiates AVC420 (H.264 4:2:0) only. This is a platform ceiling,
not a missing feature:

- RDP's AVC444 is not a single 4:4:4 stream. Per MS-RDPEGFX it is *two*
  AVC420 bitstreams (a luma frame plus an auxiliary frame carrying the packed
  chroma samples) that the client must decode independently and recombine in
  the pixel domain. The NDL media pipeline feeds the elementary
  stream straight to the hardware video plane and never exposes decoded frames
  to the application, so the recombination step has nowhere to run.
- On the validated webOS 23/24 targets, the available hardware profiles are H.264
  BP/MP/HP, HEVC Main/Main10, and AV1 Main — no Hi444PP, HEVC RExt, or AV1 High.
- Decoding the AVC444 stream pair in software would forfeit the hardware
  video plane and cannot sustain 4K on TV SoCs.

The same composition ceiling applies to any hardware-plane client that cannot access
decoded frames.

## Layout

- `native/` — C11/CMake shell: webOS lifecycle, raw evdev mouse+keyboard reader
  (`input_evdev.c`, with an SDL pointer fallback), SDL presentation, gnomecast-specific
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

gnomecast's own code is released under the [MIT License](LICENSE).

Bundled dependencies under `third_party/` keep their own licenses: IronRDP
(MIT OR Apache-2.0), LVGL (MIT), miniaudio (MIT-0), and the IBM Plex and JetBrains Mono
fonts (SIL Open Font License 1.1). The `third_party/backend_ndl/`
subproject, including its host-test ABI double, is MIT (see
`third_party/backend_ndl/THIRD_PARTY.md`).
Packaged builds ship dependency provenance and applicable notices under `licenses/`
inside the `.ipk`.

[grd-vaapi-frame-num]: https://gitlab.gnome.org/GNOME/gnome-remote-desktop/-/merge_requests/412
[grd-camera-media-format]: https://gitlab.gnome.org/GNOME/gnome-remote-desktop/-/blob/00195e889ad9390d4fa462d291a70fa92fe64678/src/grd-rdp-dvc-camera-device.c#L1388
