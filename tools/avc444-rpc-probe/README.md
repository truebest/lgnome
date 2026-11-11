# AVC444 hardware output investigation

This experiment targets the ARMv7 application ABI on the tested webOS firmware
(AArch64 kernel), GStreamer 1.18.5 and its OMX plugin. It is not a portable
replacement for the NDL backend. Firmware libraries,
recorded desktops, settings and generated binaries are not distributed here.

The experimental lgnome branch now decodes every main/auxiliary AVC444 picture
with one hardware OMX decoder and releases auxiliary output before the video
sink. The visible result is the primary 4:2:0 image. It does not reconstruct
4:4:4 chroma.

## Hardware output path

On this firmware, `app-type=DIRECTMEDIA` on `omxh264dec` selects `rvdec` and
`rvsink2`, whose decoded video bypasses ordinary buffer pad probes. SVC output
flags alone do not suppress auxiliary pictures on this path.

`app-type=RTC` retains `omxh264dec1` and the DVO video sink and enables low-delay
decoding. NDL acquires and assigns the decoder resource. Decoded picture buffers
reach the GStreamer output pad, where auxiliary pictures are released.

The optional GStreamer module in `third_party/backend_ndl` selects this route.
The application passes picture identity as generic feed flags; bytes carry no
inserted SVC prefixes. The library copies metadata into firmware buffers, keeps
it across copies, assigns unique decoder PTS tokens and restores original timing
on matched output. It also owns bounded input queueing and latched errors.
See the library README for profiles, statistics callbacks and lifecycle rules.

The five-second `decoded pictures` counters count returned pictures and main
buffers sent to the sink, not physical presentations. Queue depth includes
pictures retained by the decoder. `ares-launch --close` can end the process
before the decoder object's finalizer logs totals; use the periodic counters.

## Diagnostic helpers

Build outside the source tree:

```sh
./tools/avc444-rpc-probe/build.sh /external/avc444-rpc-build
```

The SDK defaults to `$HOME/.local/opt/arm-webos-linux-gnueabi_sdk-buildroot` and
can be selected with `WEBOS_SDK`. The build includes:

- The former `test-avc444-omx` mechanics now run as backend_ndl's
  `backend-ndl-gstreamer` CTest with `BACKEND_NDL_WITH_GSTREAMER=ON`.
- `libavc444-rpc-trace.so` and `test-forwarding`: passive ARM interposition that
  preserves register/stack arguments, records bounded RPC input, and counts
  selected decoder/output calls. Trace output goes to `/tmp/lgnome-rpc-*` with
  mode 0600. It contains compressed video: use synthetic input and keep it private.
- `libavc444-omx-mode.so` and `test-omx-mode`: diagnostic property substitution
  for the standalone observer. The latter needs the former in `LD_PRELOAD`.
  The default selects `RTC`; `PROBE_OMX_NORMAL_DELAY=1` reproduces the initial
  empty app type for the latency comparison.
  `PROBE_OMX_APP_TYPE=RTC|MIRACAST|APPCAST|AIRPLAY` selects an explicit comparison mode
  and takes precedence over that legacy switch. Other values exit with status 2.
  This diagnostic overrides both sink bins; use it only in the H.264 observer,
  not as a general product preload.

Run the forwarding test with `LD_PRELOAD=./libavc444-rpc-trace.so` and its sibling
`librpc-probe-fake.so` available through the executable's `$ORIGIN` search path.
The library tests run without a preload. The production library exports
its scoped GStreamer hooks and does not package these diagnostic libraries.

## Recorded-input results

All tests below used the same authentic 3840x2160 GRD AVC444v2 recording with 73
AUs. The separately installed `avc444-ndl-observer` belongs to the probe worktree.

- Ordinary OMX decoded 72 pictures: 42 primary and 30 auxiliary. The final
  auxiliary picture remained pending at shutdown without EOS draining. Filtering
  delivered 42 primary descriptors to the DVO sink. Four display captures showed
  the synthetic desktop, with no packed auxiliary image in those samples.
- The input `DECODE_ONLY` flag did not suppress output on this decoder.
- Output buffers are 4096-byte DVO descriptors, despite their NV12 video metadata.
  They are not 12,441,600-byte linear 4K NV12 images.
- Stock `vbm-mmap`/`vbm-unmap` signals returned accessible addresses and strides.
  The captured bytes differed from PC decoding on over 99.6% of samples. Those
  addresses cannot be treated as linear NV12; tiling/compression or another
  layout issue has not been determined. GPU reconstruction from them is invalid.
