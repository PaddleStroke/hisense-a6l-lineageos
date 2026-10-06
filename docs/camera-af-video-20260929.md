# Camera phase 3 prep: GT9769 autofocus + video recording plan (29 Sep 2026)

Worker item `camera-af-video`. Offline only: no phone, no adb/fastboot, nothing flashed, no `m` in the Lineage tree.
This work does not change the libcamera phase 2 ROM files (`device/hisense/a6l/camera/libcamera/**` and the ROM
patch series 0001-0008 are only read). Everything new is under `device/hisense/a6l/camera/af/` and in a separate
libcamera branch (`a6l-af`, `/home/a6l/libcamera-work/src-af`).

## 1. Status

| Part | State |
|---|---|
| Stock focus calibration decoded (actuator tuning, EEPROM AF layout, lens data) | **READY** (s.2) |
| libcamera AF: SoftISP sharpness statistic, soft IPA lens plumbing, contrast AF algorithm, Android HAL AF mapping (patches 0101-0104 on top of the ROM series 0001-0008) | **READY offline**: recovery build and Android HAL compile check, both `-Werror`, 0 warnings. **Not in the ROM.** |
| Host tests `af/tests/test-af.sh` (AF search on simulated focus curves, ASan/UBSan; sharpness tool; EEPROM decoder; tuning keys; patches == branch) | **AF_TESTS_PASS** (AF_SEARCH_PASS 71/71; worst focus error 5 DAC, at most 20 measurements) |
| Recovery AF test `run-af.sh` + bundle `firmware/extracted/libcamera-af-20260929` (16 files, SHA256SUMS `06aef636…`) | **READY**, staged on the laptop as `v75/libcamaf` (16/16 OK) |
| VCM movement, EEPROM read, sharpness curve, AF convergence on the phone | **NEEDS AN ATTENDED TEST** (s.5) |
| AF in the ROM (merge 0101-0104 into the ROM series + Af tuning block + HAL rebuild) | **Needs the phase 2 owner / Pierre**, after the attended test (s.6) |
| Video recording | **Plan only** (s.7). Software Codec2 first, then venus + v4l2_codec2. |

## 2. What stock tells us (focus calibration)

- **Actuator** (`vendor/lib/libactuator_gt9769.so`, `.data`, `actuator_driver_params` + `actuator_tuned_params`):
  I2C 0x18 (8-bit) = 0x0c on CCI0, 10-bit DAC. Init 0x02=0x02, 0x06=0x61, 0x07=0x39 (already used by our `gt9769.ko`).
  Tuning: **initial_code 227**, one region of **400 steps x 1 code** (so 227..627 when no OTP), ringing threshold
  472, damping step 0xfff / delay 1000 us both directions (hw_params 0x180 / 0xfe80).
- **Lens** (`camera_config.xml`, camera 0): focal length 3.95 mm, f/1.8, **minimum focus distance 0.07 m**, total focus
  distance 5 m, HFOV 65.9 / VFOV 52.0.
- **Per-unit AF calibration** is in the module EEPROM (`libmmcamera_imx576_hmct_eeprom.so`, EepromName `imx576_hmct`).
  Memory map in the lib: slave **0xB0 (8-bit) = 0x58**, word addresses, read **0x0000 x 0x0B05** then **0x1800 x 0x0422**
  into one buffer. `format_afdata` (disassembled): **0x708 flag (1 = valid), 0x709 macro DAC (big-endian 16),
  0x70B infinity DAC (big-endian 16), 0x720 checksum = sum(0x709..0x71F) & 0xFF**. The starting DAC is set to infinity.
  Margins stored with it: **infinity -0.25, macro +0.05** of the infinity..macro span (the scan range). The stock DT
  powers the EEPROM with cam_vana (our `cam_avdd_2p8`, gpio51, shared with the VCM).
- Our values: without the OTP, the AF uses the stock no-OTP range: infinity 227, macro 627, widened to a scan of
  **127..647**. After the attended EEPROM read, put this unit's values in the Af block (`infinityDac`, `macroDac`).

