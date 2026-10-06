# A6L e-ink usability investigation — 2 October 2026

The user reports that r6g still has white side bars and unusably slow rear-screen
interaction in Fastest mode, although it may be faster than earlier builds.
Switching buttons and rear touch work. The initial usability candidate below
was installed in r6h; physical rear-screen validation is still pending. The
later stock-refresh candidate described near the end is prepared separately
and has **not** been installed or physically validated.

## What the r6g logs establish

Source: `firmware/extracted/rom-r6g-20261002/r6g-camera-eink-attended-20261002.tar.gz`.
Filtered service lines are retained in
`firmware/extracted/eink-usability-20261002/r6g-eink-timing.txt`.
Phone wall-clock timestamps are quoted only to correlate events; they are not
the laptop's local time.

- First mirror entry at 09:59:03.385 was still Auto. Capture took 481 ms and
  resizing/tiles 110 ms. Another capture took 468 + 98 ms before the first
  quality command. That command completed at 09:59:06.320: about 2.94 s after
  mirror activation. The subsequent Auto settle performed a GC16 refresh.
- The user selected Fastest at 09:59:37.686, before the second mirror entry at
  09:59:40.186. It correctly submitted Fastest for every subsequent changed
  frame; no Auto settle refresh appears in this sequence.
- The first Fastest frame cost a waveform transition: 39 drive frames generated
  in 167 ms, 872 ms physical drive, and 1.76 s socket round trip including
  conversion and initial bridge bring-up. It completed about 2.34 s after entry.
- Steady A2 updates used 10 drive frames, generally generated in 79–105 ms and
  physically displayed in 518–535 ms. Their socket round trips were about
  0.89–0.95 s. One generation outlier was 264 ms.
- Capture/resize before commands commonly added another roughly 0.6 s, and one
  recorded capture alone took 2276 ms. This is a significant software latency,
  independent of the A2 panel waveform time.
- All these drive operations succeeded. No drive-frame vblanks were missed.
  The bridge reported `bridge_ok=1`; no evidence here justifies changing the
  panel clock, waveform bytes or power-rail timing.

The prior removal of entry clears helped, but the serial full-resolution CPU
capture/conversion path and conservative drive overhead still prevent stock-like
interaction. Stock HWC used a producer/consumer frame pipeline; our service
currently captures, converts and drives sequentially. CPU frequency is also
still at bootloader settings. These facts do not establish that CPU frequency
alone causes the slow e-ink.

## First candidate, included in r6h

The following describes the first payload. Saved r6h boot logs confirm
`fit=stretch`, Fastest and the unchanged cleanup interval of 10 are installed.
Those logs do not include an attended mirror session; rear-screen performance
and side-bar removal on r6h remain unverified. The later refresh correction
below supersedes item 4's mode selection and manual-clear behavior.

1. **Fill the rear surface with complete content.** The installed init command
   now uses `--fit ${persist.vendor.eink.fit:-stretch}`. The new stretch geometry
   scales the entire 1080×2340 picture onto 720×1440, with the same geometry used
   by rear-touch mapping. Relative to the previous letterbox, the image is about
   8.3% wider; text remains complete, and no Android `wm size` change is involved.
   `letterbox` and `crop` remain explicit choices. The bare diagnostic binary
   retains its old letterbox default. This is a scaling solution, not validated
   automatic Android layout reflow.
2. **Bulk-copy scanout into cached CPU memory.** Capture previously ran the
   luma/blend loop directly against the mapped DMA scanout, issuing per-pixel
   byte reads. The new path copies the plane once with `memcpy`, then composes
   from normal allocated memory. It retains per-capture buffer mappings/handles
   and releases them after use. DMA read synchronization now checks errors,
   retries EINTR/EAGAIN, and completes before processing the private copy.
   Allocations/layouts are bounded by the existing 64 MiB capture limit.
   Uncached scanout reads are a plausible contributor, **not yet measured as
   the dominant cause**. Stage logs now separate sync, bulk copy and composition;
   slow overall captures are also logged beyond the initial three frames.
3. **Allow display work to run outside the background CPU set.** Both services
   use `ProcessCapacityNormal`, whose current AOSP definition selects the root
   CPU set, instead of `ServiceCapacityLow`/`system-background`. No clock,
   voltage, scheduler real-time priority or rail change is made.
4. **Use stock-style periodic ghost cleanup.** Regular policy cleanup now sends
   one `frame … clean`: a forced GC16 update of the new page. The previous
   sequence was white GC16 + INIT + requested page. Startup/manual full clears
   and recovery after a failed drive still retain the established full clear.
   This follows the stock force-clear path documented in
   `docs/eink-android-integration-20260923.md`; mode changes still carry the
   stock library's transition cost.
5. **Avoid the old 60-command minute cap at interactive cadence.** The default
   limit is now 120/minute. One in-flight update and the existing 150 ms minimum
   gap still bound submissions. This prevents a faster capture path from
   exhausting 60 commands early and pausing for the rest of the minute; no such
   cap exhaustion was observed in the short r6g trace itself.

