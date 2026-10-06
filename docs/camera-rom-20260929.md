# Camera in the ROM: what fixed the stream, the clean rom1 camss patch, staging, the camera15 confirm test and the Android camera plan (29 Sep 2026)

**Status:** offline only. No adb, no phone, no reboot, no image build, nothing flashed.

- **What is proven on the phone:** the attended camfix12 run t35 (camera14 bundle) captured full RAW frames from imx576 (rear) and s5k3t1 (front), plus one real photo (`logs/t35-cam14/photo1.png`).
- **Built and staged offline, not yet run on the phone:** the clean ROM module (`qcom-camss` rom1) and the camera15 confirm bundle.

## 1. What fixed the stream

### 1a. Evidence (t34 `logs/t34-cam13/kmsg-live.txt` fail vs t35 `logs/t35-cam14/kmsg-live.txt` + `c14.tar` pass)

| | t34 (camfix11, 12 sensor rows) | t35 (camfix12, 12 sensor rows) |
|---|---|---|
| result | all FAIL: done 0-2, 1 bus error, every packet an ECC event | **all PASS**: 94-116 frames done per row, 0 bus errors, pkts/ecc/crc `…/0/0`, maxl 2157 / 1728 |
| CSID0 regs (`A6L_CSID0_REG 000`) | `50000000 00032103 00000009 00002100 00000001 0000002b` | **identical** |
| CSID1 ctrl0/ctrl1/lut/cid | `00032103 / 00040009 / 2b / 23` | **identical** |
| CSIPHY lane config (cfg3 settle, cfg4 clk, ctrl5/6/7) | `14 … a5 d5 01 02` (settle 0x0e/0x13 in the settle rows) | **identical**; lane readback `d7 08 14 00 52 00 00 0a … b8`, clock lane `c0 08 14 a5 52` |
| CSID core clock `csiK` (set by the CSID) | 310 MHz | 310 MHz |
| CSIPHY timer | 269.33 MHz | 269.33 MHz |
| **`csiphy_clk_src` (CSIPHY digital clock)** | never set by any camfix11 code | **19.2 MHz at the first CSIPHY power-on after boot** (row 1 `CLK PRE … gparent csiphy_clk_src 19200000`, MMCC CFG `00000000`). Then 200 MHz (`cfg 00000505`), 269.33 MHz or 100 MHz from row 1 on, **in every row** |
| CSID IRQ status (0x64) | `3a0f28ff` (error bits 16, 25, 27-29) | `090e28ff` / `080008ff` |

**Why the controls passed.** The "controls" `inoclk` (row 3) and `snoclk` (row 11) did not set the clocks, yet they passed.
- The RCG keeps its last rate after `clk_disable`. The `CLK PRE` lines of both controls show `csiphy_clk_src 200000000`, `cfg 00000505` and `csi0/csi2 310000000`, left there by the rows before them.
- So the controls ran with the fix already active. They were not negative controls.
- For the same reason, `iclk100` (100 MHz) and `iclk269` (269.33 MHz) passing shows that any rate well above the 19.2 MHz XO rate works. Stock uses 200 MHz.

### 1b. What is proven and what is inferred

**Proven:**
- With `csiphy_clk_src` at 100, 200 or 269 MHz (read back through the clk API and the MMCC CFG register), both CSIPHY0 → CSID0 (imx576) and CSIPHY2 → CSID1 (s5k3t1) deliver full clean frames.
- The effective CSID and CSIPHY register programming is byte-identical between the failing t34 run and the passing t35 run. The camfix11 register knobs (settle, CTRL7, CORE_CTRL_1, LUT) are therefore **refuted** as the cause, and so is any "CSID register programming change".
- After a fresh boot the mainline state is `csiphy_clk_src` = 19.2 MHz, with `csiphy0` enabled only as the parent of `cphy_csid0`.

**Inferred (strong, but no same-boot negative control):**
- t34 ran every row at 19.2 MHz. camfix11 had no clock readout, but no code in camfix1-11 sets `csiphy_clk_src`, and t35 shows that the boot state is 19.2 MHz.
- The CSIPHY digital lane logic, clocked far too slowly, corrupts the deserialised byte stream. That is the t34 signature: every packet an ECC event, spurious SOFs, TG clean.