## 3. Kernel side (no change)

`gt9769.ko` (camfix2 build, staged in the ROM) already exposes a `MEDIA_ENT_F_LENS` subdev with
`V4L2_CID_FOCUS_ABSOLUTE` 0..1023. The DT (`a6l-camera-v75.dtso`) links it with `lens-focus = <&vcm_main>`, and
`imx576_a6l` registers with `v4l2_async_register_subdev_sensor()`, so v4l2-async should create the **ancillary link**
that libcamera's `CameraSensor` uses to find the lens. This is not proven yet: `AF_LENS_PASS` in the attended test proves it.
Runtime PM: the lens is powered while its subdev is open. It parks slowly to 0 on suspend and ramps back on resume.
libcamera keeps the subdev open while the camera manager runs. The test script holds it open (`exec 7<`).
Probe log so far: `GT9769 VCM initialised (stock mode 0x61 / timing 0x39)` (t33-t38). The lens has never been moved
in an attended test (`MODE=focus` was never run).

## 4. libcamera AF (branch `a6l-af`, patches `af/libcamera/patches/0101-0104`)

Base: libcamera v0.7.2 + the ROM patches 0001-0008 (the exact ROM series, applied at build time and not copied).

| Patch | Content |
|---|---|
| 0101 software_isp: swstats sharpness | `SwIspStats.sharpness/sharpnessCount`: the squared difference of the averaged green of consecutive sampled 2x2 blocks (4 px apart). It covers the centre half x half of the stats window (= the SoftISP output crop) in the RAW10 CSI-2 packed line functions (all 4 Bayer orders; the 3 A6L sensors are RAW10P). Per-thread row gating (debayer threads), summed in `finishFrame`. Other formats report count 0, and AF stays manual. |
| 0102 simple: lens plumbing | `soft.mojom`: `init(..., lensControls)` + event `setLensControls`. SoftwareIsp passes `sensor->focusLens()->controls()` and relays the event. `SimpleCameraData::setLensControls` calls `CameraLens::setFocusPosition`. The IPA context gets `lens.present/min/max` before the algorithms are created. |
| 0103 ipa: simple: Af | `algorithms/af.{h,cpp}` + `af_search.h` (pure logic, host-tested). Controls only when a lens exists: `AfMode` Manual (default) / Auto / Continuous, `AfTrigger` Start/Cancel, `LensPosition` (dioptres 0..10.5 = the widened macro end). Metadata `AfState`, `LensPosition`. The search runs a coarse sweep infinity->macro (13 positions, early stop once 2 samples fall below 70 % of the peak), then a fine sweep of +-1 coarse step (7 positions), then a parabolic fit. It fails on a flat or low-texture scene (lens returns to 0.2 dioptre, near hyperfocal). Continuous mode rescans after 3 measurements below 65 % or above 160 % of the focused value. Stats come every 4 frames, and a valid stats frame less than `settleFrames` (3) after a move is skipped. Measure = mean squared difference / (mean green - black)^2. Tuning = `Af:` block in `imx576_a6l.yaml` (AF copy `af/libcamera/data/imx576_a6l.yaml`). Log: `A6L_AF pos= sharp= state=` (Debug) and `A6L_AF_RESULT` (Info). |
| 0104 android: AF | Only when the camera exposes `AfMode`: AF modes OFF/AUTO/MACRO/CONTINUOUS_VIDEO/CONTINUOUS_PICTURE. Templates: continuous picture (video: continuous video). `LENS_INFO_MINIMUM_FOCUS_DISTANCE` / `HYPERFOCAL` from the LensPosition range, calibration APPROXIMATE, `LENS_FOCUS_DISTANCE` request/result, `LENS_STATE` MOVING while scanning. AF_STATE mapping: AUTO -> ACTIVE_SCAN/FOCUSED_LOCKED/NOT_FOCUSED_LOCKED; CONTINUOUS -> PASSIVE_*; a trigger START in continuous mode locks (IPA switched to Auto: the scan finishes, then the lens is kept) until CANCEL. |

