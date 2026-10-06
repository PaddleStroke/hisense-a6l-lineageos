# Camera and e-ink investigation — 3 October 2026

Current diagnostic run uses installed r6p, boot
`dc39b553-eddc-4a73-9f1b-eb7e7e15266e`. All three EV-probe tests save both
JPEGs and reports; originals, the longer settled rear photo and complete live
streams are in `firmware/extracted/rom-r6p-20261003/causes-run`.

## Confirmed camera findings

* The provider abort comes from reading exposure/gain as Integer32 before
  validating presence/type. The retained method reproduces the actual type
  assertion; an early guard passes the reproduction and request-completion
  checks. The exact physical callback that supplies malformed controls still
  needs the new bounded rejection logs.
* RAW resizing dominates physical CPU ISP time: main median about 152 ms out
  of 210 ms; wide about 146 ms out of 193 ms. Both sensors run near 30 FPS,
  while selected/processed frames are around 5 FPS. An output-equivalent
  row-cache/identity-bypass candidate is built, signed with the retained key
  and independently reviewed; physical speed improvement is not yet measured.
* Wide timing metadata is demonstrably false. Its Camera2 report advertises
  299.963 ms frames and 299.725 ms exposure, while physical sequence/timestamps
  show a 33.341 ms frame interval. The driver fixes PIXEL_RATE's initial range
  to 32 MHz and silently clamps the later 80 MHz request. LLP3800/FLL2526
  interpreted at 32 MHz exactly reproduce the erroneous report. Changing only
  that range still leaves a transport-versus-array clock-domain error.
  Actual LLP/FLL/cadence imply roughly 288 MHz effective timing. A narrowly
  scoped candidate compiled and passed the driver/helper checks; sensor mode,
  PLL and link programming remain unchanged. Physical validation is pending.
  Current AGC works in exposure lines, so correcting reported
  microseconds alone does not fix its dark image.
* Exposure does respond. The longer static rear run raises gain from roughly
  6.1× to 16× and RAW mean from about 22.6 to 42–45. Gain reaches its current
  ceiling while exposure is already 2154 lines/32.415 ms. The user confirms
  gradual brightening. The twelve-second probe waits do not reach convergence;
  low ISP throughput slows the four-frame statistics cadence. Saturation below
  the retained 0.4 linear target also explains continuing AE SEARCHING here.
* A separate actual-source reproduction proves shared statistics can be
  overwritten before asynchronous IPA consumption. Deferred frame128 can read
  frame129's invalid statistics; frame129 can read frame132's valid statistics.
  This is a frame/statistics ownership bug, with matching drain symptoms in the
  physical trace. A lifecycle-safe snapshot/ownership correction must precede
  the next camera bundle; the existing signed cache/guard package stays frozen.

These findings do not establish stock-equivalent colour, noise reduction,
highlight handling or AF precision. No arbitrary gamma, black-level, exposure
target or colour-matrix changes have been promoted. The early and settled
photos have different framing, so they are not a controlled photometric pair.

The genuine covered-lens repeat (`ev-test-1791054953093`) is dark: JPEG mean
about 1/255 with median near zero. This argues against a large raised black
floor/range mismatch. RAW histogram estimates are near configured black16 but
are too coarse to calibrate the pedestal. Dark-scene AWB produces a nearly-zero
red gain through its floored channel-ratio formula; the r6p statistics ownership
defect remains a confounder, so recheck on r6q before tuning. The first purported
covered pair still contains a visible scene and is excluded from calibration.

