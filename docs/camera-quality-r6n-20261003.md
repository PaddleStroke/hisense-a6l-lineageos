# r6n camera quality review — 3 October 2026

This review keeps the working r6n HAL/core/IPA/worker and ROM inputs frozen.
User testing confirms all three cameras produce images and rear center autofocus
helps, but focus still struggles, contrast is wrong, and video is choppy with a
perceived five-second start delay. These are image-quality/performance reports,
not another failure to open the camera.

## Physical log evidence

Evidence is `firmware/extracted/rom-r6n-20261003/feedback-live-logs/`, covering
11:38:53–12:05:46 Paris. Selected camera lines are retained separately in
`firmware/extracted/camera-controls-20261003/r6n-camera-focused.txt`.

- At 11:39:44.944 the current core finds the rear GT9769 through its ancillary
  link. AF initializes its DAC range 127–647. Physical user observations and
  many AF results establish that the AF path runs; a logged `focused` state
  alone does not validate the chosen optical focus.
- Rear scans repeatedly finish at DAC 137–388, often every three to seven
  seconds. Examples are 388 at 11:53:06.245, 337 at 11:53:10.810, 336 at
  11:53:15.469, 141 at 11:53:18.337 and 292 at 11:53:22.287. Scene motion and
  exposure changes are not logged, so this is consistent with hunting but does
  not prove the rescan threshold alone is wrong.
- Actual Debayer benchmark samples are 60,990 microseconds/frame for rear
  1600×1200 at 11:53:06.173 and 95,034 microseconds/frame for the front photo
  configuration at 11:53:40.320. These correspond to about 16.4 and 10.5 frames
  per second of processing capacity in those samples. They are not measurements
  of encoded video frame rate.
- Both recordings use the software `c2.android.avc.encoder`. Although the device
  profile is 1280×720, 15 FPS, 6 Mbps, CameraX selects rotated/cropped 720×1278,
  30 FPS, 11.98 Mbps. Advertised sensor timing and requested encoder FPS do not
  account for sustained CPU ISP and software encoder capacity.

| Recorded event | Rear video | Front video |
| --- | --- | --- |
| Recorder pending | 11:58:11.192 | 11:59:45.838 |
| Start event | 11:58:11.388 | 11:59:46.022 |
| First keyframe received | 11:58:12.096 | 11:59:47.420 |
| Muxer starts | 11:58:12.173 | 11:59:47.483 |
| Pending to muxer | 0.981 seconds | 1.645 seconds |
| Encoder stop to configured | 5.538 seconds | 5.477 seconds |

Thus the saved Recorder timeline does not yet show a five-second wait after
the recording request. Mode selection/reconfiguration and finalization are
separate delays; correlate user actions before attributing the perceived delay.
The front's first encoded buffer is dropped as outside the start/stop interval
before CameraX requests another sync frame. Media PTS and retained footage will
show what was actually lost.

## Source findings and hypotheses

The frozen source is `/home/a6l/libcamera-work/src-af-current-20261003`.

**Exposure and tonal mapping:** `algorithms/agc.cpp` targets a black-subtracted
linear RAW histogram mean of 0.4 at zero EV, uses proportional coefficient 0.2,
8% tolerance and sensor exposure/gain limits. Statistics arrive every four
processed frames. A simple no-delay, no-clipping feedback model needs 44 updates
to raise mean 0.1 into tolerance: 176 frames, about 5.9 seconds at 30 FPS or
17.6 seconds at 10 FPS. This is a model, not a physical convergence measurement.
The high linear target plus no highlight-weighted metering may clip bright
regions. Lowering it without examining actual media could make a different scene
too dark.

`Adjust` uses gamma 2.2, neutral contrast exponent 1 and no calibrated CCM.
All three installed sensor helpers supply black level 64/1024 (8-bit level 16);
the empty tuning BlackLevel node does not normally invoke estimation because
the helper overrides it. AWB is gray-world and can change rapidly on colored
subjects. No chart calibration, lens shading correction or stock ISP tone
mapping is implemented.

