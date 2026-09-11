# Native Runbook

This runbook tracks the current native-only webOS target. The native app must not fall
back to a web app, JavaScript service, MSE, WebCodecs, RDCleanPath, or browser rendering.
AVC420/H.264 is preferred through the in-house NDL DirectMedia backend
(`backend_ndl`, dlopen of `libNDL_directmedia.so.1`); RemoteFX is supported only as a native
Rust/IronRDP decode path with RGBA updates presented by SDL.

## webOS TV compatibility

One ARM32 package targets webOS TV 5.0, 6.0, and TV 22 through TV 26. At
startup the native LS2 client performs one bounded `getSystemInfo` request for
`sdkVersion` before any RDP, miniaudio, or NDL thread starts. SDK 1-4 is
rejected; an unavailable version falls back to the DirectMedia ABI probe.

The backend selects the ABI from one facade's symbols, never from a guessed
release cutoff: `DirectMediaSetWindowId` means the one-argument Init profile;
without it, a complete v2 control plane uses callback Init with a NULL
resource callback. The guaranteed common path is H.264 plus S16LE mono/stereo
PCM. TV 23 / SDK 8.x is the reference device. Other generations remain
ABI-supported candidates until the same IPK passes launch, H.264/PCM,
reload/IDR, reconnect and concurrent audio/video smoke tests on that runtime.
SDK 11 is treated as TV 26 pending confirmation from the first device audit.

No NDL library is packaged or linked. Product builds retain the existing
Buildroot compiler and compile against backend_ndl's private minimal ABI
declarations, so the webOS NDK is not a build dependency. When an official ARM
target NDK is available, use backend_ndl's separate optional conformance test
to compare enum values, layouts and signatures; the x86_64 NDK sysroot is
rejected.

## Multi-RDP Sessions (red/green/yellow/blue)

The app can hold up to four simultaneous RDP sessions to different servers, mapped to
the TV remote's color buttons in the remote's own order: red, green, yellow, blue
(slot ids 0..3):

- **Any color button** → "go to that session's screen": the live stream when the slot is
  connected, a direct connection attempt when its saved profile is offline, or that
  colour's setup drawer when it is empty. The app starts on the four-profile hub,
  selecting the first configured profile (red when none is configured). **Save and
  connect** always connects the profile whose drawer is open. Exiting is left to webOS
  itself (EXIT/home button -> system close, delivered to the app as
  SDL_QUIT/SDL_APP_TERMINATING).
- **One press of the remote's central OK button** while a desktop is visible opens HUB.
  An H.264 session remains on the fullscreen hardware plane; a RemoteFX session's cached
  RGBA desktop is drawn into the same SDL backbuffer as HUB before its translucent chrome.
  Both paths remain visible beneath one darkening mask: a left-to-right
  96/90/62/22/0% gradient and a bottom-to-top 92/80/50/18/0% gradient. Their clear
  upper-right area leaves the stream untouched; browsing cards never swaps the background.
  USB input is released, and selecting a connected
  card resumes or switches explicitly. Remote OK is identified by its evdev device origin,
  so Enter on a physical USB keyboard still goes to the GNOME host. BACK closes a nested
  setup/onboarding layer first; on the unobscured HUB it returns to the session that was
  on screen when HUB opened. A new connection started from HUB keeps that return target
  until the new worker is ACTIVE, so validation/start/network failures remain returnable.
- **Keyboard Esc** follows the same return path: close an open dropdown, cancel Edit
  without saving, then return from HUB to the live session. It works without a focused
  widget and while a connection is pending; holding Esc dismisses only one layer.
- **HUB D-pad navigation**: LEFT/RIGHT move across all four computer cards, UP enters
  the selected card's action controls, and the remaining directions move between
  Connect/Resume, Edit, Help, and the selected card. OK activates the focused element.
  Colour keys and CH +/- remain optional direct shortcuts.
- **Connection status**: `REBOOT`, `SHUTDOWN`, `SIGNED OUT`, and `CLOSED` identify
  normal session endings in grey. `REPLACED`, `TIMED OUT`, `LOST`, and `FAILED` are
  amber; protocol, graphics, and server failures remain red `ERROR`. The selected
  card explains the last disconnection. A transport close without a server reason
  never implies a reboot. Server shutdown/reboot codes are `0x19`/`0x1A` in
  [MS-RDPBCGR Set Error Info](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-rdpbcgr/a21a1bd9-2303-49c1-90ec-3932435c248c).
  `RpcInitiatedDisconnect` is also used by GNOME daemon handoffs. It and ambiguous
  MCS user/provider disconnects allow three total attempts, one second apart,
  displayed as `RETRYING`. Other explicit server terminations require manual
  Connect/Retry. The last reason survives worker cleanup
  until a new connection or profile change; it is not persisted across app launches.
  The C/Rust callback is `on_state(ctx, state, reason, detail)`: `reason` is the shared
  `RdpDisconnectReason` enum, while `detail` is synchronous diagnostic text for logs.
  Rebuild both sides together after ABI changes; UI text comes from the typed reason.
- **Brand and type** follow the `dc_v3` prototype: the four-colour cube rotates in HUB
  and onboarding. Pre-generated IBM Plex Sans fonts render headings, controls, and
  prose; IBM Plex Mono renders values, metadata, fields, badges, and scales; JetBrains
  Mono is reserved for the `lgnome_` wordmark. The TV package does not parse or ship
  runtime TTF files.
- Switching to a connected slot moves the VIDEO plane and — KVM-style — the keyboard and
  mouse to it; a colored square in the top-right corner confirms the active slot for
  ~1.5s. Navigating to another slot's setup drawer leaves the previous session running
  backgrounded (graphics suppressed server-side, audio still in the mix).
- **Audio is mixed from ALL connected sessions** — a backgrounded session keeps playing
  its sound. Each connected HUB card carries a compact stereo VU indicator; its rails
  follow the post-fader L/R levels once the stream opens. The hero line stays neutral
  until the first stream arrives, then shows the negotiated codec, sample rate, and
  channel layout. The client
  advertises Opus 48k + PCM; gnome-remote-desktop prefers Opus
  (~96kbps per session instead of ~1.4Mbps raw), each session decodes it in-process via
  libopus on its own worker thread, and the headless miniaudio engine sums `ma_sound`
  voices as float PCM at 48kHz stereo in 480-frame (10ms) blocks. Each session has its
  own SPSC ring and dynamic converter, so 44.1kHz PCM and 48kHz Opus can play
  simultaneously. NDL receives paced
  S16LE from the engine pump. PCM-only servers keep working unchanged; builds with
  `LGNOME_WITH_OPUS=OFF` mute Opus sessions with a log.