**Refuted as the fix:**
- **`a6l_v12` bit 16 (stale poll-pointer clear).** t34 row 1 was the first stream after boot, so no stale pointer was possible, and it failed exactly like the others. bit 16 only protected camfix10 diagnostic MMIO polls, which do not exist in rom1.
- **bit 2 (CSID rails).** It was only set in the `ivreg`/`svreg` rows.
- **A different boot state.** The t35 row-1 PRE line shows the plain mainline boot state.

**Optional true negative control (attended, only if wanted):** first capture after a fresh boot, with the camera14 bundle:
```
D=/tmp/camera14 MODE=bars SENSOR=imx576 WM=6 V6=128 V10=1 V11=1 V12=0x11 TAG=-neg sh /tmp/camera14/run-camera.sh
```
Expected result: FAIL as in t34, with `CLK PRE … csiphy_clk_src 19200000`.

### 1c. Minimal set kept in the ROM (rom1)

| kept (fixed behaviour, gated on `CAMSS_660`, no knob) | origin | why |
|---|---|---|
| **Stock CSIPHY digital clocks.** At CSIPHY k power-on: `cphy_csidK` set to 200 MHz (propagates to `camss_csiphyK_clk` → `csiphy_clk_src`) and enabled; `csiK` set to 310 MHz and enabled. Both released at power-off. | camfix12 bit1, without its diagnostics | **the fix** (§1a). The csiK part is stock; not proven needed, and harmless. |
| ISPIF on sdm660, CSID2/3 parent ops, `vfe-mem` ICC path | camfix2/3 (camss.c) | the pipeline does not exist without them |
| CSIPHY `clk_mux` ioremap on sdm660 | camfix | CSIPHY2 → CSID1 routing (t35 `clk_mux 00000001 (csid 1)`) |
| CSID core clock ≥ the 310 MHz level, CSIPHY timer at the 269.33 MHz level | camfix2 (`a6l_csid_fast` / `a6l_phy_fast`, default on) | stock rates, used in every t33-t35 run |
| VFE core clock ≥ 404 MHz | camfix5 (`a6l_vfe_min`) | stock; the WM unified buffer overflows at 120 MHz |
| WM ping/pong MAX address, CGC override, stock frame-based BUFFER_CFG | camfix4 | stock; without MAX address every burst is a bus error |
| Nested VFE power-domain link (`a6l_pd_users`) | camfix3 | ISPIF reset nesting |
| `qcom_camss.a6l_wm` (the only parameter). **Default 6** = stock BUFFER_CFG + fixed driver ping/pong copied into the queued vb2 buffer (the t35 mode). 3 = zero-copy stock-like, 0 = upstream. | camfix3/4 | 6 is the only mode proven with the clock fix. camera15 tests 3 and 0 (§5). |

**Removed:** every diagnostic and experiment of camfix1-12. That covers `a6l_dbg` register dumps and per-IRQ lines, fdump, the UB experiment, the CSID IRQ-mask knob, a6l_v6-v12, stk, TPG, the WM reload/max-size bits, the 0xa5 fill and the stop scan. As a result `camss-csid-4-7.c`, `camss-csiphy-3ph-1-0.c` and `camss-ispif.c` are back to the baseline.

**Sensor drivers are unchanged.** The ROM keeps the camfix2 builds of imx576_a6l, s5k3t1, hi846, gt9769, CCI and v4l2-cci.
- t35 ran the camfix11 sensor builds. Their only additions are `a6l_regs` (empty by default), `a6l_rd` (register reads) and imx576 `a6l_hts` (0 = unchanged).
- The s5k3t1 camfix11 source with that block stripped rebuilds to exactly the camfix2 srcversion `D3260773…`.
- camera15 runs the ROM (camfix2) sensor builds to prove this on the phone.

## 2. The patch

| file | sha256 |
|---|---|
| `device/hisense/a6l/kernel/camera/patches/camss-sdm660-rom1.patch` (684 lines, 8 files, applied with strip 0 in `drivers/media/platform/qcom`) | `4d6855eba149c1b1f9a9ca9d1518e0ff16304c9201f30f1118b72654914551c7` |
| `…/camss_rom1_patch.py`: provenance. It takes baseline + camfix5 cumulative, restores the diagnostics-only files, strips the rest and adds the clocks. It fails if any diagnostic identifier is left (`ROM1_STRIP PASS`). | `65e9231312d764ab69b8f3f70ace0fa859917d80cb6bef291e70d344a5fc20c4` |
| `device/hisense/a6l/camera/tools/build-camss-rom1.sh`: builds for both kernels and runs the checks | |