DMA synchronization requirements are documented by the
[Linux DMA-buffer API](https://docs.kernel.org/driver-api/dma-buf.html).
The synchronization is required for coherent CPU access; it is not itself a
lock excluding another process from the buffer. This candidate does not claim
to solve GPU mapping faults or replace composer ownership of scanout.

## Validation and integration

- Existing full host suite: **18 passed, 0 failed** (policy, keys, rear-touch
  routing/loss, socket replies/retries, late flips, rail failure/recovery).
- Final policy/touch/plane logic: **179 checks, 0 failures**, including 90
  half-second Fastest submissions without the old mid-minute pause, periodic
  cleanup, whole-panel corner mapping and former-bar touch usability.
- Output fixtures: a black 1080×2340 page produces 1,036,800 output pixels.
  Letterbox has 79,200 white-bar pixels; stretch and crop have zero. Invalid
  `--fit` is rejected. Stretch geometry additionally passes landscape checks.
- Drive regression: `frame … clean` performs exactly one update, with force=1
  and mode=2, remembers the submitted pixels, and ends with rails off. Existing
  startup/manual/recovery clear cases still pass.
- Both final ARM64 binaries compile through NDK r27c/API34 with **zero warnings**,
  using the existing Android libdrm output. No Android source tree restaging,
  full-image rebuild, physical phone access or partition backup was performed.

Work directory: `/home/a6l/eink-work/usability-20261002/`.
Small payload: `firmware/extracted/eink-usability-20261002/vendor/`;
`manifest.json` and `SHA256SUMS` identify the exact binaries and init file.
Integrate through the changed e-ink sources/init file in the next bundled image,
or the verified isolated payload; do not mix the new mirror's `clean` command
with an older epdd that lacks it. This payload has not undergone a new physical
test or full-image/QEMU boot check.

## Physical acceptance tests still required

With hardware GPU rendering, record a fixed Settings page and select Fastest
**before** switching. Time entry and a series of taps/scrolls while capturing
the new stage logs. Check that all four panel corners are usable, side bars are
gone, and no Android size override was created. Compare first entry separately
from steady A2 updates and the every-tenth-page cleanup. Confirm ghosting,
readability, no lost touches, and continued LCD return after many updates.

If capture sync dominates, investigate fence waits; if copying dominates,
measure the mapped memory path; if composition dominates, profile plane
geometry/alpha and optimize that actual case. If capture is improved but
interaction remains too slow, the remaining serial capture/drive architecture
needs a latest-frame producer/consumer path or a SurfaceFlinger virtual display,
with physical timing and GPU stability evidence. **Stock parity remains open.**

## Stock refresh investigation and second candidate

Pierre reports that stock changes applications in roughly one second or less,
with a brief partial or inverted-looking flash. That is useful acceptance
evidence, but appearance alone cannot identify a waveform. Static inspection of
the extracted stock binaries and execution of the original TCON library give
the following firmer conclusions.

Stock Java `EpdManagerService.forceClear()` calls
`SurfaceControl.forceClearGhosting(1)`. In stock
`vendor/lib64/hw/hwcomposer.sdm660.so`,
`HWCDisplayExternalEpd::WaitForNextImage()` at `0x43568` reads the clear flag and
the currently selected display mode, then submits **force=1 with that mode**.
The TCON internally chooses a GC16 cleanup while retaining the selected mode's
history. Stock mode strings include `fast:6`, `reading:3`, and `picture:2`;
the ordinary HWC mode defaults to 8. Modes above 5 take the TCON's A2 path.

Our r6h periodic cleanup instead submitted force=1, mode=2. In Fastest this
unnecessarily changed the library's mode history away from A2, then paid for a
second transition on the next ordinary page. Our manual clear also still
performed the white/INIT sequence. Both are concrete differences from stock's
ghost-refresh path.

The second candidate changes only that behavior:

- Periodic cleanup submits `frame W H MODE force`, preserving the policy's
  selected mode while refreshing the new page. Legacy `frame W H clean`
  remains supported as force=1, mode=2 for older clients.
- The normal `vendor.eink.clear_req` request and long e-ink key press now submit
  `refresh`. The driver refreshes its remembered page using its remembered
  mode. This is a ghost refresh, not a white-screen reset.
- Explicit low-level `clear`/INIT, startup initialization, and drive-error
  recovery retain the full clear. A refresh without a remembered page retains
  the existing white-GC fallback.
- The default cleanup interval remains **10**. Rail timing, lead/tail idle
  vblanks, waveform data and temperature selection are unchanged.

### Stock-library execution results

The candidate ran with the extracted stock `libtcon_eink.so` and the actual
ED058TC7U2 waveform in diskless QEMU at 25 C. This validates generated drive
sequences and driver logic; it does not drive the physical panel. Two
720x1440 test pages alternate a black rectangle on a white background.

| Sequence | Cleanup frames | Following Fastest frames | Total |
| --- | ---: | ---: | ---: |
| New force=1, mode=8 | 39 | 10 | 49 |
| r6h force=1, mode=2 | 79 | 39 | 118 |

The exact short-run frame counts were startup INIT **99**, reset to white
**78**, initial Fastest transition **39**, steady A2 **10**, new cleanup **39**,
next A2 **10**, old cleanup **79**, next A2 **39**. Thus the new cleanup plus
next page saves 69 drive frames, about 0.81 seconds at 85 Hz before the
unchanged fixed overhead. This is a calculated sequence reduction, **not a
measured reduction in phone interaction latency**. Temperature and content can
change frame counts; saved r6h startup at 27 C used 87 INIT frames.

A separate 120-page sequence generated one 39-frame entry transition, then
119 consecutive 10-frame A2 updates. A manual mode-preserving refresh generated
39 frames, followed by another normal 10-frame A2 update. No unexpected
refresh appeared at update 101. The older
`docs/eink-clear-prep-20260923.md` claim that A2 automatically refreshes at 101
is therefore too broad. Inspection of stock `GetRefreshFlg()` at `0x2f14`
shows mixed-mode counter conditions, not that simple A2 trigger; the stock HWC
does not call `Set_OneMode_Refresh`. These tests establish the result for this
sequence, not every possible mode, temperature or image history.

The remaining architectural difference is real: stock uses a five-slot
producer/consumer ring to overlap waveform generation with display. Our
service generates a whole update before driving it and captures/converts the
next Android image serially. The r6g trace showed roughly 0.6 seconds of
capture/conversion before ordinary A2 commands taking roughly 0.9 seconds.
That can still make interaction feel slow even after cleanup is corrected.
Saved stock panel-open/close code also has its own reopen delay; the evidence
does not justify shortening our proven rail or idle-frame timings blindly.

### Tests and isolated integration payload

The full host suite passes **18/18**, including mode-preserving periodic and
manual refresh, remembered pixels/mode, rail shutdown, unknown flag rejection,
legacy clean compatibility, late flips and drive failure/recovery. Both ARM64
binaries compile with NDK r27c/API34 with **zero warnings**. The two stock-library
QEMU comparisons pass. No phone changes or new ROM build were performed.

The new isolated payload is
`firmware/extracted/eink-stock-refresh-20261002/vendor/`. Deliver the mirror and
driver together: `frame MODE force` requires the new driver. It contains:

| Binary | SHA-256 |
| --- | --- |
| `vendor/bin/hw/a6l_epdd` | `25e73745b0ff0e5a2fa5548e63d21af69807e13f3001df87dd2f1aa8c937c4b2` |
| `vendor/bin/a6l_eink_mirror` | `e6028f50499aba720c967663e710c5845136d951665207d468b0d66e3a658896` |

That folder's `manifest.json`, `SHA256SUMS`, compile logs, host-suite log,
`mode-preservation-results.json` and `long-sequence-results.json` record the
scope and assertions. Focused stock disassemblies are retained alongside them.
Full QEMU reports are in
`firmware/extracted/epdd-qemu-20261002-rstockrefresh2-20261002/` and
`firmware/extracted/epdd-qemu-20261002-rstockrefresh-long-20261002/`.
The private test harness used the already built vendor `libdrm.so` dependency;
the reports record its source/hash. This did not restage the Android tree.
Work is in `/home/a6l/eink-work/stock-refresh-20261002/`.

### Attended comparison before changing the default

First finish the pending r6h baseline: select Fastest before switching, use a
fixed Settings page, record first entry and later page changes separately,
check the side bars and all panel corners, then verify return to LCD. Retain
capture sync/copy/compose times, command latency and waveform frame counts.

When the second candidate is bundled, compare the same sequence with
`persist.sys.a6l.eink.clear_every=0` and `=10`, keeping mode, pages and lighting
constant. Zero disables the mirror policy's periodic cleanup, not startup,
manual refresh, recovery or every internal stock-library decision. Include at
least 20 page changes, record the every-tenth cleanup separately, and inspect
ghosting/readability before and after a manual ghost refresh. Restore 10 after
the comparison unless the results support a different default.

The separate appearance candidate may change the app theme after the switcher
polls screen state (currently every 500 ms). Time the first captured image and
the later theme redraw separately; avoid treating that additional redraw as
the panel's entire entry latency. The refresh candidate does not implement
per-screen wallpaper or theme preferences.

Acceptance still requires usable steady interaction, acceptable ghosting,
correct rear-touch mapping, no lost LCD return and repeated safe operation on
the real panel. Stock parity, actual latency gain and the best cleanup interval
remain physically unproven. If the measured bottleneck remains serial capture
or generation/drive overlap, profile that path before designing a bounded
latest-frame pipeline; do not accumulate stale frames behind the user's input.

## Accounting for steady A2 drive overhead

The r6g Fastest trace shows 10 generated waveform frames but **518-535 ms**
inside `drive()`, rather than the waveform-only estimate of 118 ms. Most of the
difference is already explained by the source; it is not a hidden 400 ms sleep.
`a6l_epdd.c` defaults to `lead=10`, `tail=20`, and the installed service does not
override them. Every update waits for 10 idle page flips, each waveform page
flip, and 20 more idle page flips. Each `flip()` submits a legacy DRM page flip
and synchronously waits for its completion event. The panel mode is
40.046 MHz / (641 x 735), approximately 85 Hz.

| Work inside the shown-in timer | Nominal time for steady A2 |
| --- | ---: |
| 10 waveform vblanks | 118 ms |
| 10 lead + 20 tail idle vblanks | 353 ms |
| All 40 page flips | 471 ms |
| Explicit VPOS/VNEG-to-VCOM wait | At least 20 ms |
| Measured total | 518-535 ms |

The remaining nominal difference of about 27-44 ms includes variable regulator
power-good wait, VCOM/I2C writes, CPU copies into the two dumb scanout buffers,
event handling/scheduling and initial vblank phase. It is not yet measured by
stage. The A6L-patched TPS65185 module polls the PG register in 5-6 ms steps up
to a 200 ms failure timeout; that timeout is **not a fixed successful-update
delay**. The panel driver explicitly waits 20 ms after VPOS/VNEG enable before
VCOM enable. Each update switches these rails off afterward. The trace reports
zero missed waveform vblanks for updates 11-16, with the total gap counter
unchanged, so there is no evidence of repeated frame dropping in those updates.
The extra idle frames carry the established strobe-safe pattern; this analysis
does not establish that they can be shortened safely.

Cold CRTC/bridge initialization is separate: `drive()` calls `drm_start()`
**before** starting its shown-in timer. A cold start runs panel prepare
(roughly 25 ms of reset waits), PHY reprogramming, bridge-table writes and
stream checks (roughly 206 ms of explicit waits for one successful attempt),
then 10 startup idle flips. The r6g update-10 trace has about **417 ms** between
generation completion and the bring-up log; its subsequent shown-in value is
872 ms. Updates 11-16 have no intervening cold bring-up. Service idle shutdown
defaults to 30 seconds; switching back to LCD explicitly requests CRTC off.
The one-second retry sleep occurs only after failed bridge bring-up, not on a
normal steady update.

There is also unaccounted command work **outside** the shown-in timer. For
update 12, command-to-reply is about 893 ms, generation is 82 ms, and drive is
518 ms, leaving roughly 293 ms. The command path expands the 1,036,800-byte
greyscale page, performs Floyd-Steinberg dithering and packs the library's RGBA
input before the generation timer begins. This is a plausible large part of
that difference; transport, temperature reads and scheduling also need timing.
The mirror's capture/conversion precedes the command again. Its 250 ms capture
interval and 150 ms minimum submission gap are policy bounds, not fixed sleeps
inside `drive()`.

Stock's extracted HWC uses separate generation/display threads and a five-slot
ring with condition-variable signaling. Its display path copies a generated
frame and pans the framebuffer. The inspected userspace path does not expose
our explicit 10+20 idle-flip loops; stock kernel or library behavior may still
provide settling. Its panel-open routine also waits in 40 ms steps when
reopening shortly after close, and the consumer can close after a three-second
empty-ring timeout. Neither stock path proves our electrical waits unnecessary.

### Bounded overlap option and decisive measurements

Capture and CPU conversion could architecturally overlap the current physical
drive because they use the front scanout and a private cached copy, while epdd
owns the rear lease and waveform state. The current mirror deliberately skips
capture while busy so its single event loop can service touch and replies.
Simply removing that guard would block those events again. A future capture
worker should publish only the latest completed private grey page and immutable
geometry/tile metadata into a bounded mailbox. The event loop would continue
to own touch, policy, socket commands and acknowledgement state, submitting
only after the current update succeeds. Replace stale unsent pages rather than
queueing every animation frame, discard obsolete generations on side/layout
changes, retain the last acknowledged baseline, and serialize all TCON mode
decisions/error recovery. Do not hold a DMA mapping or cache-access interval
through rear drive. A private copy prevents subsequent overwrite by our worker;
it does not itself prove a stable front scanout snapshot or resolve GPU buffer
lifetime/fence bugs. This remains a design option, not an implemented fix.

An attended warm/cold trace should timestamp command receipt/payload completion,
input expansion/dither/packing, temperature read, TCON generation, DRM start,
rails-on, lead flips, waveform copies/flips, tail flips, rails-off and reply.
Aggregate each flip's copy/submit/event-wait time and vblank sequences; avoid
per-frame logging that changes timing. Correlate those stages with the existing
mirror sync/copy/compose/resample logs and input/page identifiers. Compare warm
10-frame A2, forced cleanup and first update after CRTC-off separately. That
will distinguish the known approximately 353 ms idle-frame budget from variable
rail waits, input preparation, capture or cold initialization before selecting
the next optimization. The tested candidate binaries remain unchanged.

## Additional stock input evidence and isolated CPU candidate

Further inspection of stock `GetEpdTargetBuffers()` (`0x40aa4`) shows a path
accepting a **768x1440 RGBA target**, waiting on its acquire fence with
`sync_wait(..., 1000)`, mapping it through the buffer allocator, and copying
**4,423,680 bytes** into a private target or pending target. The 1000 ms argument
is a failure timeout, not a mandatory one-second wait. `NormalModeWaitImage()`
(`0x42ef4`) copies a pending target when present and transposes **720x1440 active
pixels**, stepping 768 RGBA pixels between input rows and 1440 pixels between
output rows. This is a composed rear-size target with padded input stride. Its
copy/transpose path has no equivalent to our full 1080x2340 front-plane luma
composition, resampling, and separate Floyd-Steinberg pass. This does not prove
the stock TCON lacks its own quantization/dithering.

The stock generator increments its ring index modulo five and waits when the
next index equals the consumer index: one slot is reserved, leaving four queued
slots. It calls `Update_Display_Image()` for one slot, publishes it, and signals
the consumer. The consumer copies the frame, returns ring space to the
producer, and updates the framebuffer's virtual-screen offset through ioctl
`0x4601` (`FBIOPUT_VSCREENINFO`). Exact downstream kernel pacing is not proved by
these userspace calls. Consequently the inspected stock userspace pipeline
supports overlap and replacement of pending image targets, but does not justify
removing our established 30 idle flips or physical rail waits.

A fenced SurfaceFlinger virtual rear target would avoid reading and resizing
the larger front scanout, and fits the separate rear-wallpaper rendering design.
It would require correct buffer/fence ownership and composer integration; the
existing 384x725 rear DRM connector encodes waveform drive frames and cannot
simply be presented to Android as the 720x1440 UI. A CPU capture mailbox is a
smaller alternative but still pays for larger front capture. Generation/drive
overlap alone could hide only the measured generation work (roughly 80-105 ms
on steady r6g A2), not its approximately 0.6-second capture/conversion or the
idle-flip budget. A large pipeline implementation is deferred until stage
measurements and stable physical operation support it.

One smaller candidate is retained separately from the frozen build inputs:
when Floyd-Steinberg quantization error is exactly zero, skip the four
neighbour updates by zero. Quantization order, values, orientation and output
bytes remain identical. This primarily helps white/black UI backgrounds and
does not disable dithering. A separate portrait tiling experiment was tested
but not promoted because the host gains with dithering were small.

The zero-error candidate passes **56 full-image comparisons** against the
original routine, checking all RGBA bytes and the entire mutated diffusion
buffer for both orientations, both rotations, dither on/off, and seven image
patterns including out-of-range intermediate greys. It passes ASan/UBSan,
the same 56 comparisons on emulated ARM64, the full **18/18** service host suite,
and NDK r27c/API34 compilation with **zero warnings**. The full-conversion WSL
x86-64 benchmark has median speedups of about **2.82x** on solid black/white or
binary checker pages, and **2.17x** on a synthetic mostly-white text-like page.
Gradient/random cases are approximately unchanged to 5-6% slower. These are
offline CPU results, **not measured phone speedups**; cache/CPU differences and
real page content can change them.

The isolated patch, candidate source/binary, original-routine comparison test,
benchmarks, sanitizer/ARM64 results, host suite and compile log are under
`firmware/extracted/eink-pack-20261002/`. `zero-error-diffusion.patch` is the
minimal change; `manifest.json` records its base/source/binary hashes. It is
**not installed, not applied to shared e-ink sources, and not in the currently
prepared image**. It leaves the mode-preserving refresh candidate, cadence,
waveform and all rail/idle timings unchanged. Measure full input-preparation
time on real text and photo pages before promoting it or claiming user-visible
improvement. The stock input/draw disassemblies are retained in the same folder.

## r6j measured software budget and next candidate

The saved laptop `rom-r6j/logs/live-r6j-firstboot-20261002/logcat-live.txt`
confirms `active=fastest`, `clear-every=10`, `fit=stretch`, and effective
`refresh mode fastest`. The rear interval at 22:40:28–22:40:34 contains updates
54–57, all ordinary mode 8, force 0, ten generated frames at 28 C. None of
these updates is a forced periodic cleanup. Warm physical drives take
524–537 ms. Capture reports 64–115 ms bulk DMA copy, essentially zero read-sync
wait, and 429–456 ms plane composition; resize/submit adds roughly 114–128 ms.
Warm epdd commands take 0.91–1.00 s, including about 270–295 ms preparation
before generation. First command takes 1.34 s with cold bridge initialization.
Two successive warm cycles are approximately 1.56 s. The user's observed
3–4 seconds per page therefore cannot be explained as a three-second A2
waveform in this trace; input/app presentation, policy polling and other
intervals remain relevant. The full-area result is physically confirmed;
stock-like usability is not achieved.

The existing opaque identity composition fast path excludes ARGB formats
unless blending is NONE. A full-screen ARGB plane with opaque contents can
therefore still use the expensive floating-point geometry/blend path. The
next source candidate extends only the unrotated 1:1, integer source offset,
full global-alpha case to alpha-bearing planes. Coverage uses
`(a*luma + (255-a)*background + 127)/255`; premultiplied uses
`luma + ((255-a)*background + 127)/255`, clamped to 255. Other geometry and
global alpha retain the original routine. Alpha 255 takes the opaque shortcut.
Layout changes are now logged to prove which path real pages can use.

The comparison preserves **1,536 complete image cases**, including clipping,
RGB/BGR, RGB565, pixel alpha, each blend mode and generic fallback transforms.
An additional **33,554,432** alpha/luma/background combinations exhaust both
blend equations against the original floating-point rounding. Host,
ASan/UBSan and emulated ARM64 all pass. WSL x86 full-frame examples improve
roughly 4.6–8.2 times; emulated ARM64 examples improve roughly 11.8–18.2 times.
Neither is a phone or end-to-end latency measurement. The real composition
layout was not recorded in r6j, so coverage of that specific slow page is still
an attended measurement, not a proved fact.

The previously isolated exact zero-error diffusion skip is now applied to
shared epdd source after authorization. A fresh comparison against the
original pack routine again passes all **56 full images** including mutated
diffusion buffers. Input preparation now reports dither and RGBA pack timing
separately. Waveforms, rail waits, 30 idle flips, temperature selection,
startup/error hard clear and default cleanup cadence remain unchanged.

The combined source passes **20/20 EPD host suites**, **11/11 dualux suites**,
**31 theme restoration checks**, **24 appearance gate checks**, **46/46 app
static checks**, standalone resource/javac/d8 app compilation, and NDK ARM64
builds of all three native binaries with zero warnings. Payload, source hashes,
equivalence tests and logs are in `firmware/extracted/eink-prelight-20261002/`.
This candidate has not been installed or physically measured. The phone was
left untouched during its overnight GPU stability test.

## Apply appearance before releasing the target light

The native daemon now prepares a screen change with both lights held off and
publishes `vendor.dualux.prepare="<monotonic-us-token> eink|lcd"`. The app
publishes the same atomic pair in `sys.a6l.dualux.appearance` before its theme
Binder call. After global configuration and its own resources agree, the app
publishes `sys.a6l.dualux.theme_ready`. Root's WMS integration acknowledges the
wallpaper transaction in `sys.a6l.dualux.wallpaper_ready` and its sequence-safe
window redraw/committed-buffer stage in `sys.a6l.dualux.frame_ready`. With the
build's `wallpaper_sync=1`, the app requires both matching WMS pairs before
publishing `sys.a6l.dualux.ready`. Global night mode and its schedule are retained.

For LCD return, that matching app readiness releases the LCD light. For rear
entry, it starts mirror capture while the frontlight remains off. After
observing readiness the mirror samples the LCD CRTC sequence and waits for
one advance before capture, polling at 20 ms and failing open with a warning
after 500 ms or if sequence reads are unavailable. This is a presentation
opportunity after the framework's committed redraw, not a fence-proof guarantee
that every app complied. The exact request is sampled before capture and
after conversion, then travels with the private queued frame. Only its
successful epdd ACK writes matching `vendor.eink.ready`, releasing the rear
frontlight. An old frame's ACK cannot satisfy a newer token. A static identical
page still gets a new correctly tagged update when needed for this gate.

The daemon remains responsive throughout; the preparation deadline is three
seconds, and the first rear-frame deadline is eight seconds. Timeout restores
normal operation with a warning. These are maximum failure bounds, not delays
inserted into every successful switch. Readiness is checked every 100 ms by
the app while a request exists. The 17 mirror tests cover stale preparation,
failed frame ACK, wrong target, sequence progress, new tokens, changed CRTC
and bounded fallback. Ten actual-daemon tests cover held lights/mirror,
stale-ready rejection, valid release, timeout and asleep-LCD blank preservation.

Physical follow-up must compare first visible theme in both directions,
wallpaper restoration, screenshot/page contents, layout/compose timings,
input-dither/pack times and warm versus cold updates. A rear screen visible
under ambient light still retains its old physical image until driven; holding
its frontlight off does not erase that retained image. The new handshake and
CPU optimizations are software-tested candidates, not a claim that stock
one-second app switching has been reached.

## r6k physical timing and r6l follow-up, 3 October

Saved laptop `rom-r6k/logs/live-r6k-firstboot-20261003/logcat-live.txt` confirms
that the earlier CPU optimizations are effective on the phone: composition is
roughly 52–63 ms, and input preparation roughly 50–80 ms, including 20–35 ms
dithering and 20–40 ms packing. The first rear entry at 07:53:15.106 has app
readiness, capture at 15.222–15.549, and command submission at 15.560. That
first update generates 39 frames in 421 ms, pays cold bring-up until 16.692,
then drives for 877 ms, completing at 17.570. Rear light release follows at
17.578. Another successful entry has app readiness at 07:57:27.655 and first
frame readiness at 29.694: 2.04 seconds after app readiness. It includes a
roughly 0.4-second cold start and a 530 ms ten-frame drive. Warm commands
remain around 0.86–0.95 seconds; approximately 200 ms between input preparation
and waveform generation is outside the old generator timer.

Most other switches, including every sampled LCD return, hit the native
three-second preparation fail-open. GPU/WM investigation identifies invalid
present fences as the cause rather than the rear waveform. That framework
fix belongs to the parent/GPU agent. The reported delayed wake after the saved
stream ends is not captured by these logs, so it is not attributed to this
three-second switch barrier. New native stage logs record physical key receipt,
target decision, synthetic wake dispatch start/end, observed Android awake,
and LCD/rear light release with monotonic timestamps. Epdd stage logs separate
temperature read, cold CRTC start, rails-on, lead flips, waveform, tail flips
and rails-off. The mirror logs scanout progression and tagged capture stages.

The r6l controller temporarily sets **all three global animation scales** to
zero for EPD and restores their exact previous strings on LCD, including unset
keys, `0`, `0.0`, and custom scales. It persists a complete recovery snapshot
before the first provider write. Repeated polls and controller recreation never
replace that snapshot with the applied zeros. Global settings persist across
reboot, so a later LCD startup recovers a session from a previous boot too.
Newer explicit external settings are preserved. The Android controller already
requests and allowlists WRITE_SECURE_SETTINGS.

Global scales alone do not cover the specified finger-driven gestures.
`Launcher3/0001-a6l-rear-instant-gestures.patch` uses PagedView's existing
immediate page-snap path, including its synchronous scroll/page completion;
its swipe-home spring branch delivers final geometry and normal start/update/
end lifecycle callbacks without starting the springs. Framework patch
`0003-a6l-rear-instant-qs.patch` makes TouchAnimator and the MotionLayout QS
header choose endpoints on EPD. The default LCD path remains unchanged.
The transient `sys.a6l.eink.no_animations` flag controls these paths.

The rear mirror also withholds **new** captures while a forwarded rear contact
is moving beyond Android's touch slop, while continuing to deliver the complete
gesture to Android. A stationary tap/long press is not held. Contact movement
remains a drag until release. Pending input is drained after CPU conversion;
a frame spanning a drag transition is discarded. This prevents home-page,
notification drawer and QS drag positions from becoming successive physical
panel updates. Existing in-flight waveforms complete safely; after release the
mirror captures the current completed UI. It does not freeze or disable touch.
This behavior still needs physical assessment, including long presses and
in-app fling behavior; app-authored animation mechanisms beyond the Android
scales and these system gesture paths are not universally intercepted.

The redundant fixed 200 ms app settling floor has been removed: configuration,
wallpaper and committed-redraw predicates remain the actual readiness barrier.
Mirror property detection is now 50 ms rather than 500 ms when theme-sync is
enabled. Three-second/eight-second failure bounds, waveform selection,
refresh cadence and all electrical/idle timing remain unchanged.

Validation: **78 animation recovery checks**, **31 theme checks**, **24 readiness
checks**, **24 actual mirror token/gesture checks**, standalone app resource/
javac/d8 compilation, NDK binaries with zero warnings, **20/20 EPD suites**,
and **11/11 dualux suites** pass. An actual mirror/FIFO integration test confirms
touch delivery, zero new frame submissions during a moving gesture, and a new
frame after release. Both launcher/SystemUI patches pass reverse-apply checks;
their complete module builds are coordinated by the parent, not claimed here.
The main source candidate was frozen for that build. Evidence is under
`firmware/extracted/eink-r6l-20261003/`. No physical phone operation was performed.

### Isolated TPS65185 temperature-completion candidate

The actual r6k boot image's 87,370-byte appended DTB has no `interrupts` or
`interrupts-extended` property in `pmic@68`. Power-good GPIO IRQ is a separate
signal; it cannot complete the temperature request. The retained driver only
registers a temperature-completion handler if `client->irq` is present, yet
every temperature read waits up to 200 ms for that completion before checking
the ADC register. This matches the approximately 200 ms untimed per-update gap.
New epdd instrumentation will directly confirm its duration.

The isolated candidate uses register polling only when that PMIC IRQ is absent.
It starts the same fresh acquisition, requires READ_THERM to self-clear and
CONV_END to indicate completion, then reads the same signed temperature value.
It polls at 2 ms with the original 200 ms bound; IRQ-equipped devices retain
the original completion path. Start/status/value I2C errors propagate. It does
not cache temperature or change any rail, voltage, waveform or power-good
sequence. These completion bits are defined in the
[TI TPS65185 datasheet](https://www.ti.com/lit/gpn/tps65185).

Only one external module was built in a uniquely named work/output directory,
using a small copy of retained prepared metadata. Shared kernel sources and
outputs remain untouched. Its vermagic matches the staged module exactly:
`7.2.3-a6l-probe+ SMP preempt mod_unload modversions aarch64`. All 36 candidate
symbol CRCs match retained kernel Module.symvers; all 35 shared baseline imports
match. The only added import is `ktime_get` for bounded polling. Twelve tests
compile the actual extracted driver routine with fake I2C/completion behavior,
including early/delayed completion, stuck READ_THERM, timeout, IRQ mode, errors
and signed temperature; ASan/UBSan passes. It has not been installed and the
real ADC duration/saving is not yet measured.

Candidate module SHA256:
`60d28b6cd233dc9edb90e08ed0849b005a115cc2764a9743fe85ab7c30d382c4`.
Its source patch, ABI manifest, small DT evidence and tests are retained in
`firmware/extracted/eink-r6l-20261003/tps65185-adc/`. Parent review has now
promoted this exact module into the r6l kernel inputs and repeated its ABI
checks. It is still not installed or physically timed.

The final native instrumentation revision adds `boot_ms` from CLOCK_BOOTTIME
and `mono_ms` from CLOCK_MONOTONIC to all three daemons' Android logcat lines.
BOOTTIME includes suspended time and aligns with Java elapsedRealtime; the
MONOTONIC clock continues to control every deadline and schedule unchanged.
Host stdout is unchanged. The two clocks are sampled separately, so their
sub-millisecond difference is not a presentation fence or timing guarantee.
Fifteen checks of the actual extracted log wrappers pass under ASan/UBSan,
including a simulated 45-second suspend offset, error fallback and preserved
log priorities. All three Android NDK builds pass without warnings. Their
updated source/binary hashes and commands are in
`firmware/extracted/eink-r6l-20261003/compile-results.json`; the reproducible
clock check and result are alongside it. Gesture and electrical behavior
remain frozen while the parent builds the bundled image.

## r6m measured latency and r6n software candidates (3 Oct)

The r6m attended archive is retained at
`firmware/extracted/rom-r6m-20261003/r6m-user-camera-sleep-20261003.tar.gz`.
The extracted stage lines, summary, per-update records and stock disassembly
are under `firmware/extracted/eink-r6m-performance-20261003/`. Native `boot_ms`
aligns with Java elapsedRealtime; deadlines still use MONOTONIC.

This trace actually used **Auto**, not a verified Fastest selection. Both the
startup property snapshot and the native mode-change line say `auto`; no later
mode change appears. The log's `active=fastest` means the waveform used during
Auto motion, not the selected policy. Source review found no refresh-mode write
in PerScreenMemory: its per-screen restoration covers brightness and timeout.
Settings/provider writes and native readback must be checked in the next
attended comparison before interpreting a Fastest test.

### What the phone actually spends time doing

There are 91 updates, including 79 warm A2 updates of ten generated frames.
The following are medians of those 79 updates, not estimates from a different
device. Summing individual medians is only an approximate budget.

| Stage | Median | Meaning |
| --- | ---: | --- |
| Fresh temperature read | 3 ms | 91 samples, range 2–13 ms |
| Generate the A2 drive images | 97 ms | Range 83–114 ms |
| Rails on | 61 ms | Electrical sequence retained |
| Leading idle scans | 113 ms | Ten scans retained |
| Actual A2 waveform | 118 ms | Ten drive frames |
| Trailing idle scans | 236 ms | Twenty scans retained |
| Rails off | 0 ms rounded | Range 0–2 ms |

The IRQ-less TPS65185 completion fix is therefore physically verified in r6m:
temperature acquisition no longer consumes the former approximately 200 ms.
This supersedes the earlier uninstalled/unmeasured status of that candidate.

Successive warm drive completions are separated by a median **1,120 ms**
(77 intervals below two seconds; range 1,073–1,746 ms). The non-waveform portion
of the physical drive is approximately 410 ms: rails on plus ten leading and
twenty trailing idle scans. The warm trace has no cold bridge bring-up in these
updates. Generation and front-screen preparation account for much of the
remaining whole-update time.

The capture log samples show median copy 154 ms and composition 113 ms, with
three planes. These 82 samples are partly threshold-selected logging and are
not an unbiased capture-time distribution. The actual saved layout contains a
full 1080×2340 opaque ABGR plane, sometimes another full opaque plane above it,
and a small navigation plane. Before the second full plane appears, copy and
composition are approximately 74+54 ms; afterward approximately 154+113 ms.
The hidden lower plane still incurs a complete CPU copy and composition.
Resize adds approximately 100 ms in the initially logged frames. Input dither
and packing have median total 91.5 ms across 86 samples. These are serial
stages in the current pipeline.

The two recorded screen switches each hit the unchanged three-second appearance
fail-open. The rear request's app logs never progress beyond `theme=false`,
even after wallpaper readiness. The old readiness test queried the service
process's ValueAnimator cache, which can remain at its initial value when the
headless service has no WindowManager session. The r6n controller instead reads
the authoritative WindowManager animation scales, while preserving the exact
theme/configuration and wallpaper/frame pair checks. New readiness reasons
will distinguish `animation_wm`, `global_local_config` and `requested_config`
on the phone. This is a tested candidate, not proof that the cache explains
every switch timeout. LCD frame readiness remains a separate framework issue.

### Frozen, isolated mirror candidates

The candidate and manifest are in
`firmware/extracted/eink-r6m-performance-20261003/occlusion-candidate/`.
Canonical vendor source was left unchanged for parent review. Apply
`opaque-fullscreen.patch` then `pixel-damage.patch`; both were checked and
applied in an isolated baseline copy and reproduce the tested three source
files byte for byte. A subsequent instrumentation-only `damage-timing.patch`
adds measured comparison/snapshot-copy time to each submission log. Its exact
removal restores the fully tested behavior source, and its ARM64 build has zero
warnings. The manifest distinguishes these hashes and the patch order.

1. **Skip completely hidden planes.** A higher plane must have exact full-output
   coverage, zero source/destination origin, unscaled source bounds, ROT_0,
   full global alpha and explicit NONE pixel blending. Its actual framebuffer
   must have valid bounds, a recognized readable format, adequate pitch and a
   linear modifier. Unknown/premultiplied/coverage blending, alpha, crop,
   rotation, holes or invalid metadata keeps the original path. Metadata-only
   GETFB2 handles are closed. Visible planes retain their existing validation,
   private copy and DMA cache-sync path. For the saved duplicate full-plane
   layout, the stage comparison suggests **120–135 ms** saved, approximately
   11–12% of the measured 1.12-second cycle. Single-full-plane scenes receive
   no such saving. This estimate has not been measured on the phone.
2. **Detect changed pixels hidden by tile averages.** Eight-by-eight mean tiles
   can alias relocated text strokes, or round a one-level grey change to the
   same mean. The latter can cross an actual 16-level quantization boundary;
   Floyd–Steinberg can propagate a small input change. If coarse damage is zero,
   compare the private pre-dither image exactly. A changed image receives the
   smallest nonzero damage fraction, preserving the existing policy, rate,
   gesture and cleanup gates. This can conservatively submit an image whose
   final quantization would be unchanged. The previous capture snapshot changes
   after capture; the panel snapshot changes **only after a successful frame
   ACK**, from the immutable queued frame. Failed ACK/clear/disconnect continues
   to invalidate the panel baseline. This prevents a proven stale-text failure
   mode, but does not establish the cause of every reported physical ghost.

The pixel fallback adds two 1,036,800-byte grey snapshots (2,073,600 bytes total).
Its worst-case host comparison plus copy measured approximately 0.031 ms per
iteration; ARM64 QEMU measured 0.608 ms. Neither is a phone timing claim. The
next phone trace should measure the capture/policy budget with this enabled.
Both candidates keep waveform history, voltages, rails, scan counts and cleanup
cadence unchanged. Each submitted frame now logs policy, effective mode,
fixed-policy state, exact damage fractions, capture/resize/damage time and gesture
generation, allowing its command and ACK to be correlated with drive stages.
The damage timer includes both image comparisons and the previous-image copy;
the ACK-only panel-image copy occurs after drive completion, outside that timer.

Validation: 1,024 whole-image equivalence cases, 17 conservative geometry
rejections, 20 actual DRM metadata/handle cases, 164 actual-source pixel,
quantizer and ACK checks under host/ASan/UBSan/ARM64 QEMU, and two complete
capture→resize→policy→socket→ACK scenarios. The latter each send exactly two
correct pages for an equal-mean glyph move and quantization-boundary change.
All 20 existing host suites pass, including lost replies, disconnects, rail
failure recovery, gesture input and appearance tagging. The isolated Android
API34 ARM64 NDK build has zero warnings. The authoritative animation readiness
candidate passes 31 actual-source checks and an isolated resource/Java/dex build.
Physical latency, visual ghosting, animation suppression and switch readiness
remain to be validated on the next installed image.

### Stock architecture and practical refresh ceiling

Additional stock kernel disassembly connects the HWC burst lifecycle to panel
power. EpdPanelOpen/GetFramebuffer holds the rear framebuffer fd while the
consumer drains a burst; its condition wait closes the panel after approximately
three seconds of inactivity. The kernel's first mdss_fb_open unblanks the
display, mdss_dsi_on calls tps65185_active_mode, and final framebuffer release
blanks it with FB_BLANK_POWERDOWN; mdss_dsi_panel_power_ctrl calls
tps65185_sleep_mode. Ordinary pan/update paths did not show per-image PMIC
active/sleep calls. This supports **power retained during a stock burst**,
rather than merely an inference from HWC's open fd. It does not prove that the
current mainline bridge can safely omit any particular idle scan.

Stock also accepts an already composed rear-sized 768×1440 RGBA image with an
acquire fence, instead of reconstructing a 1080×2340 front screen on the CPU.
It prepares drive images in a five-slot producer/consumer ring with four usable
queued slots, so generation and panel scanout overlap. The stock generator also
publishes the terminating library-return-zero frame: the current ten-frame A2
sequence must not be shortened to nine merely because ModeDecision reports nine.
The library tracks old/new pixel state and generates differential pulses;
small Android damage does not by itself authorize cropping the electrical drive
or discarding cleanup pulses.

At the observed 85 Hz electrical scan rate, the current stock-library waveform
length imposes these ideal whole-image batch ceilings before CPU/idle overhead:

| Observed waveform | Frames | Nominal duration | Ideal completed batches/s |
| --- | ---: | ---: | ---: |
| Warm A2 | 10 | 117.6 ms | 8.5 |
| Quality/REGAL or mode-preserving cleanup | 39 | 458.8 ms | 2.18 |
| Observed mode transition | 79 | 929.4 ms | 1.08 |
| Observed startup | 87 | 1,023.5 ms | 0.98 |

The unchanged ten leading plus twenty trailing scans bring warm A2 to forty
scans, approximately 470.6 ms; adding the measured rail-on stage limits that
physical path to approximately 1.88 batches/s before capture and generation.
**85 Hz scanout is not 85 completed Android images per second.** A 15/30/60 fps
whole-image target gives 66.7/33.3/16.7 ms per image, shorter than this ten-frame
waveform even with all software work removed. Such rates require different
per-pixel scheduling/overlapping transitions or validated waveform behavior;
they cannot be promised by changing a refresh-rate property.

### Research and the next measured improvements

The [E Ink kit manual](https://shopkits.eink.com/en/download/0223013011w2578422/User%20Manual%20-%206%27%27ePaper%20Display%20Kits%20%28ED060KC1%29.pdf)
describes panel/lot-matched waveforms and distinct initialization, direct-update
and quality modes. It is not the A6L panel's waveform specification.
A [2020 electrophoretic waveform study](https://pmc.ncbi.nlm.nih.gov/articles/PMC7281290/)
uses measured particle activation/reference phases to reduce delay; those
experimental timings are not a replacement for this phone's LUT. A
[2017 Displays paper](https://www.sciencedirect.com/science/article/pii/S0141938216301524)
investigates video above ten fps through shorter transition paths and image
compensation, demonstrating that waveform and optical history matter.
The [Modos controller source](https://github.com/Modos-Labs/Glider) uses per-pixel
old/new state and frame counters, with early cancellation and fast-binary/later-
grayscale scheduling. Its high input/scan rates are a different architecture;
they do not mean every particle completes a full transition in one 60 Hz frame.

The largest stock-parity paths are a **fenced rear-sized composed image** and
bounded producer/consumer overlap. A latest-frame mailbox could overlap front
capture/resize with the current rear drive while keeping only one active and
one replaceable pending image. It must not overwrite qframe before ACK, mutate
the TCON handle concurrently, or generate history against a failed physical
update. Merely removing the current `busy` capture guard would again block
socket/input handling and would not establish a stable source image.
[Kernel DMA-BUF documentation](https://docs.kernel.org/driver-api/dma-buf.html)
explicitly distinguishes cache synchronization from exclusion of GPU/process
access: the existing READ_SYNC/private copy is not a producer ownership fence.
Proper acquire-fenced composition is needed to exclude mixed/intermediate frames.

No burst rail-retention candidate is enabled. An attended, default-off future
comparison should preserve every library pulse and electrical shutdown/recovery
sequence, measure idle/power/temperature behavior, and independently decide
whether leading/trailing scans are required only at burst boundaries. Today
they remain untouched. The next controlled tests should first verify the
effective selected mode, new appearance readiness reasons and per-submission
stage totals, then compare the same static page/clock and settled gesture in
Auto versus verified Fastest. Visual response and ghosting must be assessed
alongside completed ACK intervals rather than inferred from input frame rate.

An eventual burst-retention experiment should be separated into two steps:

1. First retain power only across a bounded burst, **keeping all ten leading
   and twenty trailing idle scans per update**. This isolates the approximately
   61 ms rail-on cost. Use an explicitly default-off option and a maximum
   three-second idle deadline, with a wakelock until shutdown, neutral idle
   scanout between complete waveforms, and the original rail-off/retry sequence
   on inactivity, LCD return, disconnect, termination or any failure. Preserve
   ACK/history and full recovery clear after partial/error drive. Test the
   state machine with fake clocks/rail errors before an attended short run.
   Record power state and rail-on duration, scan counts, ADC temperature,
   complete ACK interval, ghosting and shutdown/recovery. The maximum expected
   warm saving from this first experiment alone is about 61 ms, not 410 ms.
2. Only if that comparison and optical behavior pass, separately assess whether
   a **burst-boundary** leading/trailing sequence matches the stock driver and
   this mainline bridge. Keep the library drive frames unchanged. The retained
   stock disassembly supports a burst lifecycle, but does not establish the
   required mainline neutral-scan minimum or charge balance. Those counts
   therefore cannot be changed based on latency arithmetic alone.

Neither experiment nor a retention option is included in r6n. The current
daemon already retains the bridge/CRTC across warm updates until its configured
idle-off deadline; the proposed first experiment concerns PMIC rail ownership,
not eliminating the measured cold bridge bring-up on every warm update (there
is none in this trace).

## r6n feedback, measured switch delays and next options (3 Oct)

The user reports better rear-screen response, but still insufficient smoothness
and approximately three seconds or longer to switch screens. Waking now works;
USB disappearing is not evidence that the phone froze. Preserved live logs are
`firmware/extracted/rom-r6n-20261003/feedback-live-logs/`. A reproducible read-only
parser and results are in `firmware/extracted/eink-r6n-research-20261003/`.
No r6n payload, property, phone state, rail or waveform was changed for this study.

### What the new measurements establish

All six appearance requests reach `theme=true reason=ready`: the authoritative
animation-scale readiness correction works in this run. Four requests still
wait for the three-second native fallback. Each has theme and wallpaper ready
but frame false, and WindowManager explicitly reports `no-present-fence` with
`latch_ns=-1`. In these cases the transaction completes 31–71 ms after observation,
while ViewRootImpl's explicit invalidation is logged later. This points to a
redraw synchronization race, rather than a three-second panel waveform. The
parent/GPU agent owns the framework proof correction; merely shortening the
timeout would not prove the right theme was drawn.

Even the first successful rear switch takes **3,379 ms**, with no readiness
timeout. Its native preparation starts at boot 975549.704 ms and first-frame
light release is at 978928.432 ms:

| Phase | Measured elapsed time | What it does |
| --- | ---: | --- |
| Detect request, apply appearance, prove redraw | 1,236 ms | Native accepts the exact readiness pair |
| Scanout advance and two capture/resize passes | 585 ms | First captured page is not submitted by Auto policy |
| First submitted page through acknowledged rear drive | 1,558 ms | Includes input, generation, cold bridge and complete waveform |
| Total | 3,379 ms | Native first-frame-ready/light release boundary |

The first driver phase includes input 56 ms, temperature 4 ms, generation
184 ms, cold bridge 417 ms and physical drive 872 ms. This first A2 command uses
39 generated frames, not the warm ten-frame path. The latter is a mode/history
transition managed by the stock library; its pulses cannot simply be dropped.
The first app starts processing approximately 473 ms after the native request,
matching the existing 500 ms idle poll. The first successfully proven capture
is then discarded by the normal motion policy before a second page is submitted.

Forty warm ten-frame A2 updates show median generation 87.5 ms, rail-on 59 ms,
leading scans 113 ms, waveform 118 ms and trailing scans 236 ms. Twenty-five
consecutive warm completion intervals have median **956 ms** (range 908–1,332).
This is better than the previous 1,120 ms median, but scenes, temperature and
layout differ, so it is not a controlled estimate of one patch's saving.
Across 87 submissions, capture is median 134 ms, resize 101 ms and exact-damage
work 0.748 ms (maximum 3.08 ms). Input preparation is median 84 ms among 72
logged samples. The added damage snapshots therefore have small measured CPU
cost; this trace contains zero coarse-tile alias detections, so it does not
physically demonstrate that the reported clock ghosting was such an alias.

Only five capture-stage lines were emitted; all say `culled=0`. Later layouts
do include an opaque second full-screen plane, so they exercise the eligibility
conditions, but the threshold-selected stage samples cannot isolate actual
culling savings. Capture is now approximately the cost of one full plane; a
controlled identical-scene comparison with per-submission cull count is needed.
Every recorded refresh selection is still **Auto** (`fixed=0`), with 51 Fastest
motion submissions and 36 quality/cleanup decisions. A selected Fastest test
must first show a native `refresh mode fastest` and `fixed=1` readback.

### Four separate changes to judge first

These are proposals, not changes silently applied to r6n. Each can be tested
and accepted independently.

1. **Correct the proof that the new appearance was drawn.** In plain language,
   make Android wait for the redraw it requested, instead of accidentally
   checking an empty transaction and falling back after three seconds. This
   targets the four observed failed switches. It could avoid roughly two
   seconds of unnecessary waiting in those cases; the first-image/panel time
   remains. Preserve exact request pairing, current configuration, fresh buffer
   sequence and real fence completion. Test no-damage, delayed redraw, rapid
   switches, stale callbacks, lockscreen and app/SystemUI participants. There
   should be no image-quality compromise or old-theme flash.
2. **Notice switch requests faster.** Poll only the small prepare property at
   100 ms while awake, retaining 500 ms brightness/timeout bookkeeping and
   normal sleeping behavior. In plain language, the controller should react
   promptly rather than waiting half a second to notice the button. This can
   remove up to 400 ms of request-detection delay, approximately 200 ms on
   average for randomly timed requests; the recorded first request waited
   473 ms. Cost: more cheap awake property checks. Measure main-thread/CPU cost,
   cancellation and sleep/restart behavior. Keep all readiness proof checks.
3. **Submit the first eligible rear image immediately.** Once the existing
   theme/frame/scanout checks pass, give that initial image priority over the
   ordinary Auto motion-settling rule. In plain language, do not take a correct
   first picture, throw it away, and take another before lighting the screen.
   The observed avoided pass is approximately 230 ms. Select the waveform
   according to the user's policy (quality/partial startup remains quality),
   preserve rate/backoff/ACK/gesture guards, and retain normal subsequent
   motion/cleanup behavior. Test exactly one initial submission, rapid target
   changes, active gestures, errors and a static page with the same pixels but
   a new appearance request. The readiness proof, not a fixed delay, decides
   eligibility; do not submit an arbitrary intermediate animation frame.
4. **Prepare the cold bridge while Android prepares the theme.** Run the same
   already validated bridge bring-up against the retained neutral idle frame,
   with rails still off, concurrently with the earlier appearance stage. In
   plain language, prepare the rear screen's connection while Android prepares
   its picture, instead of doing both jobs in sequence. The observed cold
   bridge cost is 417–422 ms and could be hidden when the appearance stage is
   longer. This changes scheduling, not voltage or waveform scan counts.
   It needs a default-off command, valid neutral-template prerequisites,
   cancellation/idle cleanup, lease-loss and modeset-error tests, followed by
   an attended comparison. It cannot run during sleep, leave an unwanted
   bridge enabled, or change the startup/error recovery clear. Some additional
   bridge activity happens earlier; there is no promised optical-quality gain.

The savings above cannot simply be added: phases may overlap, different
requests take different paths, and the long first waveform remains. They
target measured waste while preserving the panel's current driving sequence.

### Later software work and tradeoffs

**Prepare only the newest complete page while the current page refreshes.** A
private, bounded latest-frame mailbox can overlap capture and resize with rear
drive. Keep one immutable active frame and one replaceable pending frame;
discard obsolete gesture/appearance generations and commit optical history
only after ACK. This deliberately skips obsolete intermediate frames. With
the existing 150 ms minimum gap retained, the measured warm budget suggests
approximately **850–900 ms** per completed update rather than assuming 720 ms.
That is an estimate, not a tested target. Merely removing `busy` from the
current capture guard would block input/replies and retain producer ownership
races. A worker needs explicit fences, cancellation, age bounds and ACK tests.

**Ask the GPU for one complete rear-sized picture.** This is the closest
software analogue to stock's acquire-fenced, rear-sized composition. The local
Android tree provides `ScreenCaptureInternal.DisplayCaptureArgs.Builder.setSize`
and asynchronous capture; SurfaceFlinger has a CPU-readable output allocation
path. A persistent system-side producer could return a scaled 720×1440 image,
avoiding manual front-plane reconstruction and the measured 101 ms CPU resize.
It targets much of the current 235 ms capture/resize budget, but actual GPU,
readback, grayscale and fence costs must be measured before quoting a net gain.
GPU interpolation may render thin fonts differently, so compare text, photos,
rotation, system bars, wallpaper, protected-content behavior and frontlight
appearance. Keep logical Android display size unchanged. The current Java JNI
listener waits on the capture fence, so use a bounded worker/epoch protocol and
explicit buffer release; do not block input or allow a late result into a newer
request. This option also addresses mixed source-frame capture more directly
than heuristic additional sleeps.

**Use a stable spatial dither option.** A fixed screen-anchored ordered pattern
can avoid sequential error diffusion and keep changes local; it may reduce the
measured 55 ms dither stage and improve temporal stability. It changes the
image: gradients can show texture/banding and thin text needs inspection.
Keep today's Floyd–Steinberg output as the default and compare opt-in images
and conversion timings first. Alternating dither patterns over time would
create changes even on a static page and can add flicker/ghosting; it is not
an automatic improvement for this panel.

**Compare verified Fastest with Auto before changing cleanup.** Auto in this
run frequently switches between quality and A2 and performs quiet refreshes.
Observed 39/79-frame sequences take approximately 459/929 ms of waveform time,
versus 118 ms for warm A2. Staying in the existing Fastest mode can avoid some
mode-change/quality work, at the cost of less precise shading and potentially
more ghosting. Keep the stock-library transitions and periodic/manual cleanup;
record the effective property, emitted mode/frame counts, text quality and
long-run history. A2 optical residue, an intermediate captured animation and
an unchanged stale image are distinct problems and require different fixes.

Burst rail retention and any burst-boundary scan-count changes remain the
separate attended study described above. The first retention comparison alone
can target approximately 59–61 ms; it does not authorize eliminating 410 ms
of existing scans/rails. Generation overlap using stock's producer/consumer
ring is another future option, but it must prove no underrun extends a voltage
pulse and preserve all generated frames, errors and history. No such change
is enabled here.

### What modern research does and does not transfer

The primary [Ghostbuster manuscript](https://web.eng.fiu.edu/gaquan/Papers/ESWEEK24Papers/EMSOFT/EMSOFT_40_Hu)
(TCAD/EMSOFT 2024, DOI 10.1109/TCAD.2024.3446711) predicts optical ghosting from
previous images and adjusts/dithers the next source image. It does not require
replacing the electrical waveform. It needs panel-specific optical calibration
and careful history tracking. Its reported processing cost is 126.3 ms on a
Mate 40 Pro, with dual-buffer overlap proposed; this is not an A6L speed result.
Adding that processing before fixing this port's capture/scheduling would be
premature. The retained manuscript is for investigation, not an imported patch.

[NXP's EPDC reference manual](https://www.nxp.com/docs/en/reference-manual/IMX_REFERENCE_MANUAL.pdf)
describes snapshots, queued/merged work, collision handling, partial regions
and configurable power-down delay. These support the scheduling direction,
but its hardware can manage multiple independent update regions; our existing
A6L library interface is whole-image and already performs differential pixel
driving. A dirty rectangle alone cannot shorten a full-panel scan sequence.

[Modos/Caster](https://github.com/Modos-Labs/Glider) uses per-pixel counters and
early cancellation to process changed pixels before an older global update
finishes. That explains how high incoming frame rates can coexist with slower
particle motion. Reproducing it requires a different transition scheduler and
accurate optical state; aborting the existing A6L library waveform does not
provide that behavior.

Recent waveform research includes
[dynamic programming for waveform design](https://sid.onlinelibrary.wiley.com/doi/10.1002/jsid.2113)
(2025; publisher abstract reviewed) and
[multi-frame image compensation](https://pubmed.ncbi.nlm.nih.gov/39573029/)
(2024). These improve transition paths or optical consistency on their tested
panels. Their waveforms/calibration are not transferable defaults for ED058TC7U2
with the retained `320_R301_AFD521_ED058TC7U2_TC` waveform. First pursue the
measured software delays. The current warm ten-frame sequence at 85 Hz still
takes approximately 118 ms before preparation/idle overhead; no 15/30/60 fps
promise follows from another controller's advertised input rate.

### Isolated polling and first-frame candidates (3 Oct, r6o preparation)

The r6n controller already stepped appearance at 100 ms once it had noticed a
nonempty prepare request; idle bookkeeping ran at 500 ms. Merely changing the
pending interval would therefore save nothing. The polling candidate adds a
cheap prepare-property probe every 100 ms while awake, with full appearance
checks only for an outstanding request or cancellation. Brightness/timeout
bookkeeping remains at 500 ms, and screen-off removes both callbacks. A request
published at 25 ms in the actual-source scheduler test is detected at 100 ms
instead of the baseline's 500 ms. That can remove up to 400 ms of discovery
delay (473 ms was measured on r6n's first rear switch), with eight additional
property reads per second while idle/awake. It adds no sleep polling and changes
neither the native three-second prepare bound nor eight-second first-image bound.

The candidate in
`firmware/extracted/eink-r6n-research-20261003/prepare-poll-candidate` passes
135 actual-source scheduler/gate checks, isolated Android resource/Java/D8
compilation, and exact-source/hash verification. Root promoted this reviewed
candidate independently while the isolated validation finished; reverse patch
check confirms the promotion. It is not a physically measured latency result.

The separate first-frame candidate removes only Auto mode's extra capture to
reach its normal two-moving-frame burst threshold on a new rear switch. It
uses the already captured private image when the exact request/ready pair is
unchanged before and after capture, the existing scanout gate has completed,
the capture did not span a suppressed drag, and the panel has not ACKed this
request. Immediately before this priority decision it rechecks the current
exact request/ready pair. Busy/queue, retry backoff, minimum gap and per-minute
rate checks remain; explicit Quality/Partial/Fast/Fastest behavior, later Auto
policy and all electrical sequences remain unchanged. The selected first Auto
waveform is the same Fastest waveform the existing second moving capture uses.

Identical pixels do not introduce a shortcut: the existing outer source block
forces a pending update when the new exact pair lacks its own panel ACK. The
candidate retains that behavior and sets no ready property until a successful
frame reply. Mirror entry currently discards its previous shown/capture policy
baseline, so reusing an older optical image without driving needs a separate
exact-pixel/history proof design. Failed, disconnected and missing ACKs retain
the existing retry behavior.

The isolated first-frame payload in
`firmware/extracted/eink-r6n-research-20261003/first-frame-candidate` passes
4,534 actual-source guard/policy/ACK checks on the host and under ASan/UBSan,
1,500 normal/explicit policy-equivalence steps within those checks, 179 original
logic checks, 24 original appearance/gesture checks, three full
capture/resize/socket/ACK scenarios and the failed/disconnected/missing-ACK
regression. Verified Auto submits capture 1; stale or canceled readiness keeps
the original later submission. NDK API 34 compilation has zero warnings; host
GCC retains existing format-truncation warnings. The forward patch check passes
against the retained r6n mirror. Neither this candidate nor a bridge prewarm
experiment has been installed by this agent.

This candidate targets roughly one avoided capture/resize pass (about 235 ms
in r6n), not the 1.56-second cold first drive. It preserves the existing bounded
scanout fail-open; that opportunity is not a producer-ownership fence for the
CPU copy. The framework fresh-frame race fix remains a separate prerequisite
for reliable theme/wallpaper presentation. Physical acceptance must compare
request-to-apply, ready-to-first-queue, first-ACK/light release, identical-page
switches, stale/canceled sequences, and text/ghosting with the same Auto setting.

### Fenced compositor capture feasibility and default-off prototype

An isolated source-checked design and prototype are retained in
`firmware/extracted/eink-fenced-capture-20261003`. The platform controller can
request a logical-display capture via `IWindowManager.captureDisplay` with
independent X/Y scaling, retaining Android's logical size. WMS requires a
requested `READ_FRAME_BUFFER` permission even for the controller's system UID;
the current app does not explicitly request it, though another shared-UID package
may already supply the grant. Confirm runtime permission and request it explicitly
in a future backend manifest. Ordinary compositor output has CPU-readable
usage and its callback exposes a separate buffer after GPU completion. Explicit
secure/protected redaction would black out those surfaces on the rear; it must
not request their protected contents.

The local native Java callback calls `Fence.waitForever` before returning to
Java. Consequently a Java timeout can discard a late epoch and stop issuing
captures, but cannot cancel that native wait. A bounded design keeps one
unresolved capture credit after timeout, rather than accumulating new captures
or replacement threads. A hard fence deadline needs a separate native platform
listener. CPU mapper locks can also block; a timed-out worker must retain buffer
ownership until its operation actually ends. These limitations are documented
before any continuous backend is enabled.

The default-off mailbox passes 642 actual-source ownership checks, including
100 newest-pending replacements and exactly-once closure of 107 test buffers.
The compile-only Android API adapter passes Java/D8 compilation and 12 checks
against actual local framework signatures. It is not wired into startup, a
manifest, SELinux, sockets or the current ROM. No compositor capture or native
HardwareBuffer transfer has been performed. The next stage needs authenticated
handle transport, validated size/format/usage/stride, bounded worker lifetime,
exact appearance/gesture/geometry epochs and one-shot image/timing comparisons.
The measured target is the existing 235 ms capture-plus-resize work, less the
unmeasured replacement GPU/fence/lock/copy/transport cost; no speedup or optical
parity is claimed from compilation. Compositor filtering may alter thin text
and must be compared with the current area-average resize.

A separate read-only Fastest plumbing audit in that directory finds no
switch-time reset. Inline Settings, provider, legacy radio and native mirror
agree on `persist.sys.a6l.eink.refresh` and literal `fastest`; native mapping
sets `fixed_fast=1`. r6n's retained property is explicitly `auto`. The next
physical comparison should read property/provider before selection, after
selecting Fastest and after each screen switch, and verify `refresh mode
fastest` plus `fixed=1`. Current evidence establishes r6n's effective Auto
selection without establishing why an earlier preference differed.

### r6o repeated Auto cleanup: independent mode-correct candidate

The retained filtered r6o trace confirms that Auto's `POL_REFRESH` action says
`mode=quality`, but the mirror sends bare `refresh`. The daemon deliberately
preserves its last requested mode for that manual stock ghost-refresh command.
After A2, it therefore drives mode 8/force 1/39 frames; the next quality page
can still pay the mode 2/79-frame transition. Thirteen such forced A2 refreshes
appear in the saved trace, eight followed by a 79-frame quality update. This
establishes a mode disagreement, not a static-scene proof.

The isolated candidate in `firmware/extracted/eink-auto-cleanup-20261003`
serializes only automatic `POL_REFRESH` as the current captured page in its
policy-selected mode with `force`. Manual/key bare refresh remains unchanged.
The existing immutable frame and successful-ACK baseline path is used, and
explicit modes, policy thresholds, driver/electrical sequences remain intact.
It passes 39 actual-source command/pixel/ACK checks on host and sanitizers,
four complete capture-to-socket scenarios and the ACK error/disconnect/timeout
regression. NDK compilation and forward patch check pass. It is frozen and
not promoted; the directory's report and manifest carry hashes and results.

Two additional stock-library QEMU comparisons with the actual ED058TC7U2
waveform at 25 C confirm: forced quality directly after A2 takes 79 frames,
and the next changed quality image takes 39; preserving forced A2 takes 39,
then normal A2 stays at 10. Thus the candidate is a correctness change, not
a nonflashing cleanup: on a truly unchanged page it can lengthen the one
cleanup from 39 to 79 frames. REGAL mode 3 is available and generated 39
frames for A2 entry, repeated identical input and forced input in the tested
sequence. Those counts and the stock `REGAL_NonFlash` symbol do not prove
optical ghost removal, grayscale fidelity or absence of a black flash.
Changing Auto's settled rendering to REGAL is a separate policy decision.

The GPU agent's independent source audit found a definite DRM blend ABI
mismatch: this mirror's numeric None/Premultiplied/Coverage constants do not
match the retained kernel's raw values. Earlier composition tests used our
own constants and did not validate that external ABI. This can corrupt alpha,
opaque culling and measured damage even when the user sees a static menu.
Correct that capture bug and measure the remaining deltas before retuning
Auto or hiding them with thresholds. Existing explicit Fastest is the first
useful comparison afterward; the user reports no sharpening and tolerable
ghosting there. Keep the mode-correct cleanup candidate separate until its
longer static cleanup tradeoff is chosen explicitly.

### r6p residual switch delay and diagonal fields

The measured report and reproducible parsers are saved in
`firmware/extracted/eink-r6p-performance-20261003/report.md`. Canonical driver,
mirror, controller and ROM inputs stayed unchanged during this investigation.

All 67 submitted policy frames used Fastest (`fixed=1`), and all six switches
received valid framework presentation fences without a three-second fallback.
Rear light release took 3.497 s on the first switch and 2.352/2.353 s on later
switches; LCD return took 1.076/1.364/1.218 s. The later rear switches spent
about 0.9–0.95 s preparing Android, 21–34 ms on scanout gating/dispatch,
260–264 ms capturing/resizing, and 1.12–1.16 s from queue to physical ACK.
The latter includes 425–428 ms of bridge startup: retained image history
does not mean a running rear CRTC after an LCD visit. Ordinary warm updates
retain ten-wave 118 ms drive, 114.5 ms lead, 236 ms tail and 59 ms rails-on
medians. Capture/resize submission medians are 167/108 ms. Completion intervals
have a 1.155 s median for the 43 consecutive ordinary pairs under two seconds;
this is a workload-specific observation, not a controlled speed regression.

The corrected blend ABI removed black bars according to the user. Remaining
sloping gray fields still lack an exact input-raster/optical pair. The source
does have independent KMS property reads and sequential CPU copies without
producer-buffer ownership fences, but that does not prove the pictured fields
originate there. DMA-buffer CPU sync is cache coherency, not scene ownership.
Rejecting every capture spanning vblank would discard healthy static captures
too. No speculative capture patch or damage-threshold change was prepared.

The next attended experiment should preserve one exact submitted private luma
raster, the same raster after actual dithering/packing, and matching update
metadata/ACK, then photograph the rear after completion. A bounded pair of
rear-sized 8-bit rasters costs about 2.1 MB. This separates source pixels,
conversion and optical residue. The report lists individually reviewable
future changes: completed rear-sized compositor capture; one-active/one-newest
capture overlap; unchanged bridge preparation alongside appearance; and
instrumentation of remaining WM dispatch/lock delay. All remain unintegrated,
and electrical sequencing, stock waveforms and panel-history rules stay intact.

### Exact received-frame/conversion diagnostic candidate

`firmware/extracted/eink-exact-raster-20261003/README.md` now contains a frozen,
isolated implementation and run instructions, rather than only the comparison
proposal. Its native patch changes only the daemon. The existing real mirror
socket supplies the exact payload; the daemon snapshots those bytes before
conversion and inverse-maps its actual post-dither packed image afterward.
It records update/mode/force/frame count, timestamps and physical drive result.
Generated reply and mirror-observed ACK are explicitly distinguished.

Default off. A narrowly labeled nonpersistent
`debug.vendor.a6l.eink.trace_once` property accepts a strict 1–32 character
token, with shell set permission only for userdebug/eng policy. Runtime gating
uses readonly `ro.build.type=userdebug|eng`; the retained ROM intentionally
has `ro.debuggable=0`, which stays unchanged. The normal socket remains 0660
system/system and dump files remain 0600. One attempt per daemon instance writes
two fixed exclusive/no-follow rasters plus final metadata, about 2.076 MB total;
it never overwrites a prior pair or streams/grows a capture directory.

The candidate passed 1,390 actual-source checks on host and ASan/UBSan, eight full
baseline conversion identities and a real mirror/daemon socket integration
test with fake TCON/physical drive disabled. The NDK/API34 build has no warnings,
and native/policy patches apply-check cleanly. Root's actual SELinux/vendor
build, deployment and attended optical comparison remain pending. No shared
source or phone mutation was performed by this investigation. The dump's
copy/write cost must be measured, and bounded byte count is not a hard disk-IO
deadline. Post-conversion means before stock ModeDecision, whose transitional
behavior still needs interpretation from the recorded update history.

### Stock neutral-scan and burst lifecycle audit

The isolated report and reproducible RLE analysis are in
`firmware/extracted/eink-neutral-mailbox-20261003/`. No shared source or
electrical timing has changed. r6p warm physical cost is explained by 59 ms
rails-on plus 114.5 ms lead, 118 ms library waveform and 236 ms tail; the
observed scans track the 85 Hz cadence. Per-frame DRM event synchronization
is not an unexplained additional 400 ms delay.

The exact stock consumer ioctl is **FBIOPUT_VSCREENINFO (0x4601)**, not
FBIOPAN_DISPLAY as an older summary stated. Newly decoded retained kernel
code follows fb_set_var → fb_pan_display → mdss_fb_pan_display_ex and sets
the ordinary commit's wait field to one. Stock also waits for presentation.
Its consumer keeps the rear framebuffer open during bursts, closes after
three seconds idle, and on reopening checks the last-close timestamp against
199,999,999 ns with up to five 40 ms waits. Stock's PMIC shutdown also contains
a 100 ms discharge-related wait; a zero-duration sysfs return in our log does
not imply instantaneous physical discharge.

The verified generated 39-frame waveform file already ends with four
no-data-drive frames; the 117-frame files end with three. Our daemon adds
another twenty neutral scans, but the files and userspace assembly do not
prove a safe replacement count across panel temperatures/bridge states.
Raw stock framebuffer captures with unverified data-lane order are excluded
from that count. Every generated library frame remains required.

The source-informed mailbox design keeps exactly two rear raster slots,
one immutable active and one filling-or-ready. It preserves a single worker
credit after cancellation/timeout, rejects stale scene/gesture/appearance
epochs, and makes policy comparisons against the latest ACKed baseline.
It is a design, not a wired or runtime-tested backend. With all present rail
and neutral timing retained, hiding capture/resize under the previous drive
has an optimistic warm budget around 850–900 ms. Burst retention and any
burst-boundary scan reduction remain separate attended electrical/optical
experiments; no panel-rate or artifact-removal claim follows from this budget.

### Isolated capture-overlap foundation and resize candidate

`firmware/extracted/eink-native-overlap-20261003/README.md` documents an
implemented native worker/private-context library, default off and unlinked
from the mirror. Exactly two rear output slots are enforced; cancellation and
timeout retain a filling worker's credit until its real return. ACTIVE pixels
are immutable and only OK advances the optical baseline; stale OK cannot
acknowledge a new appearance. Actual-code tests cover late completion, newest
replacement, errors, shutdown, bounds and eight allocation-failure positions.
The generated real-source context passed 189 complete raster/geometry identities
against unchanged mirror code, host/leak-enabled ASan/UBSan and NDK compilation.
Coordinator/epoch/proof/policy and nonblocking socket integration remain required
before a runtime trial. Manual DRM producer coherence remains unproved; the
library does not remove diagonal artifacts by itself. No shared input changed.

`firmware/extracted/eink-uniform-row-resize-20261003/` is a separate small
output-equivalent patch which directly fills the horizontal resize intermediate
for uniform gray rows. Mixed rows keep the original math. It passed 571 full
byte/geometry identities including all 256 uniform values, host/ASan/UBSan and
NDK checks. Host synthetic reader resize saved 25.4%, uniform rows about 36%,
but random/photo input regressed 8.4%. Those figures are host CPU measurements,
not phone timings or FPS; this candidate remains unintegrated pending an ARM
measurement and explicit review of that tradeoff.

The exact-raster diagnostic's deployed property name is now
`vendor.a6l.eink.trace_once`: vendor Soong rejected the original debug.vendor
namespace. Root owns the one-name deployment adapter and policy build.
`firmware/extracted/eink-exact-raster-property-adapter-20261003/adapter-report.json`
records 1,390 host/ASan actual-source checks, eight packed identities, the actual
mirror/daemon socket pipeline with physical drive disabled, and clean NDK compile.
Original frozen artifacts/hashes are preserved; the original candidate README
prominently explains the deployed name and adapter provenance. Build gates,
tokens, permission types, files and electrical behavior did not change.
