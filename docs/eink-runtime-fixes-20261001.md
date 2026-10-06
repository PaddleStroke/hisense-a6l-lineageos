# E-ink responsiveness investigation — 1 October 2026

Pierre's installed-r6e report: side-switch controls and rear touch work, but
switching and interaction take about six seconds; fastest mode still flashes;
white bars occupy both sides. Performance fixes below are incorporated into
the final r6f rebuild. They have not yet been installed or timed on the panel.

## Evidence and changes

The recorded log in `logs/eink-timing-evidence-20261001.txt` contains two
LCD→e-ink transitions. One shows key release at 07:29:32.640, clear request at
07:29:34.359, clear acknowledgement at 07:29:38.675, then quality redraw
acknowledgement at 07:29:40.206. Thus about 7.6 seconds from switching to image
acknowledgement: 1.7 seconds before the command, 4.3 seconds clearing, then
1.5 seconds showing the page. Another entry clear takes 4.39 seconds and its
quality redraw 2.80 seconds. These phone-clock timestamps precede the software
renderer isolation test; the log file also contains its later events.

The mirror forced `clear` then `frame ... quality` on every entry. The clear
command itself runs white GC16 then INIT, explaining repeated flashes.
The candidate removes this entry sequence: epdd already initializes at startup,
retains its library/panel image across CRTC off, and performs its mandatory
recovery clear when a drive fails. Manual and configured periodic clears remain.
Pierre described the LCD flashing; which panel flashed is awaiting clarification.

Fast/Fastest previously selected only a scrolling burst waveform; taps and
settled pages still used quality. The candidate uses the selected waveform for
all changed pages in these explicit modes, without an automatic quality settle.
Auto, Quality and Partial retain their policies. Fast modes still honour the
configured periodic clear interval, counted in their page updates. Ghosting
and the first waveform transition need physical evaluation; no panel timing
or rail sequencing has been modified.

The mirror continued capturing and resizing while a panel command was in
flight, although it could not submit another frame. It now polls input/replies
while busy, then captures the latest page once the command completes. Expired
capture deadlines do not cause a busy-spin. Reply timeout/backoff, failed-frame
resends, and rear-touch recovery remain tested.

Opaque unrotated 1:1 LCD planes now use integer pixel conversion; transformed
or alpha-blended planes retain the general path. Resizing now traverses rows
contiguously, reusing precomputed horizontal weights, and logs its own timing.

## Validation

- Final host suite: `EINK3_HOST_TESTS pass=18 fail=0`; covers policy, keys,
  touch mapping/loss, page-flip/rail failures, socket replies and retry recovery.
  New policy assertions cover entry/taps in both fast modes, unchanged pages,
  busy gating, periodic clears and return to Auto.
- Phone CPU-only differential benchmark: 256 plane cases and two full LCD
  frames matched the original implementation byte for byte. RGB565: 425.224 ms
  → 58.482 ms; 32-bit: 406.823 ms → 41.014 ms. These measure pixel conversion,
  not end-to-end display latency.
- Phone resize differential benchmark: 28 complete output images matched,
  covering portrait/landscape, crop/letterbox, small/upscaled images, contrast
  0/100 and 1080×2160. LCD resize: 112.962 ms → 94.996 ms.
- Standalone NDK mirror/epdd compilation passed; the updated mirror compiled
  without warnings. Soong compilation and image/QEMU validation are being run
  on the final r6f snapshot.

Evidence: `firmware/extracted/eink-perf-20261001/`, WSL
`/home/a6l/eink-work/perf-20261001/`. Benchmarks ran as adb shell, using small
temporary binaries; no panel access, ROM installation or phone backup.

## Full-area layout

Source currently fits 1080×2340 into 720×1440: about 665 px of image width,
with 27/28 px white bars. Crop would hide top/bottom content; stretch would
expand the width about 8.3%. Prefer Android reflow to a 2:1 logical layout,
for example 1080×2160 at the current density, preserving complete controls.

A resolution change alone is insufficient for the current DRM mirror: Android
may centre its logical viewport within the physical 1080×2340 scanout. The
mirror must then capture that viewport and map rear touch to logical coordinates.
The pinned SurfaceFlinger capture path uses layerStackSpaceRect for ordinary
screenshots, offering a way to confirm logical geometry during a temporary test.
The phone currently reports physical 1080×2340, density 400, no overrides.

Attended test at 14:43 UTC saved the original size/density (no overrides)
and requested 1080×2160. The screenshot succeeded, and SurfaceFlinger showed
logical content 1080×2160 centred at physical y=90..2250. This confirms that
the mirror would need to remove those top/bottom scanout margins before
scaling, together with matching rear-touch mapping.

Pierre reported a freeze while navigating to Display settings. Mesa sysmem
was active and radio disabled. The automatic restoration timed out because
adb shell stopped responding; original geometry has **not** been verified
restored. Restore `wm size reset` at the next accessible Android boot, after
preserving crash evidence. Do not ship automatic aspect switching on the
basis of this failed test. Logs: `logs/layout-2to1-20261001T144330Z/` on the laptop.
Recovery yielded empty pstore and only an early-boot metadata log, without
coverage of the freeze. After reboot, software rendering was selected and
`wm size reset` succeeded: original 1080×2340, density 400, no overrides.
Restoration is now verified; tag `restore-layout-20261001T150100Z`. Native
2:1 reflow remains unvalidated and is absent from r6f.

Automatic switching
needs a reliable framework-side observer plus restoration on LCD return/boot,
and validated viewport/touch mapping. No default stretch/crop or forced size
is shipped in this candidate.