**`kernel/rom-v2/series`:**
- The `camss-sdm660-rom1.patch` line replaces the `camss-sdm660-camfix5-cumulative.patch` line.
- The camfix5 line is kept as a comment, and the patch files are kept.

**`tools/check-rom-v2-kernel-series.sh`:**
- The marker changed from `a6l_vfe_min` to `A6L_VFE_MIN_HZ`, and a new marker `a6l_csiphy_stock_clocks` was added.
- New: it builds the series camss with W=1 against v67 and requires 0 warnings and no unresolved imports.
- `A6L_KSERIES_DIR=/tmp/kseries-cam tools/check-rom-v2-kernel-series.sh` → **A6L_KSERIES_PASS** (all 12 patches apply, `qcom-camss.ko (series) a6l_wm`).

## 3. Builds (M= builds; neither kernel out dir nor .config was modified)

| kernel | build | qcom-camss.ko (stripped) | checks |
|---|---|---|---|
| **v67** | `make -C a6l-baseline-7.2 O=out-a6l-phone-v67 ARCH=arm64 LLVM=1 W=1 M=…` | `3574edd628b6609549d4525bc14a31d6b0ef40dd3c47d7f71912c83401bda7d5` | 0 warnings; vermagic `7.2.3-a6l-probe+ SMP preempt mod_unload aarch64`; all 135 undefined symbols in Module.symvers |
| **r5** | `LOCALVERSION=+ make -C a6l-rom-r5-src O=out-a6l-rom-r5 … W=1 M=…` | `cec3e5e983c98fcf52bfacd41151952e9d5dc050a204f43df453eca7bc915a25` | 0 warnings; vermagic `… modversions aarch64`; **all 138 `__versions` CRCs = out-a6l-rom-r5/Module.symvers** (vmlinux 77, videodev 29, vb2-v4l2 15, mc 7, v4l2-async 5, vb2-common 3, vb2-dma-sg 1, v4l2-fwnode 1); the 7 provider modules have the same srcversion as the staged r5 set |

- **Before building against it, the r5 source check confirmed:** the r5 tree's camss with camfix5 reverse-applied is identical to the baseline camss (`R5_SRC_MATCH`).
- **Output:** `firmware/extracted/camera-rom1-20260929/{v67,r5}/qcom-camss.ko` + patch + script + `build-info.txt`, with `SHA256SUMS` 5/5.
- Build dir: `/home/a6l/camss-rom1-132720`.

**Parameters:** only `a6l_wm`. Depends: mc, videodev, v4l2-async, v4l2-fwnode, videobuf2-common, videobuf2-dma-sg, videobuf2-v4l2 (unchanged).

## 4. Staging, DT and module load order

**`tools/stage-rom-v2-prebuilts.sh`:**
- **v67:** it checks `camera-rom1-20260929/SHA256SUMS`, then `setko …/v67/qcom-camss.ko` (OVERRIDE `f9ce7705` → `3574edd6`).
- **r5 (`A6L_KERNEL=r5`):** after the 122-module r5 replacement it copies `…/r5/qcom-camss.ko`.
- All other camera modules are unchanged.

**Dry runs:**
- `A6L_STAGE_DRYRUN=/tmp/stage-cam` → **STAGE_ROM_V2_PREBUILTS_PASS**.
- v67 (`/tmp/stage-cam-v67`): MODULE_ORDER PASS, 122 modules / 489 files, staged camss `3574edd6`.
- `A6L_KERNEL=r5` (`/tmp/stage-cam-r5`): r5 vermagic on all modules, MODULE_ORDER PASS, 122 / 489, staged camss `cec3e5e9` → **STAGE_ROM_V2_PREBUILTS_PASS**.