Builds (in `/home/a6l/libcamera-work`, own dirs and logs, shared cross files only read):
`af/libcamera/tools/build-libcamera-af.sh gen src rec bundle hal`. gen = the patches from `a6l_af_edit.py`. rec =
recovery build `/tmp/libcamaf` (cam): AF_REC_OK, 0 warnings. hal = Android HAL with the ROM options:
AF_HAL_OK, 0 warnings, `libcamera-hal.so` + `ipa_soft_simple.so`. This is a compile check only: nothing is packaged
into `camera/libcamera/prebuilt`.

## 5. Attended test (next camera session, V75-usb recovery, about 10 min; after the camera15/17 probe)

Tools: `af/run-af.sh` (in the bundle), `a6l_afotp` (READ-ONLY EEPROM dump: 2-byte address pointer write + read,
8-byte chunks, retries 1-byte chunks and then while streaming), `a6l_afsharp` (sharpness of a captured frame:
XRGB/ABGR/RGB/NV12/RAW10P), `af-script-auto.yaml` / `af-script-continuous.yaml` (cam capture scripts).
Scene: a printed page or textured object 20-30 cm away, good light, phone on a stand. Then something far away.

```
A="adb -s HLTE730T-PROBE"; cd ~/A6L-usb-20260915
(cd v75/libcamaf && sha256sum -c --quiet SHA256SUMS && echo LIBCAMAF_SHA_OK)   # SHA256SUMS = 06aef636a34150232c4f1bee2d0c13332ff8c64510a9a127d5869f6be99ff11c
$A shell 'rm -rf /tmp/libcamaf /tmp/afout'; $A push v75/libcamaf /tmp/libcamaf     # camera15 (or camera17) + libcam2 pushed as in libcamera-plan s.10.4
$A shell 'chmod -R 755 /tmp/libcamaf'
$A shell 'export PATH=/tmp/bin:$PATH; D=/tmp/camera15 MODE=probe sh /tmp/camera15/run-camera.sh 2>&1 | grep -E "PASS|FAIL"'
$A shell 'export PATH=/tmp/bin:$PATH; MODE=env   sh /tmp/libcamaf/run-af.sh 2>&1 | grep A6L_AF'   # ENV_PASS, VCM /dev/v4l-subdevN i2c-4
$A shell 'export PATH=/tmp/bin:$PATH; MODE=click sh /tmp/libcamaf/run-af.sh 2>&1 | grep A6L_AF'   # Pierre: lens click / movement?
$A shell 'export PATH=/tmp/bin:$PATH; MODE=otp   sh /tmp/libcamaf/run-af.sh 2>&1 | grep A6L_AF'   # A6L_AF_OTP macro=.. infinity=.. csum=OK
$A shell 'export PATH=/tmp/bin:$PATH; MODE=sweep FINE=1 sh /tmp/libcamaf/run-af.sh 2>&1 | grep A6L_AF'   # 17+6 positions, ~1 min
$A shell 'export PATH=/tmp/bin:$PATH; MODE=af    sh /tmp/libcamaf/run-af.sh 2>&1 | grep A6L_AF'   # AF_LENS_PASS, TRACE, AF_auto_PASS, VERIFY
$A shell 'export PATH=/tmp/bin:$PATH; MODE=cont  sh /tmp/libcamaf/run-af.sh 2>&1 | grep A6L_AF'   # move near -> far during the 10 s
$A shell 'cd /tmp && tar cf /tmp/afout.tar afout'; $A pull /tmp/afout.tar v75/logs/
```

Pass criteria and what each result tells us:
- `CLICK_DONE` and an audible or visible movement: the VCM works (never proven before).
- `OTP_PASS` + `A6L_AF_OTP_YAML infinityDac: I macroDac: M`: put I/M into the Af block (expected in the region of
  200-300 / 550-700). `OTP_FAIL` (NACK) with the retries: the EEPROM needs another supply or pin. Keep the no-OTP
  range. The dump `eeprom-imx576.bin` is also the LSC/AWB calibration source for later.