- **Audio quality** is a global setting (`audioCodec`, also a pre-connect UI dropdown and
  the `--audio-codec` CLI flag): `auto` (default) lets the server pick Opus; `pcm` offers
  the server PCM only, trading ~1.4Mbps per session for a lossless stream. It applies to
  connections made after the change. grd's PCM is 44.1kHz while Opus decodes at 48kHz;
  the per-source converters handle both concurrently without re-pinning the sink.
- **Camera and microphone redirection** use two independent privacy toggles per profile
  in the setup drawer. Saving a changed camera or microphone choice
  restarts that profile because the RDPECAM/RDPEAI channels and `INFO_AUDIOCAPTURE`
  client flag are negotiated at connection time. The HUB's camera-icon drawer owns the
  app-global camera device, resolution, frame rate, microphone device, and input gain.
  Device dropdowns offer `Auto` and stable locally enumerated endpoints. Camera and
  microphone payloads go only to the on-screen profile. The capture gate synchronously
  drains the old owner before a handoff and purges already-copied PCM. A background
  profile keeps RDPECAM capability negotiation but receives `DeviceRemoved`, so its
  device DVC and PipeWire camera are closed instead of retaining sample credits. The
  foreground profile receives `DeviceAdded`; the server opens a fresh device DVC and the
  client reopens physical capture and suppresses dependent frames until the first
  self-contained SPS+PPS+IDR access unit. An already-open camera consumer might need to
  reopen or reselect the new PipeWire node. RDPEAI remains negotiated for background
  profiles but sends no PCM. A whole-session reconnect requested by the separate video
  recovery policy still renegotiates every DVC. Incoming playback audio remains mixed
  from every connected profile.
- RDPECAM lifecycle requests are serialized on the device DVC. `StopStreamsRequest`
  is idempotent while the device remains activated: the first stop invalidates the
  capture generation and completes every accepted sample credit with `SampleError`,
  then it acknowledges the stop; a repeated stop is acknowledged without another
  physical-capture transition. A later `StartStreamsRequest` starts a fresh generation.
- The camera must emit H.264 itself; there is no transcode anywhere in this client.
  Redirection opens only a non-emulated, single-plane `V4L2_PIX_FMT_H264` mode at the
  exact selected geometry and frame rate (640x480 at 15 fps by default). It consumes the
  driver's `bytesused`, sequence, error flags, and timestamp and accepts only Annex-B
  access units beginning with a start code. YUYV, MJPEG, and `V4L2_PIX_FMT_H264_NO_SC`
  are not fallback formats. The jail exposes exactly two camera nodes, `/dev/video0` and
  `/dev/video1`, as fixed entries whose minors do not track host numbering, so a camera
  that exposes H.264 on a later node is unusable on the TV. Bitrate, GOP/I-period, joined
  headers, and repeated sequence headers are requested best-effort, as is force-key-frame
  while the stream waits for a decoder seed. None of these are required: the webOS SDK
  header exports no `V4L2_CID_MPEG_*` control at all, so on the TV every one of them
  compiles out and the camera keeps its own bitrate and GOP cadence.
- The camera stream uses the shared H.264 scanner, caches SPS/PPS, and prepends the cache
  to an IDR that lacks its own parameter sets. It sends no P/B picture until it has a
  self-contained SPS+PPS+IDR decoder seed. Copied access units live in a FIFO limited to
  eight AUs and 8 MiB; one normalized AU may not exceed 4 MiB. A corrupt flag, malformed
  or oversized AU, FIFO overflow, ownership handoff, or refused Rust mailbox submission
  clears the FIFO, answers an accepted RDPECAM credit with `SampleError`, and reopens
  V4L2 until a new decoder seed arrives. Camera recovery does not stop the microphone
  worker or RDP session. Eight consecutive valid but unsendable AUs (for example,
  dependent pictures or IDRs without cached SPS/PPS) instead answer the current credit
  with `SampleError` while preserving V4L2, sequence, and SPS/PPS state. Capture
  continues through a long GOP until a later decoder seed is available; the accepted
  credit cannot wait forever.
- A jump in `buffer.sequence` is treated as dropped buffers, not as a broken device: it
  re-gates the decoder seed and fails one credit, but keeps V4L2, the FIFO, and the
  cached SPS/PPS. Reopening on a gap would be strictly worse, because it also discards
  the seed and restarts mid-GOP. Cameras whose sequence counter jumps periodically would
  otherwise loop through open, gap, `SampleError`, reopen, which surfaces on the server
  as unexpected errors, timeouts, and stray responses. Missing or unplugged cameras are removed
  from RDPECAM and retried in the background.
- **Camera purchase recommendation:** buy a camera with hardware/native H.264 support
  for camera redirection. Current gnome-remote-desktop officially accepts only
  `CAM_MEDIA_FORMAT_H264` through RDPECAM; YUYV and MJPEG are not directly supported.
  At upstream `main` commit `00195e889ad9390d4fa462d291a70fa92fe64678`,
  [`has_supported_media_type()`][grd-camera-media-format] returns support only for that
  format. Before buying, verify that Linux/V4L2 actually reports
  `V4L2_PIX_FMT_H264` at the exact required resolution and frame rate; a marketing H.264
  claim without driver exposure is insufficient. The previously used Logitech Brio 300
  may be incompatible if its V4L2 node does not provide native H.264.
- Device acceptance: native H.264 capture was observed on an Adesso CyberTrack H5 at
  640x480/15 fps; the earlier Brio 300 software-encoding path does not qualify this
  implementation. Still qualify long-stream reference integrity, Stop/Start,
  reconnect, unplug/replug, and foreground handoff between two capture-enabled
  sessions. RDPEAI was observed streaming ALSA 48 kHz mono resampled to negotiated
  44.1 kHz stereo and must remain live during these checks.