**DT is the same as the test recovery.**
- The dtbo embedded in `a6l_cam_ovl.ko` equals the bundle `a6l-camera-v75.dtbo` byte for byte, and equals the ROM build of `kernel/a6l-camera-v75.dtso` (sha `4791bb03…`, decompiled DTS identical).
- Applying it with fdtoverlay to the V74 `base.dtb` gives camss and cci nodes identical to `rom-v2.dtb` (`5d186e79…`) after phandle resolution.
- The one textual difference, `rotation = <0x5a>`, is a phandle-map artefact of the value 90.
- The camss `clock-names` in the ROM DT contain `cphy_csid0-3` and `csi0-3`, which the fix needs.

**Load order is the same.**
- `rom/modules/camera.txt` has the same 15 modules in the same order as the bundle `load-order.txt`, minus the flash LED modules, which the ROM loads elsewhere, and minus the overlay, which the ROM does not need.
- The ROM still loads the camera group only with `persist.vendor.a6l.camera=1`. The comment in camera.txt is updated.

**The command line is the same:** the ROM boot cmdline also has `clk_ignore_unused pd_ignore_unused`, like the test recovery.

## 5. Confirm test camera15 (ATTENDED; fresh V75-usb recovery boot; about 3 minutes)

**Bundle:** `firmware/extracted/camera-20260929-rom/`, 24 entries + `extra/`, `SHA256SUMS` 24/24.
- **Laptop:** `~/A6L-usb-20260915/v75/camera15` (camera14 + 6 files). `sha256sum -c` gives **24 OK / 0 bad**.
- **Staging script:** `device/hisense/a6l/camera/tools/stage-camera15-laptop.sh`. **Test script source:** `device/hisense/a6l/camera/run-camera15.sh`.

| file | sha256 |
|---|---|
| qcom-camss.ko (rom1 v67) | `3574edd628b6609549d4525bc14a31d6b0ef40dd3c47d7f71912c83401bda7d5` |
| imx576_a6l.ko / s5k3t1.ko (ROM camfix2 builds) | `ed42784e…` / `98eeceec…` |
| extra/qcom-camss-camfix12.ko (only for `hi846diag`) | `a7c77749…` |
| run-camera.sh | `8aed3b5a0b9cb23383b429aba57dd3200273618b964f2f71d57352f3c7adbc5a` |
| SHA256SUMS | `fa5543be5a094837e00eb3184b141e8ed7dcecd445e7b048c3530b06d1f1e05b` |

**Rows of `MODE=confirm`** (each row writes `A6L_C15_BEGIN/END/DONE` to /dev/kmsg; results in `/tmp/cam15.tab` and `/tmp/cam15.sum`):

| row | sensor | mode | a6l_wm |
|---|---|---|---|
| ibars | imx576 | bars | 6 |
| ilive | imx576 | live | 6 |
| sbars | s5k3t1 | bars | 6 |
| slive | s5k3t1 | live | 6 |
| hbars | hi846 | bars | 6 |
| ibars3 | imx576 | bars | 3 (zero-copy) |
| sbars3 | s5k3t1 | bars | 3 |
| ibars0 | imx576 | bars | 0 (upstream) |

**Diagnostics without module diagnostics:**
- `CLK` is taken 3 s into each capture from debugfs `clk_summary`: `csiphy_clk_src`, `camss_csiphyK`, `cphy_csidK`, `csiK`, `vfe0` rates and enables. This is the fix itself, visible.
- `FRAMES` gives the dequeued count, the VFE sequence numbers, dq_fps and vfe_fps.
- `RAW` gives the size and the distinct byte values of lines 0, H/2 and H-1. A value of 1 means filler.
- `CLOCKFIX_WARN` is printed if a clock is missing or fails.
- `SENSOR_TX_PASS` comes from the sensor frame counter.
- The probe checks `CAMSS_ROM1_PASS`: `a6l_wm` present and no camfix knobs present.