- `output-mode=dmabuf`, `render-mode=GRAPHIC_MODE` reached DMA allocation, but
  `rtkdmaheapallocator` failed to allocate the group of eight buffers, followed
  by OMX `Insufficient resources (0x80001000)` and a vendor allocation assertion.
  This happened with an NDL-owned resource, not an invented decoder port.

The two pixel-access attempts did not produce usable linear planes. The working
DVO descriptor filter solves auxiliary-image flashes, not GPU reconstruction.
Do not infer full-color 4K/30 from caps, successful input calls, or sink counts.
The service's DISPLAY captures are 960x540 and cannot prove physical 4K scanout.

## Backpressure and parser cost

The experimental worker uses ordinary graphics frame acknowledgments. The
production baseline's one-time suspended acknowledgment let the GRD workload
accumulate updates before OMX. The video `appsrc` is also blocking with
`max-bytes=1`: GStreamer 1.18 lacks `max-buffers`, and this threshold permits one
nonempty AU in the queue, including an AU larger than the threshold. The next
push waits for space. The audio source is untouched. A TV-side test verifies
both the video properties and the unaffected audio source.

The H.264 scanners now skip nonzero payload bytes with the C library's `memchr`.
The parser's start-code checks and validation are unchanged. On this TV, scanning
24,784,165 bytes sixteen times took 1.833372 seconds before and 0.181323 seconds
after (216.3 versus 2187.0 MB/s). These are parser measurements, not decoder FPS.
`bench-h264 /path/to/synthetic-annexb.h264` reproduces this bounded scan benchmark.
Alignment/tail tests run on host and ARM; the host parser also passes ASan/UBSan.

## Throughput measurement limits

The short local recording was submitted at both 30 and 60 requested graphics
frames/s. With five seconds allowed for cold startup and outstanding input, both
runs decoded 72 pictures, released 30 auxiliary pictures and passed 42 primary
pictures to the sink. The descriptor-return spans were 1.558 and 1.548 seconds
(45.57 and 45.87 pictures/s between first and last output); median intervals in
the last 40 outputs were about 23.8-23.9 ms. This is a short saturated sample of
the OMX/DVO route, not a sustained 60-picture/s result or an isolated hardware
limit. The previous one-second completion wait stopped during startup and is
not a valid throughput result.

The initial live source spent too much time painting vertical one-pixel columns
and generated only about 28 updates/s. The synthetic generator now paints a
repeated one-pixel row, retaining the same stripes and fine colored text. A
32-bit grayscale barcode carries a fixed marker and source sequence number;
its draw timestamp is recorded on the source host. Captured sequence numbers
therefore distinguish current output from stale frames, including after the
source window closes. Captures bound latency but do not count every physical
presentation.

## Final live run, 2026-10-23

The installed same-ID lgnome package opened the authorized GNOME profile and
negotiated 3840x2160 AVC444v2. The final exercise used ten seconds of warmup and
120 seconds of continuous synthetic updates, with the accelerated source running
at approximately 40 updates/s. Within that measurement phase, the five-second
counters cover a 115.408-second interval:

| Measurement | Result |
| --- | ---: |
| Returned decoded pictures | 3,626 |
| Auxiliary pictures released before sink | 67 |
| Primary buffers sent to video sink | 3,559 |
| Primary rate over counter interval | 30.84/s |
| Pending pictures at sampled steady-state points | 2-3 |
| Maximum pending since opening, including startup | 21 |
| Process RSS during measurement | 73,104-75,576 KiB |
| Median process CPU, one core = 100% | 99.74% |

At every sampled counter point, `submitted = decoded + pending` and
`decoded = auxiliary_released + main_to_sink`. No identity/queue error occurred.
Five DISPLAY captures contained the primary scene and valid source barcodes,
with source-frame age bounded above by 576 ms including the capture request's
measurement uncertainty. Both captures taken one and three seconds after closing
the source showed the ordinary desktop. These few capture bounds are not
p50/p95/p99 latency statistics.

This run uses GRD's actual main-only and main-plus-auxiliary update mix. It does
not cover a second decoded picture on every update. It does not verify 30
physical presentations/s, native panel scanout resolution, or reconstructed
4:4:4. The confirmed useful result is hardware decoding of the complete AVC444
input and stable primary 4:2:0 output without sampled auxiliary-image flashes.

Validation passed: 108 Rust tests, all 87 native checks across their runs, the
ARMv7 cross-build/package checks, parser ASan/UBSan, and TV-side OMX/queue tests.
Host `/tmp` quota exhaustion required rerunning the two packaging fixtures with
an external `TMPDIR` and four filesystem fixtures in the existing reference
container. Those environment failures were not bypassed or treated as passes.
Windows codec selection and HEVC playback have not been revalidated in this
experimental branch after enabling normal graphics acknowledgments.