- Microphone redirection opens ALSA through `dlopen` (there is no hard `libasound`
  dependency), preferring 48 kHz mono and then stereo. The input is gain-adjusted,
  linearly resampled and packetized independently for every 44.1 kHz stereo S16LE stream
  negotiated by gnome-remote-desktop over RDPEAI (1.4112 Mbit/s of PCM payload for the
  foreground stream, before protocol overhead). Capture failure degrades to correctly
  paced silence for the foreground consumer and retries in the background; background
  streams send no PCM. It never terminates video or an RDP session.
- **Adaptive delay is always enabled and has no user control.** Every source starts at
  60ms and ranges from 40–260ms. The target covers both the producer's block cadence
  and a rolling 10s histogram of relative arrival/capture-time variation
  (`max(block cadence, p95) + 20ms`, 5ms buckets). In the measured Windows session,
  PCM arrived in roughly 186ms blocks, compared with roughly 20ms Opus packets from
  gnome-remote-desktop. Even punctual 186ms blocks drain a shorter standing queue
  between arrivals; the 260ms ceiling leaves room for that cadence and headroom.
  RDPEA timestamp
  wrap is handled; zero/repeated timestamps fall back to decoded duration. A gap over
  500ms is a talkspurt boundary and re-primes at least 60ms rather than polluting the
  jitter estimate. New peaks or a longer block cadence raise the target immediately;
  underruns postpone decay. Stable delivery can lower it by at most 10ms per five
  seconds, never below the cadence-derived floor or 40ms.
- Queue error beyond 10ms changes each source's SRC ratio smoothly, capped at +/-0.5%
  by a 50ms error, with a slow drift term for mismatched producer/DAC clocks. A backlog
  over target by 80ms fades out for 5ms, drops old frames to `target + 20ms`, resets the
  converter, and fades in for 5ms. Underrun inserts silence and re-buffers only that
  source. The 1.5s ring is an emergency limit, not normal latency; producer overflow
  requests a consumer-side trim and never moves the consumer cursor.
- The mixer overlay shows `queue/target ms` above every session. Detailed p95 jitter,
  SRC ppm, underrun, hard-correction, and overflow counters are logged as one snapshot no
  more often than every three seconds.
- A backgrounded session's graphics are paused server-side via TS_SUPPRESS_OUTPUT_PDU.
  Because grd resumes with a delta frame and rejects Refresh Rect, switching back forces
  a fresh IDR by re-submitting the Display Control monitor layout (grd rebuilds its
  encode session on every layout PDU, even an identical one → RESET_GRAPHICS + IDR). If
  no decodable frame arrives within 2s of a switch (e.g. grd mirror mode has no Display
  Control channel), the client reconnects that session once — a fresh connection always
  starts with an IDR — and remembers the slot as refresh-ineffective, so later switches
  skip the doomed wait.
- **IDR-snapshot backgrounding** (refresh-ineffective slots): a plain suppress would
  strand such a slot behind an unusable delta chain (grd emits its only IDR at connect),
  so leaving its stream instead reconnects the slot invisibly — behind the new active
  screen — and caches the fresh connection's compressed IDR (+ any deltas racing the
  suppress) in an 8MB/slot AU snapshot; the server is suppressed once the IDR is in
  hand. Switching back replays the cache into the shared decoder (same-size in-band
  handover, no pipeline reload, no splash) and resumes output without a refresh: the
  server's next delta references exactly the replayed state. The first frame shown is
  the desktop as of backgrounding time; the resume delta catches it up. Cache overflow
  (a server that keeps streaming despite suppress) voids the snapshot and switch-back
  falls back to the visible reconnect. `LGNOME_SNAPSHOT_FORCE=1` arms snapshot
  backgrounding for every slot without waiting for the watchdog to learn — the on-device
  experiment knob for validating that grd's resume delta really continues the cached
  chain.
- **Deferred switching**: a color-key press whose target stream is not ready (no cached
  IDR yet — first switch to a slot, a manual-Connect flag reset, a flip-back racing the
  suppress tail) no longer moves the screen into a black reload window. The CURRENT
  stream keeps playing (the badge shows the destination), the target fills its snapshot
  in the background — keyframe request first, hidden reconnect after the 2s no-keyframe
  deadline (which is also where refresh-ineffective is learned, now invisibly) — and the
  switch completes by replay once the cache is ready and quiet (~0.5s). Re-pressing the
  CURRENT slot's key cancels the deferred switch. Trade-off accepted deliberately: the
  hidden reconnect of a backgrounding slot interrupts that session's own audio in the
  mix for ~1-2s (`source N ran dry` + a one-cut backlog trim on rejoin) — the price of
  instant switch-backs on servers that only ever emit their connect IDR.
- **Channel-rocker ring switching**: during streaming CH_UP/CH_DOWN zap through the
  connected slots (red → green → yellow → blue → red; empty slots are skipped and stay
  reachable via their color buttons). Handled on both delivery paths like the color
  keys: SDL scancodes 480/481 from the ungrabbed RCU node and evdev KEY_CHANNELUP/DOWN
  402/403 from grabbed remote nodes. Channel keys mean nothing inside an RDP session,
  so nothing is taken from the remote desktop; the D-pad stays free for the mixer
  overlay. SDL key presses during streaming are logged (`remote sdl key scancode=`) —
  with the keyboard evdev-grabbed, only the remote and the system reach SDL, so the log
  maps what a given remote firmware actually sends.
- **Re-pressing the ACTIVE slot's color button** (while its stream is on screen) — or
  pressing the mouse's **extra/forward button** — opens the volume mixer; the **side/back
  button** opens HUB instead, mirroring the central remote button. (Extra mouse buttons
  are never forwarded to the server, which maps only left/right/middle.) The mixer is
  a compact floating console above the desktop, with a rounded border and
  one fader channel per slot. Each channel is
  an L/R pair of LIVE volume-meter columns (post-fader peak, instant attack / 30 dB/s
  release, gradient anchored to the scale) with one white knob across both and a dBFS
  scale: -60 at the bottom stop (= full mute) up to an unmarked +6 dB headroom above the
  0 line. Up/down move the selected fader 3 dB per press (applied to the live mix
  immediately); left/right — or another slot's color button — change the selection; the
  active slot's button, an extra mouse button, OK, or Back closes it, and it auto-hides
  after ~6s without input.
  Mute, Duck, and Solo are shown as the single-letter console controls `M`, `D`, and `S`.
  While it is open the pointer belongs to the SYSTEM (the evdev grab is released and the
  plain arrow shown): click a fader to jump/drag it, click a channel elsewhere to select
  it, scroll the wheel for 3 dB steps, click outside the panel to close.