```
A="adb -s HLTE730T-PROBE"; cd ~/A6L-usb-20260915
$A shell 'rm -rf /tmp/camera15'; $A push v75/camera15 /tmp/camera15
$A shell 'T=/system/bin/toybox; $T mkdir -p /tmp/bin; for a in $($T); do $T ln -sf $T /tmp/bin/$a; done; chmod -R 755 /tmp/camera15'
mkdir -p v75/logs/t36; nohup setsid sh -c "adb -s HLTE730T-PROBE shell /tmp/bin/dmesg -w > v75/logs/t36/kmsg-live.txt 2>&1" >/dev/null 2>&1 &
$A shell 'export PATH=/tmp/bin:$PATH; D=/tmp/camera15 MODE=probe sh /tmp/camera15/run-camera.sh 2>&1 | grep -v linker | grep -E "PASS|FAIL|SRCVERSION|CMDLINE|CLK_AT"'
$A shell 'export PATH=/tmp/bin:$PATH; cd /tmp; D=/tmp/camera15 MODE=confirm nohup setsid sh /tmp/camera15/run-camera.sh > /tmp/c15.log 2>&1 < /dev/null & echo STARTED'
# poll (laptop side survives a phone reset):
grep -aE "A6L_C15_(BEGIN|END|DONE|ABORT)" v75/logs/t36/kmsg-live.txt | tail -4
$A shell 'sed -n "/C15_TABLE/,\$p" /tmp/c15.log'; $A shell 'cat /tmp/cam15.sum'
$A shell 'cd /tmp && tar cf c15.tar c15.log cam15.* cam-*-c15-*.out cam-*-c15-*.dmesg cam-*-c15-*.log'; $A pull /tmp/c15.tar v75/logs/t36/
for f in imx576-bars-c15-ibars imx576-live-c15-ilive s5k3t1-bars-c15-sbars s5k3t1-live-c15-slive hi846-bars-c15-hbars imx576-bars-c15-ibars3 s5k3t1-bars-c15-sbars3 imx576-bars-c15-ibars0; do $A pull /tmp/cam-$f.raw v75/logs/t36/ 2>/dev/null; done
python3 v75/camera15/raw10_to_png.py v75/logs/t36/cam-imx576-bars-c15-ibars.raw 2880 2156 3600 RGGB v75/logs/t36/ibars.png   # s5k3t1: 2304 1728 2880 GRBG; hi846: 1632 1224 <bpl from RAW line> GBRG
# optional, LAST, only if hbars fails (loads the camfix12 diagnostic camss; reboot afterwards):
$A shell 'export PATH=/tmp/bin:$PATH; D=/tmp/camera15 MODE=hi846diag sh /tmp/camera15/run-camera.sh 2>&1 | grep -v linker | grep "A6L_CAM"'
$A shell 'export PATH=/tmp/bin:$PATH; D=/tmp/camera15 MODE=off sh /tmp/camera15/run-camera.sh'
```

### PASS criteria and how to read them
- **Probe:** `CAMSS_ROM1_PASS a6l_wm=6`, `SENSOR_ROM_PASS`, the 4 `_PROBE_PASS` lines, `CMDLINE clk_ignore_unused=yes`.
- **Fix active:** each row's `CLK` shows `csiphy_clk_src … rate=200000000` and the row's `camss_cphy_csidK_clk en=1`. For the s5k3t1 rows that is csid**2**, plus `csi2_clk_src rate=310000000`.
- **ibars/sbars/hbars** (PASS), all of:
  - `CAPTURE_…_PASS`
  - `dequeued` = N+1 = 9
  - no `TIMEOUT`
  - `RAW … bytes=` BPL×H
  - line0/mid/last distinct > 4
  - vertical colour bars over the full height in the PNG
- **ilive/slive:** a real image.
- **wm=6 rows:** the VFE seq steps by about 2 (the copy costs about 1 frame, as in t35). **wm=3 rows:** consecutive seq, which is **the zero-copy path the camera HAL needs**. If ibars3 and sbars3 PASS, change the default to `a6l_wm=3` in rom1 (one line) and re-stage.
- **ibars0 (upstream WM):** expected to show whether camfix3/4 is still needed at all. It is informational only.
- **hbars FAIL** (hi846 gave only 2 SOF before the clock fix):
  1. Run `hi846diag`. It gives the CSID STATS/ECC halves, the captured headers, the CSIPHY1 clock tree and the settle 0 (formula) vs 14.
  2. hi846 runs on CSIPHY1 → CSID0 at link frequency 80 or 200 MHz: check `A6L_CSIPHY1 link_freq`, the settle, and that `cphy_csid1` is at 200 MHz.
  3. The next suspects are the 4-lane mode table and the MIPI timing of the `hi846-4lane-default` patch.

## 6. The missing Android camera layer: plan and effort estimate