Verified package before the low-delay correction:

- ID/version: `com.truebest.lgnome.native` / `0.5.7`, title `lgnome AVC444 OMX test`.
- Binary SHA256: `0968d31926b401f38ab0e2ceefb11bbef13e1ecfdd586cc597dc1b75fcf5fce7`.
- IPK SHA256: `1ca44a6ba3dadaa4d264281ef3adc64fa3d8a1df0e70c83c1527c9d69deaa334`.

Raw recordings, source timestamps, display captures, per-core `/proc/stat`
samples, process memory/FD samples and logs remain outside the repository under
`$HOME/.local/state/lgnome/avc444-interleave-20261023/`. The final workload is in
`artifacts/live-omx/load-bounded-final/`; hardware experiments are in
`rpc-investigation/`. The separate probe worktree retains the observer and source
generator changes. No firmware libraries or saved profiles are packaged.

The final package also completed ten consecutive close/launch cycles. Every
launch reacquired the decoder with 94 process FDs and 42 threads; sampled RSS
ranged from 68,504 to 70,384 KiB. The boot ID and saved settings stayed identical.
The last launch loaded the persisted configuration and reconnected the selected
GNOME profile. Detailed cycle logs are in `artifacts/live-omx/final-cycles/`.

## Sparse-update latency correction, 2026-10-23

The reported lag on small text updates reproduced with a 3840x2160 recording
fed at one graphics frame/s: 13 AUs, including one auxiliary picture, followed
by five seconds with no input. Debug timestamps on the decoder bin's sink/src
pads matched each AU through its unique manifest PTS. In the initial empty
app-type mode, each steady-state picture emerged only after the next input.
The last primary picture never returned during the five-second pause.

| Mode | Returned pictures | Steady-state input-to-output latency |
| --- | ---: | ---: |
| Empty app type | 12/13 | 1,036-1,045 ms |
| Empty app type, trailing H.264 AUD | 12/13 | 1,036-1,044 ms |
| `RTC` app type | 13/13 | 28.1-30.3 ms; median 29.1 ms |

Steady-state latency excludes the first six AUs. These timings measure the
decoder-bin boundary, not keyboard-to-screen latency. The recorded SPS already
specifies zero reordered pictures. The AUD control did not fix the delay and is
not part of the application change.

The installed plugin's `RTC` mode selects its existing OMX low-delay setting.
It keeps the ordinary output pads and auxiliary filtering. Changing only the
internal decoder to DIRECTMEDIA also removed the hold, but `RTC` on the bins
achieves the result without another property interposer. No H.264 payload,
reference pictures, system libraries or private RPC fields are changed.

A complete 73-AU replay at 60 requested graphics frames/s returned all 73
pictures, passed 42 primary pictures and released all 31 auxiliary pictures.
The first-to-last output span was about 2.044 s (35.23 pictures/s), with a
29.0 ms median interval over the last 40 outputs. This short test confirms
complete output and filtering; it does not establish the maximum hardware rate
or sustained 60-picture/s decoding.

Artifacts are under `latency-investigation/run-baseline/`, `run-aud/`,
`run-rtc/`, and `run-full-60/` in the private experiment directory. The baseline
timestamps are in `stdout.log`; the old shared `/tmp` log was not writable in
that process. Other runs retain the selected current log as `observer.log`.
The separate observer source adds per-AU timestamps at debug level only.

Verified low-delay package (same ID/version):

- Binary SHA256: `3a9c48f6642ebbcc83bab5edaee6042bc71b9944615e758aed2936428fad9b8c`.
- IPK SHA256: `c0cec1d243191e025fac9bfd4c267594c0a32a2ea59747afab12ee156809afd4`.

The installed RTC build completed another 10-second warmup and 120-second live
run (`artifacts/live-omx/load-rtc-final/`). Its 116.530-second interior counter
span returned 3,599 pictures: 3,542 primary buffers to the sink (30.40/s) and 57
auxiliary pictures released. Sampled pending depth was 0-5, with a maximum of
10 since opening. RSS was 73,432-78,536 KiB and median CPU was 97.53% of one core.
All sampled counts reconciled; there were no identity errors. The normal
desktop returned after closing the source. This remains a primary 4:2:0 sink
measurement, with the same physical-presentation and full-chroma limits above.