- **MASTER fader** (white, rightmost channel): mirrors and drives the webOS SYSTEM
  volume (`luna://com.webos.audio` get/setVolume — undocumented-but-working from the
  app's jail; officially LG only documents volumeUp/Down/setMuted, so an unavailable
  bus just leaves the channel dimmed). Its percentage guide labels share the same
  horizontal lines as the source dB scales. Those guide labels are rounded to the
  nearest 5%, while the large current value always shows the exact system percentage.
  It never touches the app's own mix: its meters
  show the mix OUTPUT (the summed, saturated chunk leaving for the audio track) and its
  knob travels volume percent over the full track while wearing the same dBFS scale
  artwork as the other channels (deliberate — a uniform bank; the user chose the "wrong"
  scale over a mismatched one). Steps of 3 per key press/wheel notch — one remote VOL
  press moves the system volume by 3, so keys and fader land on the same grid.
  TRANSPORT IS THE NATIVE luna-service2 CLIENT on a GMainLoop thread — NEVER a
  luna-send-pub subprocess: fork()ing the app while the NDL/OMX pipeline (re)loads
  races its in-process decoder state (black screens observed live), and a pipe-spawned
  subscriber cannot deliver events anyway (block-buffered stdout, no stdbuf on the TV).
  Volume-change SUBSCRIPTIONS DO NOT WORK for a dev-mode app on webOS TV Lite
  (webOS 23 generation, internal release 8.x) — probed live:
  com.webos.audio/getVolume accepts the subscribe but the DYNAMIC service idles out and
  takes it along ("com.webos.audio is not running"; zero events even while alive);
  master/getVolume subscribe → "Message status unknown"; apiadapter (the route external
  SSAP controllers get their change events from) → "Not permitted"; a NAMED bus client
  → "Invalid permissions" (the jail allows anonymous only). The cache is therefore kept
  live by polling getVolume every 200ms while streaming — in-process LS2 one-shots,
  cheap, and each call wakes the dozing service. Set calls are coalesced to the newest
  value on the loop thread; the cache updates optimistically for a snappy knob.
- **Auto-raise on volume change**: while streaming, any system-volume change made
  outside the overlay (remote VOL keys, BT-headphone buttons) pops the mixer with the
  MASTER channel selected; while the volume keeps moving the auto-hide timer keeps
  resetting, and once it settles the ~6s idle timeout hides the panel as usual. The
  first reading after launch is the baseline, so nothing pops at startup. No mouse or
  keyboard input reaches the RDP session while the panel is up; the grab and the
  session's cursor shape come back when it closes. Levels are per launch (not persisted), survive reconnects
  and source format resets, and disconnected slots show dimmed knobs whose level applies once
  they connect. On the RemoteFX RGBA fallback path the overlay cannot be drawn over the
  stream, so the button keeps its old badge-flash behavior there.
- If the ACTIVE session drops while another one is alive, video auto-switches to a
  survivor; if none is left, the pre-connect UI returns.

## Local Config

Local native config lives at `native/config.local.json`. It is gitignored and may contain
passwords. Do not commit it or paste it into logs.

Example shape (legacy flat form, applies to the green slot):

```json
{
  "host": "192.0.2.10",
  "port": 3389,
  "username": "gnome-user",
  "password": "replace-with-local-secret",
  "domain": "",
  "fps": 60,
  "wheelStep": 60,
  "wheelScrollDivisor": 1
}
```

All slots can be configured with the session-array shape (this is also what the app persists).
`name` is an optional display label; older files without it remain valid:

```json
{
  "sessions": [
    { "slot": "green", "name": "Studio PC", "host": "192.0.2.10", "port": 3389, "username": "u", "password": "...", "domain": "", "fps": 60, "cameraRedirect": true, "audioInputRedirect": true },
    { "slot": "yellow", "name": "Media Server", "host": "192.0.2.11", "port": 3389, "username": "u", "password": "...", "domain": "", "fps": 60, "cameraRedirect": false, "audioInputRedirect": false }
  ],
  "wheelStep": 60,
  "wheelScrollDivisor": 1,
  "audioCodec": "auto",
  "cameraEnabled": true,
  "cameraDeviceId": "",
  "cameraWidth": 640,
  "cameraHeight": 480,
  "cameraFps": 15,
  "audioInputEnabled": true,
  "audioInputDeviceId": "",
  "audioInputGainDb": 0
}
```

An empty device ID means `Auto`; non-empty IDs are stable `v4l-by-id:`/`v4l2:` camera
IDs or `alsa:` microphone IDs written by the on-TV form. Camera dimensions must be even
and are limited to 640..1920 by 480..1080 at 1..30 fps — sub-VGA modes were retired, and
saved settings below VGA are rejected and fall back to the defaults. Those bounds are
sanity checks on untrusted config, not a menu: the HUB camera drawer is filled from
`VIDIOC_ENUM_FRAMESIZES` and `VIDIOC_ENUM_FRAMEINTERVALS` on the selected camera, so it
offers exactly the discrete native-H.264 sizes and rates that camera reports, and a size
with no usable rate is dropped. Microphone gain is limited to -12..+18 dB. If no such mode is visible, that camera is not
currently usable for RDPECAM. JSON preserves schema-compatible custom values, but the
capture worker still refuses them unless V4L2 confirms the exact native-H.264 mode.

The hub's setup drawer saves a profile immediately on **Save** or **Save and connect**, so
a failed first connection does not discard the optional profile name, address, username,
domain, password, or FPS. **Delete profile** removes that colour's saved credentials after
confirmation. The camera-icon drawer saves app-global camera and microphone settings
independently. Unsaved drafts in other drawers are never included. The save path is
resolved from a candidate list (each rejection is logged with its reason); on the TV the
winner is the in-app
`<approot>/settings/<euid>/settings.json` — the IPK ships `settings/` mode 01777 because
the install tree is root-owned, and the app creates a private 0700 subdir inside (same
trust model as /tmp). Verified on device: this survives app restarts, package
reinstalls AND TV power cycles; `/media/developer/temp/<appid>-<euid>` is the next
candidate (persistent ext4), and the tmpfs `/tmp/<appid>-<euid>` fallback is last —
loading scans the same priority order, so settings migrate upward on the next save.
Explicit webOS launch params or CLI flags override loaded settings (flat launch/CLI keys
target the green slot). Set `LGNOME_IGNORE_SAVED_CONFIG=1` for a one-off launch that
ignores saved UI settings.