`DebayerCpu::pairToNV12` writes full-range BT.601. The recorded encoder output
declares color-standard 1, color-range 1, transfer 3; the intermediate CameraX
RGB/surface conversion must be traced before treating this as a mismatch. JPEG,
camera preview and encoded video may have different range/matrix interpretations.
RAW10 debayer discards the two least significant bits before its LUT: this is
existing upstream CPU processing behavior, a possible dark-gradient quality
limit, not evidence of a packing error.

**AF:** Sharpness is the horizontal squared difference of sampled green pixels
in the central half of rows and columns. It is normalized by global green mean,
so a dark/bright or different-depth center subject can be misrepresented by the
surrounding scene. The algorithm assumes a requested lens move has settled by
frame N+3, with the next statistics normally at N+4. There is no physical lens
acknowledgment or measured lens position in that association. Coarse/fine scans
use roughly 14–16 measurements in the physical logs, so four-frame cadence at
low actual FPS explains long scans. Continuous mode restarts after three
statistics below 65% or above 160% of the previous sharpness reference. Fast
scene/AE changes can trigger full sweeps. Selected tap regions remain absent.

**Metadata:** AGC supplies AE state only on statistics frames. The HAL defaults
missing state to SEARCHING on the other three frames. A truthful cached AE state
is a candidate follow-on; assess actual CameraX waits before attributing startup
to it. Repeated invalid control 0x271a is FACE_DETECT_MODE, not a failed exposure
or lens command.

## Measurements for the copied media and next focused run

1. For every JPEG: dimensions, orientation, EXIF exposure/focal length/aperture;
   per-channel and luma percentiles; fractions below 4 and above 250; center
   versus corners; neutral patch channel ratios. Separate clipped highlights,
   crushed shadows, color cast and blur. ISO remains a placeholder, so do not
   derive actual gain from ISO 100. Without a chart/reference image, color
   calibration cannot be measured accurately from arbitrary scenes.
2. For each MP4: packet/frame PTS gaps, count/duration, median/p95/max interval,
   repeat-frame fraction, resolution/bitrate, range/matrix/transfer tags and
   first keyframe. Decode sample frames to inspect source exposure, focus
   changes and compression separately. Parent owns audio analysis.
3. A bounded later debug run should correlate frame number and sensor timestamp
   with requested/applied exposure/gain, RAW histogram mean/clipped bins, AWB
   gains, AF DAC/metric/settling, ISP processing time and encoded PTS. Current
   INFO logs lack these per-frame values; no conclusion about actual AE
   convergence can be recovered from the existing range messages alone.
4. Before tuning AF thresholds, use a stationary textured target at known depth
   under stable light. Record the measured coarse/fine curve, final DAC and a
   short confirmation measurement after the final move. Check focus at several
   manual DAC positions around the chosen peak, and read this unit's OTP in an
   attended session if needed. Do not widen travel or invent calibrated
   dioptres from the approximate fallback range.

The initial review did not change camera source or tuning. The following
isolated candidate adds narrowly reproduced fixes and bounded measurement
without modifying the frozen r6n package or shared Soong inputs.


## Actual copied media and source-backed findings

Seven new JPEGs and two MP4s were copied from r6n and inspected. Current photos
are upright. Rear keyboard lettering is readable and much sharper than the
previous unfocused sample, but several scenes have raised shadows and heavily
clipped highlights; the laptop/window sample has about 48% luma above250.
A stationary chart or RAW reference was not captured, so the elevated JPEG
shadow floor alone does not identify incorrect black-level calibration.

Rear EXIF exposure is32.929ms; front33.217ms; wide299.725ms. The stock-derived
focal length/aperture values are present, but ISO100 remains a placeholder and
cannot establish actual gain. The two videos deliver about4.6–4.7 unique frames
per second. The software encoder request is30FPS/~12Mbps despite the initial
15FPS/6Mbps device profile. Recorded requests reach the muxer after0.981s and
1.645s; ~5.5s is also observed when draining/stopping/reconfiguring. These timings
must be distinguished from the user's perceived delay entering video mode.

The sensor helper black level4096 on the16-bit scale becomes16 on the8-bit
software statistics scale; the debayer subtracts/clamps this level before its
gamma2.2 lookup. That lookup can output zero. Full-range RGB-to-NV12 is passed
as YCbCr to libjpeg without a separate limited-range expansion. This source
trace does not prove the sensor black level or tone curve is calibrated.
No arbitrary gamma, black-level, color matrix or AF threshold was changed.