- `SWEEP_BEST pos=P ratio_permille>=1200` = **SWEEP_PASS**: sharpness depends on the lens position, and P is the
  in-focus DAC for that distance. A far scene should peak near infinity (OTP value) and a 10 cm page near macro.
  This curve proves the statistic and the range independently of libcamera AF.
- `AF_LENS_PASS` (libcamera found the lens via the ancillary link) and `AF_auto_PASS` (`A6L_AF_RESULT state=focused`).
  The TRACE lines are the per-position sharpness of the IPA statistic. `VERIFY` must show a sharper frame at the AF
  result than at infinity for a near subject.
- If `AF_LENS_FAIL`: AF controls are absent (cam aborts with "Unsupported control 'AfMode'"). Check
  `/sys/class/media` links (`media-ctl -p` if available). The fallback is to link the lens in the pipeline by entity name.
- Nothing here writes flash. `a6l_afotp` never writes data bytes. Unload as usual with camera15 `MODE=off`.

## 6. Moving AF into the ROM (after a passing attended test; phase 2 owner / Pierre)

1. Copy `af/libcamera/patches/0101-0104` to `camera/libcamera/patches/` (after 0008) and rebuild the HAL package
   (`build-libcamera-a6l.sh src hal halpkg`). 0104 touches `camera_device.cpp`, which 0005 also changes. It applies
   cleanly on 0001-0008 today (checked by `build-libcamera-af.sh src`).