Changing connection fields on a still-live background profile and pressing **Save** ends
that old worker, so the newly labelled profile can never resume the previous computer by
mistake. **Save and connect** replaces it immediately; a name-only edit leaves it running.

Resolution belongs to each profile (`desktopWidth` / `desktopHeight`). Legacy global
`width`, `height`, and `audioPrebufferMs` JSON keys are ignored, as are the retired
`--width`, `--height`, and `--audio-prebuffer-ms` CLI flags. Audio buffering is adaptive.

## Local Build And Test

Targeted local loop:

```sh
./tools/syntax-check-native.sh
cargo test --manifest-path webrdp-min/Cargo.toml --features native native::
cmake -S native -B /tmp/lgnome-native-build-tests
cmake --build /tmp/lgnome-native-build-tests
ctest --test-dir /tmp/lgnome-native-build-tests --output-on-failure
```

Add `-DLGNOME_WITH_OPUS=ON` to the configure step to include the `audio-opus` test; the
flag is off by default on host builds (ExternalOPUS downloads the libopus source tarball)
and on only for webOS cross-builds.

Build and link the native shell against the real Rust static library:

```sh
cargo build --manifest-path webrdp-min/Cargo.toml --features native
cmake -S native -B /tmp/lgnome-native-rust-build \
  -DLGNOME_LINK_RDP_FFI=ON \
  -DRDP_FFI_LIB="$PWD/webrdp-min/target/debug/libwebrdp_min.a"
cmake --build /tmp/lgnome-native-rust-build
```

## webOS Build, Package, Install, Launch

The product webOS build requires:

- webOS buildroot toolchain;
- initialized `third_party/backend_ndl`, `third_party/IronRDP`,
  `third_party/lvgl`, and `third_party/miniaudio` submodules;
- SDL2 for the webOS target;
- Rust target support for `armv7-unknown-linux-gnueabi`.

Default toolchain path:

```text
/opt/arm-webos-linux-gnueabi_sdk-buildroot/share/buildroot/toolchainfile.cmake
```

Build and package:

```sh
./tools/build-native-webos.sh
```

Do not use `arm-unknown-linux-gnueabi` for the Rust staticlib on TV. It can emit
legacy CP15 barrier instructions that crash with `Illegal instruction` on ARMv8 webOS.

`tools/build-native-webos.sh` builds the Rust armv7 staticlib in `release` mode by
default, configures the product CMake build with NDL/SDL/LVGL/RDP FFI enabled,
stages the app, verifies the staged tree and IPK, and writes the package under
`dist/native-webos/`. Use `NATIVE_WEBOS_RUST_PROFILE=debug` only for diagnostics.

### Release steps

Bitbucket `main` is the development history. The GitHub repo is an append-only chain of
release snapshots — one commit per version, tags never moved — produced by
`tools/release-github.sh <version>`. For any version:

1. Merge everything intended for the release and obtain a green `main`.
2. If the `backend_ndl` pin has moved past the snapshot published on its mirror, release
   `backend_ndl` first, then update the pin and `third_party/PROVENANCE.md` here. The
   released `.gitmodules` points at the public mirror rather than the private dev remote,
   so `release-github.sh` refuses to publish while the mirror's `main` tree differs from
   the pin — otherwise the snapshot would ship a gitlink nobody can resolve.
3. Bump the version in `native/deploy/webos/appinfo.json`; the script snapshots `main`'s
   tree verbatim. Per-version notes belong in the GitHub Release, not in the repo —
   `README.md` describes the current client, so it does not accumulate a changelog.
4. Build a fresh IPK from that exact `main`, record its SHA-256, deploy that same file,
   and repeat the smoke test.
5. Publish the snapshot and tag with `tools/release-github.sh`, wait for GitHub CI, then
   create the Release with that same verified IPK.

The GitHub remote must be reachable for a push; `RELEASE_REMOTE=<name>` selects a
different one (for example an HTTPS remote when no SSH key is registered with GitHub).

Install and launch:

```sh
ARES_DEVICE=<tv-device> ./tools/deploy-native-webos.sh --ipk /path/to/verified.ipk
```

### Local USB camera preview probe

The launch-only camera probe bypasses RDP and opens the first V4L2 capture node that
accepts uncompressed 640x480 YUYV. It renders that stream in the normal native SDL
window and exits on the remote's BACK button:

```sh
ARES_DEVICE=<tv-device> ./tools/deploy-native-webos.sh --no-install --camera-preview
```

This is a YUYV-only diagnostic. It proves only local camera capture and presentation;
it does **not** prove that the camera provides native H.264 or is compatible with
RDPECAM. It is independent of the persisted per-profile RDPECAM setting and does not
connect an RDP session.

### Opening a profile without the remote

The hub waits for a colour-button press, which an automated on-device check cannot send.
`connectSlot` names a saved profile to open at launch instead:

```sh
ARES_DEVICE=<tv-device> ./tools/deploy-native-webos.sh --no-install --connect-slot red
```

`--connect-slot red` does the same for host runs. This is a debugging aid, not a
preference: it is never persisted, carries no connection data, and the named profile must
already be configured — the connection then follows the ordinary user-initiated path, so
it cannot skip validation or reach a state the remote could not. An unconfigured or
unknown name is logged and leaves the hub as it was.

`tools/deploy-native-webos.sh --ipk PATH` verifies and installs that exact package,
then launches with saved profiles and no configuration parameters. It does not build
or select an IPK automatically. `--no-launch` installs only; `--no-install` launches
the installed version. The CMake `webos-install-native` target likewise requires
`-DLGNOME_NATIVE_IPK=/absolute/path/to/package.ipk`.