**Today:** kernel RAW capture works (RDI path, packed RAW10, sensor test patterns and a real exposure). The ROM has **no camera HAL/provider**, so Android sees no camera. The camss path on sdm660 is **RDI only** (the mainline VFE PIX/ISP path for Bayer is not usable), so all image processing must happen in software or on the GPU.

**Recommended: libcamera (simple pipeline handler + SoftISP) + libcamera's Android HAL3 adaptation**

| step | work | effort |
|---|---|---|
| 1 | **Build libcamera for Lineage 24.** Use the meson → Android.bp route that GloDroid uses (aospext), or a prebuilt vendor lib built with the NDK. Ship `libcamera.so`, `libcamera-base.so`, the `ipa_soft_simple` IPA and `camera.libcamera.so` (HAL3 module) in vendor. | 3-5 days |
| 2 | **Pipeline match.** The simple pipeline handler already lists `qcom-camss` (Linaro used it on RB5/X13s with SoftISP). Check graph walking through csiphy → csid → ispif → vfe_rdi0 → video0 on sdm660 (ISPIF is an extra entity: may need a small patch to its supported-device table or entity traversal); formats `SRGGB10P`/`SGRBG10P`/`SGBRG10P` (RAW10 CSI-2 packed, supported by SoftISP). Needs `a6l_wm=3` (zero-copy) proven by camera15. | 2-4 days |
| 3 | **Sensor support in libcamera.** `camera_sensor_properties` entries (unit cell size, test-pattern map) and a `CameraSensorHelper` (analogue gain model, black level) for imx576, s5k3t1 and hi846 (check upstream for an existing hi846 helper). Kernel sensor drivers: add `V4L2_CID_HBLANK` read-only/`VBLANK`/`EXPOSURE` limits consistent per mode, `V4L2_SEL_TGT_CROP_*` selection targets, `V4L2_CID_CAMERA_ORIENTATION`/`ROTATION` via `v4l2_fwnode_device_parse` (the DT already has `rotation`/`orientation`/`lens-focus`), and `link-frequencies` checks. Add smaller/binned modes (e.g. imx576 1440×1078 from the stock res tables) for preview speed. | 1-1.5 weeks |
| 4 | **AE/AWB.** SoftISP IPA provides AGC (exposure + analogue gain), grey-world AWB, BLC, gamma/contrast, and a CCM in recent versions. Write tuning files `simple/imx576.yaml`, `s5k3t1.yaml` and `hi846.yaml` (black level from the sensor, CCM from a colour chart or the stock chromatix, optional LSC). **Performance:** CPU debayer of 6 MP is too slow for smooth preview on 4×Kryo-260. Use the binned preview modes and/or the **GPU (EGL) SoftISP debayer** of recent libcamera, on Adreno 512 with the Mesa freedreno that the ROM already ships. | 1-2 weeks (+ tuning iterations) |
| 5 | **Android integration.** `android.hardware.camera.provider` service wrapping the HAL3 module (the HIDL legacy wrapper may be gone in Android 17: plan a small AIDL provider shim), camera SELinux (vendor_camera domain, `/dev/video*`, `/dev/media*`, `/dev/v4l-subdev*`, debugfs off), ueventd permissions, `media_profiles.xml`, the camera feature XMLs, `persist.vendor.a6l.camera=1` by default once stable. Aperture/Camera2 app for testing; JPEG via the libcamera HAL encoder (libjpeg). | 1-1.5 weeks |
| 6 | **Autofocus (GT9769 VCM)**, `V4L2_CID_FOCUS_ABSOLUTE`, `lens-focus` in DT. SoftISP has no AF yet: start with a fixed hyperfocal position, then add contrast-detect AF (sharpness statistic in the SoftISP stats + hill climb on the lens) and tap-to-focus. | 1-2 weeks |
| 7 | **Flash/torch with adjustable strength (Pierre).** leds-qcom-flash on PM660L is already in the ROM DT (a6l-flash-v75: torch 100 mA, flash 500 mA / 400 ms test limits; stock torch max 500 mA, default 200 mA). Android torch only goes through the camera provider, so: implement `setTorchMode` plus **`turnOnTorchWithStrengthLevel` / `getTorchStrengthLevel`** (AIDL ICameraDevice, Android 13+) and `FLASH_INFO_STRENGTH_MAXIMUM_LEVEL/DEFAULT_LEVEL` in the provider shim. Map level → `/sys/class/leds/<torch>/brightness` (the led class exposes current steps up to `led-max-microamp`). Raise the DT torch max to stock-safe 200-300 mA after a thermal check. Lineage's QS torch strength slider then works. Flash for still capture: libcamera has no flash control on simple; do pre-flash/still flash in the shim first (torch-assisted capture), real flash later. | 3-5 days |
| 8 | **hi846** (second rear camera). Depends on camera15 `hbars`/`hi846diag`. If it streams, it is just another sensor for steps 3-4 (+2 days). If not, one more attended debug round (CSIPHY1, 80/200 MHz link, 4-lane mode). | 2-5 days |

