# A6L hardware integration review — twelfth pass

28 September 2026. **Complete: one new finding, F66 (P2).** GNSS position accuracy loses the modem's confidence information before reaching Android. Reproduced offline with the production wire codec, position parser and Android mapping. Production source and the phone were not modified.

Previous findings are linked from the [eleventh-pass report](C:/Users/Pierre/Desktop/A6L/docs/hardware-review-round11-20260928.md). The source snapshot below identifies this review's inputs; ongoing integration and previously built images may differ. No approval or security check interrupted this pass.

## F66 — GNSS uncertainty is reported as Android accuracy without accounting for confidence

**Sources:** [position parser](C:/Users/Pierre/Desktop/A6L/device/hisense/a6l/gnss/lib/loc_v02.cpp:206), [Fix representation](C:/Users/Pierre/Desktop/A6L/device/hisense/a6l/gnss/lib/loc_v02.h:105), [accuracy mapping](C:/Users/Pierre/Desktop/A6L/device/hisense/a6l/gnss/lib/android_map.cpp:33), and [HAL callback assignment](C:/Users/Pierre/Desktop/A6L/device/hisense/a6l/gnss/hal/Gnss.cpp:328).

The parser reads horizontal circular uncertainty and vertical uncertainty but ignores their confidence TLVs, 0x16 and 0x1D. `Fix` has no fields to preserve those confidence values. The mapper then copies the uncertainties directly into `horizontalAccuracyMeters` and `verticalAccuracyMeters`, setting both validity flags. The HAL forwards those values unchanged.

The modem fields describe uncertainty together with a confidence percentage: see the [Qualcomm LOC position-report definitions published in AOSP](https://android.googlesource.com/platform/hardware/qcom/gps/+/android-4.2.2_r1/loc_api/loc_api_v02/location_service_v02.h) and the corresponding [libqmi wire schema](https://raw.githubusercontent.com/linux-mobile-broadband/libqmi/main/data/qmi-service-loc.json). Android's [GnssLocation contract](https://android.googlesource.com/platform/hardware/interfaces/+/refs/heads/main/gnss/aidl/android/hardware/gnss/GnssLocation.aidl) defines both accuracy fields at 68% confidence. A radius carrying another confidence level cannot generally be relabeled as a 68% radius without a justified conversion or conservative policy.

**Reproduction:** construct successful QMI position indications with fixed coordinates, valid ellipsoid altitude, horizontal uncertainty 10 m and vertical uncertainty 20 m. Encode and decode each packet, verify that both confidence TLVs survived the wire codec, then execute the production position parser and Android mapper:

| Supplied horizontal / vertical confidence | Android horizontal accuracy | Android vertical accuracy | Both accuracy flags |
|---|---:|---:|---|
| 39% | 10 m | 20 m | Set |
| 50% | 10 m | 20 m | Set |
| 68% | 10 m | 20 m | Set |
| 95% | 10 m | 20 m | Set |
| Not supplied | 10 m | 20 m | Set |

The 68% case is a positive control: no confidence conversion is needed for that case. A second control omits the uncertainty fields and confirms that the mapper leaves both accuracy flags unset. Thus the issue is loss of confidence semantics, not a fixture that always produces accuracy fields.

**Impact:** when confidence is below 68%, the advertised accuracy can be too optimistic. Navigation, location fusion and applications evaluating fix quality can therefore receive misleading precision metadata even when the coordinates and altitude datum are correct. Higher-confidence input is also not normalized, although retaining a larger radius may be conservative. This is separate from F22's altitude datum and F64's time-assistance freshness.

The reproduction does not measure this modem's usual confidence values, actual position error, or navigation performance on the phone. It proves the conversion defect for valid confidence-bearing reports. The absent-confidence case shows that no distinction is preserved; it does not establish a default confidence for this firmware.

**Proposed fix:** retain horizontal and vertical confidence plus presence flags in `Fix`. Normalize each uncertainty to Android's confidence convention using a documented model appropriate to the modem's output. Horizontal radial uncertainty and one-dimensional vertical uncertainty require different treatment; do not apply one arbitrary multiplier to both. Validate confidence ranges and finite uncertainty values. When conversion is not justified, use an explicitly documented conservative policy or withhold the unsupported accuracy claim, handling framework requirements for a usable location accordingly. Resolve omitted-confidence behavior from the applicable position-report contract or captures; do not borrow defaults from the separate position-injection request.

**Acceptance:** exercise independent horizontal/vertical confidence values below, at and above 68%; omitted and invalid confidence; omitted, zero and non-finite uncertainty; and altitude absent/assumed. Verify the numeric result against the chosen conversion model and inspect the actual Android callback. Capture real LOC position reports to establish the firmware's confidence behavior. Keep the existing altitude-datum and time-assistance tests alongside these checks.

## Evidence and scope

[Reproduction instructions](C:/Users/Pierre/Desktop/A6L/research/hardware-review-round12-20260928/README.md), [results](C:/Users/Pierre/Desktop/A6L/research/hardware-review-round12-20260928/results.json), [run log](C:/Users/Pierre/Desktop/A6L/research/hardware-review-round12-20260928/run.log), [hashes](C:/Users/Pierre/Desktop/A6L/research/hardware-review-round12-20260928/source-hashes.json), and [snapshot](C:/Users/Pierre/Desktop/A6L/research/hardware-review-round12-20260928/reviewed-source.zip) are preserved. The snapshot includes the local Android `GnssLocation.aidl` contract.

The reproduction passed under ASan/UBSan with no reported diagnostics and normal process exit. No snapshotted file changed during the run. Passing confirms the defect, not acceptance of a fix. The HAL forwarding was inspected in source; a full Binder stack was not executed.

The broader pass inspected QMI response matching, SIM session/file-I/O mapping, signal-strength conversion and USB gadget integration. No additional finding is promoted for those paths. The unfinished USB gadget implementation remains an already documented, excluded component; it is not counted as a newly discovered installed-ROM defect. Existing hardware acceptance gaps remain open.