Choose at most one launch mode: `--connect-slot COLOR`, `--config FILE`,
`--with-defaults`, or `--camera-preview`. These modes close a running instance before
launching so startup parameters take effect. `--config` sends an explicit JSON object;
it must be below 16 KiB and cannot contain launch-control keys. The app validates
settings fields. `--with-defaults` explicitly bypasses persisted profiles for a smoke
check. Ordinary updates need neither option.

The local SDL UI canvas is 1920x1080; the hardware video plane uses the server's actual
resolution independently. Each profile requests its saved desktop size (3840x2160 by
default). A mirroring server can override it; the reported runtime size controls video
and input mapping.

The native app id is `com.truebest.lgnome.native`. The native package must contain the
native executable and native `appinfo.json`; verification also checks the app identity and
rejects packaged settings (only an empty `settings/` directory is allowed). It rejects browser/runtime files such as
`*.html`, `*.js`, `package.json`, and historical `app/` or `service/` trees.

## Audio Device Acceptance

NDL is the selected production sink. Accept it on TV with
30 minutes of 4K/60 video plus continuous audio, at least two audible sessions, stable
target returning to no more than 60ms for 20ms source blocks (or block cadence plus
20ms for larger blocks), no underrun for jitter within the current target,
and recovery from gaps above 150ms without permanent backlog. CPU regression must stay
within 5 percentage points and RSS within 4MB.

An independent SDL callback sink was considered but is not part of the rollout: retaining
NDL avoids qualifying a second webOS device path and keeps audio behavior consistent with
the already proven TV media stack. Do not add an SDL audio sink or a custom NDL `ma_device`
backend without a new explicit design decision. WSOLA/PLC remains deferred unless the
current +/-0.5% SRC and rare fade/drop corrections prove insufficient.

### System volume

The MASTER fader controls webOS system volume independently of app mix gains.
It uses an in-process LS2 thread: spawning a Luna subprocess during NDL reloads
caused black video on the tested TV. Sets coalesce to one in-flight request;
getVolume replies update the authoritative cache, while setVolume confirmations
can echo obsolete values during a drag.

Developer Mode probes on that TV found no usable volume subscription:

| Endpoint or registration | Observed result |
| --- | --- |
| `com.webos.audio/getVolume` subscription | Accepted, but no change events; the dynamic service later idled out. |
| `com.webos.service.audio/master/getVolume` | `Message status unknown`. |
| `com.webos.service.apiadapter/audio/getVolume` | `Not permitted to send`. |
| Named LS2 client registration | `Invalid permissions`; anonymous registration worked. |

The client therefore polls getVolume. Firmware that rejects get/setVolume leaves
the MASTER control unavailable without affecting session audio.

## Native logging

The C shell, Rust RDP core, and the NDL adapter all end at the embedded `clog` C11
logger. A C source file defines its default prefix/category once, then ordinary calls
contain only the level and message:

```c
clog_define(g_native_log_config, cLogLevelTrace, "ExampleApp");

clog(cLogLevelNotice, "Hello, World!\n");
clog(cLogLevelError, "Error: %d\n", err);
```

A C file that logs declares exactly one file-scoped `clog_define`. Pure helpers need
neither a category nor an artificial log call. The standalone `backend_ndl` library retains its callback API instead of depending
on the application logger. A category string is both the printed prefix and the selector
used for live configuration. It is 1-63 ASCII bytes: alphanumeric, `_`, or `-` components
separated by single dots.
The fixed format prints local wall time, monotonic time since launch, severity, and
category; trace/debug and `diagnostics.*` messages also include source metadata.
Production output goes to stderr. Tests can install a capture hook before starting
logging threads and remove it after joining them.
The lowercase `clog` macro shares the C99 complex logarithm's name, so a translation
unit using `<complex.h>` must not include `clog.h`.

By default the application prints `info` and higher to stderr; the native executable
redirects stderr to `/tmp/lgnome-native.log` unless `LGNOME_NATIVE_LOG_PATH` overrides
the path. Set `LGNOME_LOG` before launch to change thresholds:

```sh
LGNOME_LOG='*=info,native=debug,rdp=debug,audio=debug,media.ndl=debug,input=warn'
```

This is an environment variable of the native process. The host-side
`deploy-native-webos.sh` environment is not forwarded by `ares-launch`; use it for direct
launches/debug shells or call the live `clog_configure()` API from an in-process debug
control surface.

Rules are comma-separated, case-sensitive `selector=level` pairs. A selector matches both
the named prefix and dot-separated descendants; later rules win, and an empty value
restores compiled defaults. Levels are `trace`, `debug`, `info`, `notice`, `warn`, `error`,
`fatal`, and `off`. Invalid rule strings are rejected as a unit. Application prefixes are
`native`, `config.paths`, `config.settings`, `video.snapshot`, `video.ndl`, `audio.opus`,
`audio.pipeline`, `audio.ndl`, `input.evdev`, `input.sdl`, `media.ndl`, `ui.preconnect`,
`ui.mixer`, `cursor`, `luna.volume`, `video.h264`, `video.rgba`, `camera.v4l2`,
`camera.h264`, `capture.redirect`, `audio.input.alsa`, `audio.input.pcm`, `rdp.rust`,
and `rdp.stub`.
The `media.ndl` category also carries the standalone backend_ndl library log, forwarded
through the media adapter's callback. Rust/IronRDP events use `rdp.rust`; their original
tracing target (including `webrdp.transport`, `webrdp.session`, `webrdp.graphics`, and
`webrdp.audio`) is retained in the message. Parent selectors such as `audio=debug` or
`rdp=debug` cover every dotted child. The logger deliberately does not print credentials
or config-file contents.

`LGNOME_LOG` is the only runtime log-level control. The obsolete `WEBRDP_LOG`,
`GNOMECAST_LOG`, and `GNOMECAST_NDL_LOG` variables and the `/tmp/gnomecast-ndl-debug`
marker are not supported.
Rust probes the enabled `rdp.rust` levels when a session worker starts (pruning disabled
trace/debug callsites), re-checks the level before formatting each surviving event, and
forwards the structured level, tracing target, and message synchronously. Events outside
an active RDP worker have no native callback and are dropped.

`tools/check-logging-policy.sh`, invoked by `tools/syntax-check-native.sh`, enforces category
coverage and rejects direct stderr/SDL logging in application C plus stderr logging in
production Rust.

## NDL Backend Smoke (on-TV, run after backend changes)