**Total estimate:**
- **MVP** (rear + front preview, still JPEG, AE/AWB, fixed focus, torch on/off with strength): about **4-6 weeks** of agent work plus **6-10 short attended sessions**.
- AF and quality tuning take another 2-3 weeks. Video recording is later (MediaCodec needs a YUV/NV12 path; SoftISP outputs RGB, so a conversion or GPU output is needed).

**Alternatives considered:**
- **(a) Stock Qualcomm mm-camera blobs.** They need the downstream `msm_camera` kernel (msm_isp/msm_sensor ioctls), not mainline camss. Not feasible.
- **(b) A userspace debayer daemon feeding v4l2loopback (YUYV) + AOSP's External (USB) Camera HAL.** Quickest to a working camera app (about 1-2 weeks), but poor quality/latency, no torch-strength API, and no AF integration. Only a stop-gap.
- **(c) A custom HAL3 from scratch.** More work than libcamera for less.

**Risks:**
- Upstream libcamera pace and API changes (pin a release).
- SoftISP throughput on sdm660.
- Missing an AIDL provider path for HAL3 modules in Android 17.
- imx576 quad-Bayer full-resolution remosaic is not planned: use binned 6 MP.

## 7. Files changed or added (no deletes)

**Kernel patch and series:**
- `device/hisense/a6l/kernel/camera/patches/camss-sdm660-rom1.patch`, `camss_rom1_patch.py` (new)
- `device/hisense/a6l/kernel/rom-v2/series`: the rom1 line replaces camfix5, which stays as a comment
- `tools/check-rom-v2-kernel-series.sh`: markers + camss W=1 build

**Staging:**
- `tools/stage-rom-v2-prebuilts.sh`: rom1 camss for v67 and r5
- `device/hisense/a6l/rom/modules/camera.txt`: comment only

**Camera build and test tools:**
- `device/hisense/a6l/camera/tools/build-camss-rom1.sh`, `stage-camera15-laptop.sh`, `device/hisense/a6l/camera/run-camera15.sh` (new)

**Artifacts:**
- `firmware/extracted/camera-rom1-20260929/`, `firmware/extracted/camera-20260929-rom/` (new)
- laptop `v75/camera15`

## 8. Update 29 Sep 2026 (night): hi846 merged (docs/hi846-20260929.md round 3)

- **Test result:** attended t38 (camera17) PASS for the Hi-846. The fix is the s_ctrl return value together with the 2-lane DT; bars, live, 1280x720, 640x480 and the CSID1 route all pass, and the `fix=0` control failed.
- **ROM kernel series:** `camera/patches/hi846-set-ctrl-fix.patch` is added. `hi846-4lane-default.patch` is dropped (the line is commented out, the file is kept).
- **ROM hi846.ko:** built from `firmware/extracted/hi846-rom-20260929/{v67,r5}` and staged by `tools/stage-rom-v2-prebuilts.sh`.
- **ROM DT:** in `kernel/a6l-camera-v75.dtso`, hi846 and csiphy1 are now on 2 lanes, which is the stock wiring.
- **Android HAL:** the Hi-846 is the third camera (ID 2, `aux`), which comes from libcamera patch 0008 plus `camera_hal.yaml` `order`/`aux`. It can never be camera 0 or the torch owner.
- **§6 step 8 (hi846):** done for the kernel/DT/HAL wiring. Tuning is still `uncalibrated`, and the full-resolution 3264x2448 2-lane mode is still open.
