# A6L camera controls candidate — 3 October 2026

## Physical r6m results

Rear and selfie preview now work and two JPEGs were saved. This confirms the r6m
gralloc metadata bridge fixed the earlier SurfaceFlinger import failure. It does
not establish autofocus, exposure quality, video, or camera switching quality.

The user reports both cameras inverted by 180 degrees, rear close-up lettering
blurred, several-second camera switching, no useful tap-focus/exposure control,
and video errors. Root inspected the two saved JPEGs: the rear indoor scene is
strongly out of focus, the selfie is materially sharper, and both are inverted.
There is no saved keyboard photo.

Evidence: `firmware/extracted/rom-r6m-20261003/r6m-user-camera-sleep-20261003.tar.gz`
and `recovery-diagnostics/photos` alongside it. Rear JPEG has actual exposure
0.011462 s; selfie 0.025695 s. Their ISO100, focal length1, and f/0.0253 fields
were HAL placeholders and must not be treated as measured camera properties.

## Orientation correction

The DT copied stock Android clockwise MountAngle values into Linux's
counter-clockwise sensor rotation property. The Android libcamera HAL correctly
inverts that property, resulting in rear270/front90 rather than stock rear90/front270.
Physical logs explicitly report the configuration mismatch. The kernel defines
the [rotation control as counter-clockwise](https://docs.kernel.org/userspace-api/media/v4l/ext-ctrls-camera.html).

Only these DT properties need changing: IMX576 rear90→270, S5K3T1 front270→90,
HI846 wide90→270. Android YAML remains rear90/front270/wide90. Root owns exact
mutation of the retained boot DTB; do not regenerate every overlay from draft
source. Proposal and provenance are in
`firmware/extracted/camera-controls-20261003/camera-rotation.patch` and
`rotation-provenance.json`. The existing native-orientation stream validation
remains unchanged.

## Autofocus

The installed r6m HAL advertises AF_OFF only, minimum focus distance0, no focus
regions. The current software IPA had no AF algorithm wired into the Android
package. Thus no automatic focus or tap-focus is expected on that build.

The isolated candidate applies the retained September29 implementation0101–0104
on top of the physically working0015 baseline. It adds actual RAW10 sharpness
statistics, contrast autofocus, lens controls across IPA IPC, pipeline lens
writes, and Android request/result mapping. It advertises AF only when libcamera
has discovered a real focus lens with V4L2_FOCUS_ABSOLUTE. Front/wide remain fixed
focus. Rear tuning uses the stock no-OTP GT9769 DAC range227..627 with a bounded
scan127..647, three-frame settling, and default position235. See
`docs/camera-af-video-20260929.md` for derivation.

The recovery environment test loaded only the retained proven camera bundles
after all manifest hashes and kernel vermagic matched. It found GT9769 on
`/dev/v4l-subdev20`, all three sensor chip IDs, and passed MODE=env. No streaming,
explicit focus movement, or OTP reading was performed. Evidence is
`af-env-probe-20261003.txt` and `af-env-discovery-20261003.txt` in the candidate
evidence directory. The old ENUM_LINKS output contains pad links only; it cannot
prove presence or absence of the sensor→lens ancillary link.

Next boot must provide that proof through root's read-only G_TOPOLOGY logger and
the new core log `A6L focus lens discovered via ancillary link: ...`, followed by
`A6L_AF_INIT`. Physical convergence and per-unit OTP calibration remain untested.
Do not replace missing lens discovery with fake AF metadata.

The candidate uses a center window, not arbitrary selected focus regions. It
continues to advertise maxRegionsAF=0. The exact bundled CameraX1.7.0-alpha03
bytecode rejects startFocusAndMetering when AF/AE/AWB region counts are all0.
Consequently Aperture's current tap action will not trigger center AF either;
continuous AF is the useful first test. Region-based tap focus needs real ROI
statistics/mapping or a clearly labelled app center-focus action later.
Evidence: `camera-camera2-focus-bytecode.txt`, generated with javap from the
actual bundled AAR. No ROI support has been invented.

## Real exposure compensation and optical metadata

Existing software AGC/AWB already calculate controls from image statistics and
write sensor exposure/gain. The user observation alone does not prove automatic
exposure absent. Old Android metadata always reported AE_CONVERGED and no
compensation range, so those fields also were not evidence of convergence.

The candidate now implements ExposureValue in the actual AGC request path and
advertises Android compensation[-3,+3] with 1/3EV steps only when that control
exists. The linear RAW histogram target is0.4 at0EV, approximately preserving the
old five-bin MSV2.5 target. It scales by2^EV over[-1,+1]EV, with bounded per-frame
correction, exposure/gain clamps and hysteresis. Captured-frame metadata carries
the applied EV and actual SEARCHING/CONVERGED state. This changes the coarse
five-bin calculation to a linear histogram mean; physical dark/light adaptation
and highlight quality require testing. It is not a claim of stock ISP quality,
AE lock, manual shutter/ISO, or selected-area metering.

Stock `firmware/extracted/vendor/etc/camera/camera_config.xml` directly establishes:

| Sensor | Focal length | Aperture |
| --- | --- | --- |
| IMX576 rear | 3.95mm | f/1.8 |
| S5K3T1 front | 3.2mm | f/2.0 |
| HI846 rear wide | 1.66mm | f/2.2 |

These values now flow through per-sensor YAML, static metadata, capture results,
request templates and JPEG EXIF. Invalid/nonfinite/nonpositive YAML optics are
rejected. The ISO field remains a placeholder pending proper sensitivity
calibration; do not treat it as measured. Accurate optical metadata and root's
Aperture auxiliary-camera enablement are distinct from proving wide preview.

## Switching timeout: cancelled RAW capture reported as success

Recorded camera switching waited5366ms for disconnect, then opened the new sensor
in roughly0.6–0.9s. Camera service timed out draining the last request, with
metadata already returned, no buffers outstanding, and shutter timestamp0.

The actual simple pipeline error callback completed user-facing conversion
buffers without copying the cancelled internal RAW buffer status. Their old
success status made Request complete successfully despite having no real sensor
timestamp. The HAL then sent shutter0. Current framework keeps such a request in
flight because successful completion requires a nonzero shutter timestamp.

The candidate propagates internal capture failure to every output buffer.
STREAMOFF cancellation therefore becomes RequestCancelled and Android
ERROR_REQUEST with error buffers. The HAL additionally rejects missing/nonpositive
timestamps or failed buffers rather than sending a fictitious successful shutter.
Abort returns owned internal stream buffers. This is a source-supported cause
and fix; a physical switching test is still required to determine whether it
accounts for all delays.

## Frozen payload and validation

Isolated source `/home/a6l/libcamera-work/src-af-current-20261003`; build
`build-hal-af-current-20261003`; package `halpkg-controls-af-20261003`.
Workspace copy: `firmware/extracted/camera-controls-20261003/payload`.
The working ee1e8... baseline source/package were preserved.

| File | SHA256 prefix |
| --- | --- |
| lib64/hw/camera.libcamera.so | 49fafb47374ba3c5 |
| lib64/libcamera.so | 334e61da5c1c843d |
| lib64/libcamera/ipa/ipa_soft_simple.so | 2b9b9f10b044d00e |
| libexec/libcamera/soft_ipa_proxy | 89117b6978198748 |
| lib64/libcamera-base.so (retained) | 5c3ad25ae2b5b53d |

Package matching core/IPA/worker together: focus controls alter IPC. The compiled
proxy directory is `/vendor/libexec/libcamera`. IPA was stripped, signed, and
verified; the generated embedded core public key matches the signature key
byte-for-byte. Full hashes: payload/SHA256SUMS and payload-manifest.json. No
private key is copied to the workspace/package.

Tests run against actual source, with ASan/UBSan on host fixtures:

- 161 IPA checks: conditional lens controls, actual AF scan/settling/cancel,
  RAW10 sharpness for four Bayer orders/padded rows, exposure limits, real EV
  requests, sensor adjustment, convergence proportional to2^EV and invalid inputs.
- 17 actual YAML parser checks: correct sensor optics and invalid values rejected.
- 7 actual simple-pipeline callback checks: two real output buffers, real
  Camera/Request completion, cancelled request and drained conversion queue.
  The old callback separately reproduces false-success-with-no-timestamp in6 checks.
- 44 retained actual minigbm importer/postprocessor checks pass against the new
  host core, including aligned NV12 planes, metadataFD exclusion and BLOB/JPEG.
- Retained AF search/tool suite also passes71 search checks and RAW10/tool tests.
- Android HAL, core, IPA and executable worker compile successfully. No whole
  ROM/image was built by the camera agent.

Reproducible fixture sources, compile commands and logs live in the evidence
directory. `camera-controls-complete.patch` is the complete source delta from
the physically working0015 source, including retained AF patches and new fixes.
It is promoted as canonical
`device/hisense/a6l/camera/libcamera/patches/0016-a6l-focus-exposure-capture-completion.patch`
with SHA256 pin a7fe4622b65cd2a501e26faeb27566810bde2d639edf40925f1cb480219d44b5.
The legacy AF source helper skips0101–0104 after0016; its obsolete regeneration
step stops before edits. Canonical rear tuning/optics and future worker packaging
were updated. Existing frozen binaries/data are unchanged.

Next attended validation: prove lens discovery; check upright rear/selfie JPEGs;
hold a detailed target in the center at near/far distances and inspect actual AF
result logs; test0/-1/+1EV against the same stationary scene; switch cameras
repeatedly and confirm the last request drains without the5s timeout. Then test
wide camera and video separately. Root owns the Codec2 input-surface/video fix;
portrait encoder720x1278 negotiation still needs a successful physical recording.