Watch `/tmp/lgnome-native.log` over ssh while exercising. For sink-level
telemetry, launch the native process with
`LGNOME_LOG='media.ndl=debug'`. The adapter mirrors the effective `media.ndl` level
into the backend's minimum log level when media opens, so debug telemetry is not even
produced while the level is filtered out. Every 300 accepted video AUs, `video sink`
reports the DirectMedia render-buffer getter and the EWMA/maximum duration and error
count of the synchronous `NDL_DirectVideoPlay` call. These are pre-display diagnostics:
neither the queue nor the call duration measures compositor, scanout, or panel latency,
and they are not used as pacing feedback. Audio drop episodes always end with an INFO
`audio sink recovered: dropped N block(s)` summary, so the default log distinguishes a
transient drop from a dead sink.

1. **Probe/init**: startup shows `[backend-ndl] loaded NDL library: libNDL_directmedia.so.1`
   (or which candidate/RTLD_DEFAULT won), any missing optional symbols, and
   `initialized app_id=com.truebest.lgnome.native`. A dlopen or `NDL_DirectMediaInit`
   failure logs the exact `NDL_DirectMediaGetError()` text.
2. **Connect**: green session → video within ~2 s of the first IDR
   (`media loaded: generation=N video=yes`), audio starts immediately (the empty-frame
   priming runs after every load; a long first-audio delay = priming regression).
3. **Server resolution change**: desktop resize → video track reopen on the next IDR,
   audio recovers after the reload (`reopening video on next keyframe` flow in main.c).
4. **Session switching (color buttons)**: snapshot replay succeeds — replayed AUs must
   count as progress (`fed N AUs` advances); all-DROPPED replay means the pipeline was
   fed while unloaded.
5. **Audio-under-live-video open**: first audio negotiation after video is up re-arms
   the keyframe gate (`NEED_KEYFRAME` → refresh request → picture recovers).
6. **Background/relaunch**: Home out and relaunch; media tears down only on real exit.
7. **Camera/microphone redirection**: first confirm the camera itself emits H.264 — its
   first capture node inside the jail must list a non-emulated `V4L2_PIX_FMT_H264` for at
   least one HUB mode, since nothing here transcodes. Choose both devices in the HUB,
   enable both toggles on two profiles, and open them from normal GNOME applications.
   Verify that each start/restart begins with a clean SPS+PPS+IDR picture, then run long
   enough to expose reference artifacts. Exercise Stop/Start, RDP reconnect,
   unplug/replug, a webOS overlay, foreground handoff to the second session, and switching
   back. The background profile must send no camera or microphone payload while its
   incoming playback audio keeps feeding the mix. The foreground microphone and RDP
   session must survive camera recovery. Repeatedly close and reopen a GNOME Camera/Zoom
   consumer; each stop must be acknowledged successfully, the PipeWire `lgnome Camera`
   device must remain available (its node serial may be recreated), and the next open
   must restart video without reconnecting RDP.
8. **30 min soak**: RSS stable (`/proc/<pid>/status` over ssh), no log spam from
   `not ready`/`overflow` (each is log-once per episode), no A/V drift.

`resource released by firmware:` in the log means the TV reclaimed the decoder (another
app took it) — expect a frozen plane until the next reload; recovery policy is a known
open item.

## Triage

- Config failures: verify `native/config.local.json` exists and is readable; do not print it.
- TLS/CredSSP failures before `RDP_STATE_ACTIVE` should surface as `NetworkError`,
  `ProtocolError`, or a specific TLS/CredSSP diagnostic.
- H.264-capable sessions should use the NDL hardware path; if the server cannot provide H.264,
  verify native RemoteFX bitmap updates reach the SDL RGBA presenter.
- A Windows host can confirm `V10_7{SMALL_CACHE}` yet remain on Uncompressed/ClearCodec
  while frame acknowledgments are active. The native worker sends one
  `SUSPEND_FRAME_ACKNOWLEDGEMENT` response and then stops acknowledging frames, telling the
  server not to apply EGFX backpressure; on the validated host and continuous-motion
  workload this changes the session to H.264 after roughly 7-8 seconds. Once H.264 is live, Windows can
  continue sending RemoteFX/ClearCodec still-region refinements. The native shell holds the
  hardware plane for 1500 ms after each H.264 feed and drops those tiles instead of tearing
  the decoder down. With `native=debug`, `refinement tiles dropped while H.264 is live`
  confirms this path. At the default log level, expect one
  `switching graphics path from native RemoteFX RGBA to NDL/H.264` notice followed by the
  five-second `video: ... frames` cadence summaries, with no reverse switch.
- Camera redirection requires an exact native-H.264 V4L2 mode. `camera.v4l2` reports
  capture open/reopen failures, `camera.h264` reports decoder-seed/FIFO recovery, and
  `capture.redirect` reports the number of camera and microphone consumers.
  `webrdp.camera` should advertise the device only to the foreground opted-in session
  after RDPECAM version negotiation.
  Use `LGNOME_LOG='*=info,webrdp.camera=debug'` to record serialized RX/TX start/stop
  transitions, device state, active-stream count, pending sample credits, aggregate
  sample response/error counters, and protocol error codes. Per-sample success is not
  logged because it is a frame-rate hot path; the counters are included with each
  lifecycle transition instead.
  `audio.input.alsa` reports one shared capture endpoint, while `webrdp.audio_input`
  reports malformed negotiation. If the microphone cannot be opened, per-session silence
  is intentional best-effort behavior.
- The server's real graphics output size (from RDPGFX_RESET_GRAPHICS_PDU) can differ from the
  negotiated MCS/GCC desktop size. This is expected on webOS: the app's graphics/UI plane
  always renders on a virtual ~1920x1080 (or 1280x720 on HD-only models) logical canvas that
  the platform scales to the panel, while the separate hardware video decoder plane can output
  at the panel's true native resolution (up to 4K/8K) independently of the UI plane and of the
  negotiated RDP session size. `on_desktop_size` is re-invoked on every
  RDPGFX_RESET_GRAPHICS_PDU (not just the initial MCS/GCC handshake), so `App.desktop_width`/
  `desktop_height` and the pointer input mapping stay in sync with the real EGFX size for both
  the NDL/H.264 and RemoteFX RGBA paths; `on_video_au` reopens the video track whenever that
  size no longer matches what is currently open. This applies to any webOS target with a
  higher-than-FHD panel, not just one device.