The same-ID update preserved the currently loaded saved-settings file
byte-for-byte, verified against a private backup. Validation passed the 108
Rust tests, 83 native CTests plus the four `/tmp` fixtures in the reference
container, the ARM OMX/queue/property tests, syntax checks and the full webOS
cross-build/package verification. No television reboot was performed.
Three further close/launch cycles loaded the saved profile and reacquired RTC
decoding, with 93-94 FDs and 42 threads. An idle cycle produced no five-second
picture report, so that counter is not required as a startup check: the third
launch was independently checked after the runner restored it. The final app
remains connected to the selected profile. See `artifacts/live-omx/rtc-cycles/`.

## Decoder throughput comparison, 2026-10-23

During these comparisons the GNOME source ran at 3840x2160/60 Hz. The live session remained near
42-43 decoded pictures/s even with a small compressed bitrate. To distinguish
input, display and decoder costs, the standalone observer replayed identical
recorded AUs through the NDL-owned hardware decoder with QoS skips disabled.
Only one decoder owner ran at a time; lgnome resumed its saved profile after
each bounded replay. No firmware library, resource grant or clock was changed.

The 73-picture fixture contains 42 primary and 31 auxiliary pictures. Rates
below use returned, uniquely matched pictures between first and last output;
they do not count successful feed calls or physical presentations.

| Configuration | Returned pictures | Pictures/s | Last 40 intervals, median |
| --- | ---: | ---: | ---: |
| `RTC`, two runs | 73/73 each | 35.20, 35.31 | 28.86, 28.98 ms |
| `RTC`, release primary output too | 73/73 | 35.53 | 28.72 ms |
| `MIRACAST` | 73/73 | 30.81 | 28.97 ms |
| `APPCAST`, two runs | 73/73 each | 41.33, 41.36 | 23.86, 24.08 ms |
| `RTC`, six repeated copies | 438/438 | 35.24 | 28.93 ms |
| `APPCAST`, six repeated copies | 438/438 | 41.26 | 23.81 ms |

The primary-release control passes the first primary for sink preroll, then
releases every later decoded buffer. Its small difference from ordinary `RTC`
does not support display backpressure as the dominant limit in that workload.
The `MIRACAST` run had slower early output; its late intervals match `RTC`.
The repeated fixture preserves each AU byte-for-byte, starts every copy with
the original IDR, and renumbers manifest identities/frames/updates/offsets.
It contains 252 primary and 186 auxiliary pictures. At 30 requested graphics
frames/s, the returned-picture spans were 12.402 s (`RTC`) and 10.592 s
(`APPCAST`). Both returned every identity without decoding skips. This confirms
about 17% more throughput on this recorded workload, not 60-picture/s capacity.

An independent 180-picture, 4K60 synthetic input compared x264 CABAC, CAVLC,
and CAVLC with deblocking disabled. All used one reference and no B pictures.
On `RTC`, rates were 41.30, 42.09 and 42.09 pictures/s. CAVLC increased encoded
bytes from 10,695,087 to 12,147,773 (about 14%) for about 2% more throughput;
disabling deblocking added no measurable benefit. Encoding changes were not
applied to the GNOME server. These synthetic streams are single-view controls,
not authentic AVC444 workloads.

`APPCAST` also returned all 13 pictures in the one-update/s latency fixture,
including the final picture during the five-second idle pause. Excluding the
first six AUs, input-to-output latency was 36.4-42.3 ms, median 39.1 ms, versus
29.1 ms for `RTC`. The roughly 10 ms cost does not reintroduce the old one-frame
hold. The experimental application initially selected `APPCAST`; see the
live-session measurements below.

In a live 60 Hz GNOME session, a 160.255-second interior counter interval
returned 42.52 decoded pictures/s and sent 41.81 primary buffers/s to the sink.
The maximum pending count was 26, every sampled counter reconciled, and no
identity error was logged. A DISPLAY capture showed the ordinary primary
desktop. This live rate did not improve over `RTC`'s approximately 42-43/s;
the benefit is the heavier recorded main/auxiliary workload above. The tests
do not establish a universal H.264 hardware maximum or verify physical panel
FPS/resolution. Small compressed bitrate alone does not imply small decoding
work: every returned picture still has 3840x2160 pixels.

The same-ID update preserved the active saved-settings file byte-for-byte and
loaded it again on launch. Validation passed 108 Rust tests, 83 native CTests
plus four filesystem fixtures in the reference container, syntax checks,
ARM property/identity tests, and the complete webOS cross-build/package checks.
Private fixtures, timing logs and scripts are under `throughput-investigation/`
in the experiment state directory. The separate observer worktree contains the
primary-release control and its self-test. No television or host reboot was
performed during these comparisons.