Saved rear JPEGs occupy exactly3121961 bytes, and wide2889041 bytes, although
JPEG EOI appears hundreds of kilobytes earlier, leaving2.37–2.75MB of padding.
The framework looks for the JPEG footer at the logical BLOB byte capacity,
whereas the HAL used the larger physical allocation. The candidate uses the
actual minigbm logical BLOB width, reserves the footer during encoding and
writes it with memcpy at the possibly unaligned logical end. Overflow is
bounded and reported. This fixes JPEG bookkeeping, not photo contrast.

## Isolated combined candidate, not physically tested

Source `/home/a6l/libcamera-work/src-quality-combined-20261003`, build
`build-hal-quality-combined-20261003`, package `halpkg-quality-combined-20261003`.
Workspace package is
`firmware/extracted/camera-controls-20261003/payload-quality-combined`.
The complete source delta, hashes, commands and IPA signature verification are
in the same directory as `camera-quality-jpeg-aestate-diagnostics.patch`,
`quality-combined-candidate.json` and `quality-combined-payload-manifest.json`.

Changes are the logical JPEG capacity/footer fix, persistence of measured AE
state between statistics frames, and opt-in bounded diagnostics. Previously
missing non-statistics-frame AE state caused the Android HAL to report SEARCHING
between real convergence measurements. Exposure/gain regulation is unchanged.
No new AF regions, manual sensor controls or unsupported image processing are
advertised.

Actual-source regressions pass297 checks: IPA/AF/AE/RAW10=169, BLOB/encoder=60,
minigbm NV12 import=44, cancellation=7, optics parser=17. Another56 checks cover
diagnostic opt-in, invalid values, frame bounds and monotonic time. All changed
Android objects and the229-target isolated HAL/core/IPA/worker build pass.
The stripped IPA signature verifies and its294-byte public key matches the
compiled core. Worker directory remains `/vendor/libexec/libcamera`.
The original base library5c3ad25a... is retained: all20 base source files,
normalized compiler commands, config and exported ABI are identical; rebuilding
only changes embedded source paths and linked layout. Replay regressions with
`python3 firmware/extracted/camera-controls-20261003/replay-quality-combined-regressions.py`
in WSL with the retained dependencies described by its command JSONs.

Diagnostics require exact environment `LIBCAMERA_A6L_QUALITY_DIAGNOSTICS=1`
when launching the provider; its worker inherits that environment. Default off.
Only request sequences0–239 are logged for each stream. Reopening/reconfiguring
can start another window; root must remove the opt-in for ordinary runs.
No init property expansion behavior is assumed by this package. Fields include
RAW mean and top-bin fraction, AWB gains, AE state/EV, requested gain/exposure,
AF metric/command DAC, sensor timestamp/request sequence/queue depth and ISP
elapsed time. Driver G_EXT_CTRLS values are cached at statistics time and
explicitly **not frame-effective**. DAC is not physical lens-position readback.
Statistics validity and unsynchronised-control writes are labeled. The logs
may perturb timing and should be compared with a diagnostics-disabled baseline.

The earlier conjecture that lack of CAMSS frame-start events permanently froze
the delayed-control ring was rejected: push() advances the ring on processing.
Physical-frame association remains unproven and is now measured honestly.

## Narrow video follow-on proposal

The current HAL echoes Android AE target FPS without translating it to a real
sensor control. The simple pipeline publishes fixed timing after configure and
only clamps sensor blanking to30FPS. Advertising arbitrary15FPS at the HAL
would therefore be misleading. Before adding real VBLANK/rate control, the
smallest supported load experiment is a smaller video output (for example a
real negotiated640x480 stream) and a lower encoder bitrate, keeping capture
metadata truthful. That reduces software debayer and encoder work and can be
verified with the new ISP/queue/timestamp logs. It does not promise15FPS and
must check the actual negotiated stream and encoder settings, not only an
XML profile. A real15FPS limit would need controlled VBLANK mapping or deliberate
frame pacing with truthful metadata and tests; neither is in this candidate.