- The SDL graphics layer presents one transparent frame to punch through to the NDL hardware
  video plane, then stops touching the window (`App.video_plane_punched`). Re-presenting every
  loop tick raced the video plane's own buffer swaps and produced visible flicker.
- The server's cursor shapes (RDP pointer updates) are rendered through the platform cursor
  plane (`SDL_CreateColorCursor`, see `cursor_sdl.c`) — never as an SDL overlay, which would
  need per-tick presents and re-introduce the flicker above. Check the log for
  `DEBUG cursor: server cursor WxH ...` on connect (enable it with
  `LGNOME_LOG='cursor=debug'`); `WARN cursor: color cursor
  unavailable: ...` means the webOS SDL port refused color cursors and the client stays on
  the default arrow. The mouse is read from grabbed `/dev/input` (evdev) and the cursor is
  driven by warping the OS pointer to the logical position, so server shapes ride the real
  pointer; visibility follows the server's pointer state alone.
- Video track loads (`NDL_DirectMediaLoad(video=1 ...)`) but `LoadCallback
  STATE_UPDATE_LOADCOMPLETED/PLAYING` never follow while `DEBUG video.ndl: fed N AUs` keeps
  growing → the server is streaming at a resolution the TV's hardware pipeline cannot
  start on. Observed live with a 2048x1152 virtual display (black screen, zero errors);
  standard video resolutions (1920x1080, 3840x2160) are confirmed good — keep the server
  display on one of those. The Display Control DVC prevents this by forcing the
  configured resolution at connect — check the log for `display: requesting server
  resolution ...`; its absence means the server never opened the MS-RDPEDISP channel
  (older gnome-remote-desktop versions do not), in which case the resolution must be
  fixed server-side.
- Decoder/render failures should surface as `DecoderError` and must not fall back to
  MSE, WebCodecs, RDCleanPath, or browser rendering.
- Input issues: run `ctest --test-dir /tmp/lgnome-native-build-tests -R input-sdl --output-on-failure`.

## Third-Party Provenance

Initialize pinned native dependencies:

```sh
git submodule update --init third_party/backend_ndl third_party/IronRDP third_party/lvgl third_party/miniaudio
```

See `third_party/PROVENANCE.md` for pinned commits, licenses, and the Moonlight reference boundary.

[grd-camera-media-format]: https://gitlab.gnome.org/GNOME/gnome-remote-desktop/-/blob/00195e889ad9390d4fa462d291a70fa92fe64678/src/grd-rdp-dvc-camera-device.c#L1388

### Connection and video diagnostics

At the default info level, each session reports its requested desktop size and local
PTS cadence, the TCP-only transport, negotiated TLS version/cipher, and the RDP
activation size/compression. The local FPS setting controls synthetic video timestamps;
it is not a request that changes the Windows capture-rate limit. The TLS summary also
states the current certificate-verification policy; it never prints certificates,
credentials, or TLS keys.

EGFX logs the advertised capability sets and the server-confirmed set separately.
ResetGraphics reports the actual graphics size. The first H.264 base-view AU and first
bitmap rectangle after each reset identify the observed payload path. An H.264 base
view alone does not establish AVC420 negotiation: AVC444 can also carry that view, and
its dropped auxiliary data is reported separately. Surface mappings are debug-level.
Decoder-open records include slot, connection epoch, dimensions, AU framing, and
SPS/PPS/IDR presence. No compressed payload bytes are logged.

The tested Windows host needs V10 capabilities for H.264; AVC_THIN_CLIENT requests
its YUV420 view. The NDL decoder presents every submitted AU, so the client drops
AVC444 auxiliary chroma rather than displaying it as an image. H.264 callbacks feed
the hardware plane directly and leave IronRDP's unused compositor disabled.
SolidFill, SurfaceToSurface and CacheToSurface are currently ignored: they are not
needed to present complete H.264 frames, but may leave stale pixels in bitmap-only
sessions. Supporting those operations requires updates to the RGBA canvas.

The periodic video window belongs to the shared decoder and can span a slot switch;
`last_slot` identifies the callback ending the window. It counts access units accepted
by the decoder, not displayed
frames or server frame boundaries. AU/s and compressed-input bitrate cannot establish
screen FPS or end-to-end latency. Static desktops may produce few AUs without any
performance problem. Compare the same moving scene when changing modes.

### Mouse and keyboard event ordering

Grabbed evdev mouse and keyboard events share a timestamp-ordered queue. The
reader uses a common monotonic clock and publishes only events older than the
start of a completed device sweep under one lock. Newer events wait for the
next sweep, including when libevdev has buffered them without fd readiness.
The SDL thread drains contiguous mouse/keyboard runs without moving wheel or button
edges across modifier presses/releases. Kernel timestamps preserve ordering
when separate USB device nodes are read in a different order. Motion coalescing
happens in the mouse drain and stops at keyboard boundaries.

Queue overflow logs a rate-limited warning and requests release of held remote
keys/buttons, discarding further input until that reset is consumed. Replaying
a partial sweep could restore an old press after losing its release. A failed EVIOCGRAB excludes that device from raw input.
Mixer handoff samples physical mouse-button state after ungrab, so releases
already consumed by evdev cannot strand the overlay's held-button mask.
Host `input-event-queue` tests cover Ctrl/wheel ordering, reversed device reads,
motion boundaries, equal timestamps, ring wrap, sweep cutoffs and overflow recovery. Actual
USB capture and focus/HUB transitions still require the full webOS build and a
TV smoke test. `input-sdl` also verifies Ctrl+Alt+Shift with clicks, dragging
and wheel events through the C RDP send boundary: all left/right modifier
combinations, all press/release orders and both device-read orders.

Cursor visibility uses the webOS platform API because SDL_ShowCursor(SDL_DISABLE)
also stops pointer-event delivery on the tested firmware. Raw evdev activity bypasses
the compositor, so input takeover and overlay return restore the server's cursor
artwork before its visibility. A server-requested hide remains in effect. Cursor
resizing uses premultiplied-alpha area sampling to avoid dark fringes; capped hotspots
are derived from original geometry to avoid accumulated rounding or saturation.