Verified `APPCAST` package, same ID/version:

- Binary SHA256: `2f731799c520a460b18e529f86e1d8c025cf040a9881fa1319264c452f49ab20`.
- IPK SHA256: `6a1aa297c4acfafc2193c3c117b2e9432ffb8e22911adccc8ca82180d02d39b9`.

## Picture presets, game latency and HDR, 2026-10-23

The tested monitor identifies itself as `27SR73U-WA`, firmware `03.33.46`,
webOS `8.3.0`. LG lists the
[27SR73U-W family](https://www.lg.com/ca_en/monitors/smart-monitors/27sr73u-w/)
as 3840x2160, maximum 60 Hz, with HDR10 support. These display specifications
are not decoder throughput measurements.

While lgnome was active, the settings service reported `pictureMode=game`,
`dynamicRange=sdrALLM`, and `responseTime=faster`. The video-output service
reported `isGameMode=true`, `lowDelayMode=true`, `hdrType=none`, and
3840x2160 source/output rectangles. `faster` is the fastest enumerated response
setting; the other choices are `fast`, `normal`, and `off`. The TruMotion
description reported `memcType=none` with an inactive, hidden control.

Changing only the picture preset to `normal` left both video low-delay/game
flags true. A live comparison was unsuitable for throughput conclusions because
the desktop workload changed. The controlled comparison instead replayed the
same 73-AU AVC444 recording with `APPCAST` in each preset:

| Picture preset | Returned pictures | Pictures/s | Last 40 intervals, median |
| --- | ---: | ---: | ---: |
| Normal | 73/73 | 41.40 | 24.10 ms |
| Game | 73/73 | 41.31 | 24.19 ms |

Polling confirmed the requested preset while the diagnostic video had initialized,
and both runs retained `isGameMode=true` and `lowDelayMode=true`. A preliminary
fixed-time sampler caught startup/disconnected state, so its comparisons were
excluded. These short matched runs show no throughput improvement from the game
picture preset; they do not measure panel response or input-to-light latency.

The HDR check used the first 90 AUs of each existing 4K/30 HEVC Main10 control.
The HDR file contains PQ/BT.2020 pixels plus mastering-display/content-light
metadata; it was not merely an SDR file with changed labels. Both the SDR and
HDR files initially left the video-output service reporting `hdrType=none`.
Supplying an HDR elementary stream alone did not activate HDR in this NDL path.

The separate observer now accepts `PROBE_HDR10_FIXTURE=1` for HEVC replay only.
It submits the exact `make-controls.py` fixture metadata once, after PLAYING,
through `NDL_DirectVideoSetHDRInfo` using the already matched firmware value ABI.
The product backend's HDR helper currently accepts VP9 only, so this diagnostic
calls the exported NDL function directly; the product backend and its ABI remain
unchanged. The current ss4s
[NDL implementation](https://github.com/mariotaku/ss4s/blob/dfba721b85420ccabf91dac65be73984bf1865f9/modules/webos/ndl/webos5/ndl_video.c#L113)
also supplies HDR metadata explicitly without a VP9-only gate.

With that call, nine active-video samples confirmed `hdrType=HDR10`,
`dynamicRange=hdrALLM`, `pictureMode=hdrGame`, and matching PQ/BT.2020 metadata.
`lowDelayMode` and `isGameMode` remained true; `hfrEnabled` remained false.
Both HEVC controls used `omxh265dec -> rvdec -> DVO`, core `IP1`, video port 1,
and the same 60-FPS resource hint. All 90 AUs were submitted, but this DIRECTMEDIA
route bypasses individual decoded-buffer probes, so those counts establish
neither decoded nor displayed FPS. This confirms HDR activation through the
video-output/settings services, not improved H.264 capacity or calibrated HDR
pixel accuracy. The clock-debug summary exposed no decoder clock counters.

The diagnostic package passed its ARM self-test, invalid-option/H.264 rejection,
valid-HDR-fixture validation, cross-build, and exact package-content checks.
After all tests, lgnome reopened its saved profile in SDR/game mode. The complete
picture-settings snapshot and active saved-profile hash matched their originals;
the television did not reboot. At the end of these tests the product used the
verified APPCAST IPK above. Private scripts, snapshots and logs are in `picture-modes-investigation/`;
the accepted preset replays are `throughput-investigation/run-appcast-picture-*2/`.

## Desktop-session lifetime

Keep GNOME Shell running during decoder and RDP reconnection tests. Restarting
the shell terminates desktop applications and breaks portal connections. Changing
its `--virtual-monitor` startup argument therefore disrupts the desktop session.