The Linux sensor interface distinguishes pixel-array timing from CSI transport
rate; this matters to the wide clock-domain correction. See the primary
[pixel-rate contract](https://cdn.kernel.org/doc/html/latest/userspace-api/media/v4l/ext-ctrls-image-process.html)
and [CSI transport-rate distinction](https://cdn.kernel.org/doc/html/latest/driver-api/media/tx-rx.html).

## Confirmed e-ink findings and missing boundary

The numeric DRM blend mismatch caused the black top/bottom strips; Pierre
confirms their removal on r6p. Moving diagonal/irregular fields remain.
The recorded later LCD-to-rear switches take 2.35 s: about 0.9 s appearance
readiness, 0.26 s capture/resize, and 1.12–1.16 s daemon/bridge/drive work.
Bridge startup is about 425 ms; the unchanged warm panel sequence is about
530 ms. Around 350 ms comes from explicit ten-lead/twenty-tail neutral scans,
while the ten-frame A2 waveform is about 118 ms. Stock also waits for framebuffer
commits, but retains the panel across bursts and has no corresponding extra
scan loop in the inspected userspace consumer. Verified longer library sequences
already end with neutral scans; no safe reduced external count has been proven.
This is a measured serial latency budget, not a fixed one-second timer. Details
are in `firmware/extracted/eink-neutral-mailbox-20261003/report.md`.

Manual KMS capture samples planes separately and lacks immutable compositor
buffer ownership across its CPU copies. That is a real coherence limitation;
it is not yet proof of the optical artifacts' origin. An unrelated later
`screencap` cannot resolve that boundary.

The next diagnostic saves exactly one immutable received rear raster and its
actual post-dither/pack raster, with mode/update/physical-result metadata.
The tested candidate is default off, about 2.1 MB, never overwrites existing
files, and changes no waveform, rails, screen policy or normal pixel output.
Its property gate supports this userdebug ROM's `ro.debuggable=0`. Compare the
pair with the same acknowledged optical frame: artifacts in input implicate
capture; artifacts first in conversion implicate dithering; clean rasters with
bad panel output point to waveform/history/drive. Physical deployment remains
pending. See `firmware/extracted/eink-exact-raster-20261003/README.md`.

## Build state

### r6q physical results and stop regression

r6q installed at 21:26 Paris without development-image snapshots. Trusted
threaded IPA is confirmed. Main/wide ISP medians remain 209.916/195.946 ms;
RAW resampling alone costs 155.777/149.954 ms. Selfie has no RAW resize and
measures 94.693 ms per ISP frame (240 samples). These are processing times,
not delivered FPS. The prior row cache has not demonstrated a rear speed gain.

Wide eventually brightens normally, matching Pierre's observation. Exposure
product increases from 8.3 ms × 1 to 33.3 ms × 16 over about 47 seconds. The
controller uses absolute normalized error × 0.2, so near-black 0-EV images
increase only about 8% per update. Updates occur every four processed frames
(about 0.8 seconds). That spacing guards the delayed sensor-control pipeline;
removing it without association changes is not a safe shortcut.

The repeated open/switch abort is now pinned to `queuedRequests_.empty()` at
`PipelineHandler::stop`, source line 398, by exact installed-ELF disassembly.
Stopping shared-statistics ownership drops metadata callbacks, but some real
Simple pipeline requests already have their buffers back and still require
that metadata to finish. Clearing the frame map loses the remaining requests.
The proposed repair cancels those requests truthfully after endpoints retire,
detaches bookkeeping before completion callbacks, and keeps both stop assertions.
The real Request/PipelineHandler fixture reproduces the baseline abort and
passes 85 checks with the candidate. This closes a gap in the earlier mocked
ownership test; physical stop/reopen validation remains necessary.

### r6q e-ink Auto observations

Tiny exact pixel damage with equal tile averages is currently promoted to a
synthetic motion fraction. Repeated minor changes can therefore start fresh
fast bursts and later forced cleans. Exact pixel submission and quiet timing
must be preserved while separating that from tile-motion burst detection.

The Auto `POL_REFRESH/quality` action still sends bare `refresh`; the daemon
then forces the retained A2 mode instead of selecting quality. A corrected
selected-quality raster produces one 79-frame transition in the retained
library, whereas the old forced A2 cleanup has 39 frames before a subsequent
quality transition. One corrected clean can be longer: it is not flash-free.
An observed large post-clean delta also coincides with a real WM transition,
so it does not prove the cleanup itself corrupted the captured image.

The completed attended log and numerical summary are under
`firmware/extracted/rom-r6q-20261003/attended`. The phone is now reachable in
diagnostic recovery; pstore is empty. r6r then passed its narrow incremental
vendor audit and installation, retaining system/boot/DTBO. It boots and its
native-wide diagnostic saves and closes both trials without camera aborts.

### r6r native-wide physical comparison

Wide 1600×1200 JPEG/preview has median ISP192.649 ms and warmed preview callback
interval196 ms. Selecting already-advertised native1624×1224 JPEG with mapped
1600×1200 preview removes Bayer resampling: ISP51.789 ms, callback55 ms. Both
bounded tests saved a photo and closed in the same boot. Normal Camera selection
is still unchanged; there was no matched stock or attended optical comparison.
See `firmware/extracted/rom-r6r-20261003/native-wide-comparison/summary.json`.

### r6s normal-app selection and Auto policy

All three ordinary Camera previews opened and closed without an observed abort.
Selfie selects native2296×1728 (239 ISP samples, median93.526 ms); main and wide
still select1600×1200 and perform RAW resampling. Native configuration lines
emitted during capability enumeration do not prove active selection. The actual
per-frame output and active stream dumps distinguish these cases.

The filter incorrectly excluded enableHighResolution=true. CameraViewModel
forwards this resource capability, not a selected user capture mode; runtime
overlay lookup confirms it is true. The isolated correction removes that
predicate while retaining actualJPEG/4:3/noextension restrictions, unchanged
allowed-resolution mode and fallback ordering. Independent review, fullActivity
compile,66 source-extracted eligibility assertions and25 policy assertions pass.
It still requires rebuilt-app physical negotiation; filter-first logs alone
cannot establish the negotiated final stream.

The ordered-dither r6s staticAuto trial still emitted one79-frame forced clean
and three ordinary39-frame quality updates. Exact minor pixel damage triggers
the latter (fractions0.000062/0.001852/0.000062). There were no daemon errors or
30-second capture-failure pauses; accepted-capture logs are sampled and cannot
provide an exact accept percentage. Static capture+resize medians124+102 ms
rose to332+118 ms during drawer actions. No optical acceptance is implied.

### Moving e-ink artifacts: narrowed mechanisms

The retained DMA-BUF CPU-read synchronization waits existing implicit rendering
write fences. It does not establish an owned producer read reservation against
future reuse, nor make separate plane/property queries one atomic compositor
snapshot. Capture coherence therefore remains a candidate, but saying there is
no GPU rendering synchronization would be inaccurate.

A separate actual-source daemon test demonstrates Floyd–Steinberg diffusion
amplification. Changing one gray246 pixel to245 in an otherwise constant raster
changes494,788 output pixels,47.7%, spreading diagonally. Identical input reruns
are byte-identical: this is deterministic input-sensitive propagation. It is a
plausible mechanism for the observed moving texture, not proof of the optical
video's cause. The exact input/post-dither diagnostic can distinguish conversion
propagation from source capture damage. An alternative spatially anchored dither
is isolated for comparison; no conversion default was changed in r6r.

The app-only WM notification is integrated in both source trees. Plain property
publication did not itself dispatch the existing WM property callback in the
other process; the candidate explicitly uses the retained one-way Binder
notification after target and theme publication. Existing fresh-draw/fence
checks remain intact. Its physical latency benefit has not been measured.

The incremental r6q system build completed at 20:50 Paris. Full inventory shows
only the switcher APK/odex/vdex and three generated build-property files changed;
all other system files match r6p. The image is frozen for the vendor build.
Exact-raster source/policy and the narrow Hi846 metadata module are staged.
Camera cache/guard/ownership integration is signed and complete. Vendorimage
passed after namespace/diagnostic setter corrections; the final image inventory
contains exactly the reviewed 18 changed system/vendor files. The r6q kit is
staged, installed and booted with hash-only checks and verified readback. Shell arming of the vendor diagnostic
property depends on this ROM's unchanged permissive mode; no enforcement or
general setter restriction is relaxed. All electrical programming and unrelated
ROM inputs are retained. See `docs/rom-r6q-20261003.md` for scope and limitations.