2. Add the `Af:` block (`af/libcamera/data/af-block.yaml`, with this unit's OTP values) to
   `camera/libcamera/data/imx576_a6l.yaml`. Without the block the IPA does not create Af, and the camera stays fixed focus.
3. No sepolicy change is expected: the provider already opens `/dev/v4l-subdev*`. `LensPosition` 0.2 dioptre is the rest
   position at stream start.
4. Per-unit OTP in the ROM (later): read the EEPROM at boot (a small vendor init tool) instead of fixed yaml values.

## 7. Video recording plan (H55)

**Constraints now.** SoftISP debayers on the CPU. Patch 0004 adds NV12 output. The ROM HAL (patch 0005) runs a
**single processed libcamera stream**, and extra Android streams are produced by the HAL post-processors (YUV
crop/scale via libyuv, JPEG). A recording session needs preview + encoder surfaces (+ a snapshot) at the same time,
so the HAL must feed 2 YUV consumers from one SoftISP output. `post_processor_yuv` does that on the CPU. The
attended check is Camera app video with preview 1440x1080 + record 1280x720.
CPU: 4x Kryo 260 gold (2.2 GHz) + 4 silver. SoftISP at 1440x1078 already runs at 30 fps (t38).
`media_profiles_V1_0.xml` (phase 2) lists H.264 720p at 15 fps for now.

**Phase V1: software Codec2 (no new driver).** AOSP `c2.android.avc.encoder` (libavc) + `c2.android.aac.encoder`
are already in the Lineage 24 system image. Target **H.264 1280x720 at 30 fps (fall back to 15), 6 Mbit/s,
AAC 48 kHz**. Work: (a) HAL: at least 2 YUV output streams from one NV12 SoftISP stream (check 0005 +
post_processor_yuv on the phone); gralloc usage `VIDEO_ENCODER` on an NV12 (YCbCr_420_888) buffer. (b) Measure CPU
(SoftISP + libavc + preview composition) and temperature (thermal HAL, 60 C shutdown guard) over 5 min at 720p30.
(c) Set `media_profiles` to the measured sustainable rate; set `CamcorderProfile` high = 720p. (d) Audio: main mic via
the existing audio HAL input (works for calls/recording; check the AudioRecord `CAMCORDER` source routing). About 3-5
days including attended tests. Expected result: usable 720p. 1080p is unlikely in software alongside SoftISP.

**Phase V2: venus hardware codec.** Everything upstream exists:
- Kernel: mainline `qcom,sdm660-venus` (HFI 3xx, `sdm660_res`, fw `qcom/venus-4.4/venus.mdt`). Node
  `video-codec@cc00000` in `sdm630.dtsi` (status disabled, 20 `mmss_smmu` SIDs, `venus_region` 0x9f800000).
  `CONFIG_VIDEO_QCOM_VENUS=m` in r5, and `venus-core/enc/dec.ko` are already built in `out-a6l-rom-r5` (not staged).
  `mmss_smmu` is enabled by our display DT overlay.
- Firmware: stock `VENUS.MDT` + `VENUS.B00-B04` (NON-HLOS, `firmware/extracted/peripheral-firmware-20260917/modem/IMAGE`).
  They are signed for this SoC and loaded through TZ PAS (same as the ADSP/modem loaders that already work). They are
  installed as `/vendor/firmware/qcom/venus-4.4/venus.mdt` + `.b0x`, or squashed to `.mbn`.
- Android: `external/v4l2_codec2` is in the Lineage tree (`android.hardware.media.c2-service-v4l2`, stateful V4L2
  decoder/encoder components, as used on db845c with venus). Add the service + `media_codecs` XML
  (`c2.v4l2.avc.encoder/decoder`, `c2.v4l2.vp8.*`, `c2.v4l2.hevc.decoder`) and sepolicy for `/dev/video*` (venus
  m2m nodes, distinct from CAMSS). Keep the software codecs as fallback.
- Steps: 1. a DT overlay enabling `&venus` (+ the interconnect/GDSC check), stage the 3 modules, firmware in vendor.
  2. Recovery test with `v4l2-ctl`/a small m2m tool: decode an H.264 file and encode a YUV file (attended).
  3. v4l2_codec2 service in the ROM, CTS-like MediaCodec smoke test, then camera recording through `c2.v4l2.avc.encoder`
  (zero-copy dma-buf from the HAL YUV stream). 4. Thermal/power check.
- Risks: venus on SDM660 has had little mainline testing (CPR/interconnect votes, secure buffer regions). The
  v4l2_codec2 encoder expects a stateful encoder with NV12 input, which venus provides. About 2-3 weeks including
  attended sessions. Decode also frees the CPU for video playback (H55 decode side).

## 8. Needs Pierre

- Run the attended test (s.5) in the next camera session: listen/watch for the lens during `MODE=click`, and provide
  near/far scenes for `sweep`/`af`/`cont`.
- Decide when AF enters the ROM (s.6) and whether the phase 2 owner merges 0101-0104 before the first installed-ROM camera test.
- Video: agree the target (720p30 software first). Venus (V2) is a separate multi-week item.

## 9. Files

- `device/hisense/a6l/camera/af/libcamera/src/{af_search.h,af.h,af.cpp}`: AF algorithm sources (copied into the tree by the edit script)
- `device/hisense/a6l/camera/af/libcamera/tools/a6l_af_edit.py`: source edits (steps stats/lens/af/hal, exact-match)
- `device/hisense/a6l/camera/af/libcamera/tools/build-libcamera-af.sh`: gen/src/rec/bundle/hal
- `device/hisense/a6l/camera/af/libcamera/patches/0101-0104-*.patch`: the branch as patches (== `src-af-gen` commits)
- `device/hisense/a6l/camera/af/libcamera/data/{imx576_a6l.yaml,af-block.yaml}`: AF tuning
- `device/hisense/a6l/camera/af/run-af.sh`, `af-script-{auto,continuous}.yaml`: recovery test
- `device/hisense/a6l/camera/af/tools/{a6l_afsharp.c,a6l_afotp.c,stage-libcamaf-laptop.sh}`
- `device/hisense/a6l/camera/af/tests/{test-af.sh,test_af_search.cpp}`: `bash test-af.sh` -> AF_TESTS_PASS
  (`LIBCAMERA_TREE=/home/a6l/libcamera-work/src-af-gen` also checks the patches against the branch)
- `firmware/extracted/libcamera-af-20260929/`: recovery bundle (cam v0.7.2+AF, signed IPA, AF yaml, run-af.sh, tools),
  laptop `~/A6L-usb-20260915/v75/libcamaf`
