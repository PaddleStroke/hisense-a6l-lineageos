# Upstreaming inventory: what the A6L port could give back

Written 13 Oct 2026. This is a read-only survey: nothing was sent, committed or changed except this file.

**Question:** which parts of this port (camera, ISP, video, kernel, Android) are worth sending to upstream projects, and what work does each one need first?

**Method:**
- Every patch named here was read, at least its header and diffstat.
- The libcamera findings were checked against **upstream libcamera master `a92285e9` (13 Oct 2026)**, cloned temporarily from gitlab.freedesktop.org/camera/libcamera (the clone has since been deleted).
- The hi846, camss, venus and `sdm630.dtsi` findings were checked against **torvalds/linux master**, read from the GitHub raw mirror on 13 Oct 2026.
- Everything else was compared with the local 7.2.3 tree only.

**The kernel base:** the port's kernel is not torvalds. It is **`sdm660-mainline/linux`** at `e47d622cb` (7.2.3), in `/home/a6l/kernel/a6l-baseline-7.2` in WSL. Some drivers patched here exist only in that community tree: `pmi8998_fg`, `qcom_smbx` (not sure whether it has reached mainline), the q6voice series and CPR3. Patches to those drivers go to sdm660-mainline (GitHub), not to LKML.

Priority legend:
- **worth it**: generic, high value, reasonable effort;
- **maybe**: useful, but needs real rework or a hardware test first;
- **not worth it**: A6L-only, diagnostic, or already upstream.

---

## Top 5 recommendations

| # | What | Target | Why | Effort |
|---|---|---|---|---|
| 1 | **hi846: `hi846_set_ctrl()` returns 1, so the sensor never streams** | Linux, linux-media | A 2-line `Fixes:` + `Cc: stable` patch. The Hi-846 never streams on any kernel since v6.5. The Purism Librem 5 rear camera is a Hi-846, so this matters beyond the A6L. The patch has the real author and Signed-off-by. The bug is still in torvalds master (checked 13 Oct). | **Ready.** Follow `docs/upstream/hi846-set-ctrl-HOWTO.md`. |
| 2 | **libcamera Android HAL bug fixes** (repeating requests lose their settings, Android-build fix, sensor orientation, metadata schema) | libcamera, libcamera-devel | Real bugs, all still on master `a92285e9`. They hit every Android device that uses the libcamera HAL (GloDroid, AOSP `external/libcamera` users). The patches are small and easy to review. | Small. Split them out of the A6L patches and write commit messages. |
| 3 | **qcom camss on SDM630/660: get frames at all** (genpd NULL detach, ISPIF never instantiated for 660, CSIPHY clock mux, CSIPHY/CSID clocks, VFE 4.8 write-master MAX address / clock gating) | Linux, linux-media + linux-arm-msm | Mainline camss on SDM660 cannot produce frames today. Every SDM630/636/660 phone (Xiaomi, Sony nile/ganges, BlackBerry, Nokia...) gains a camera path. | Medium. The fixes are proven on hardware, but the A6L module parameters and hard-coded clock floors have to be rewritten into proper resource tables (plus DT binding updates). |
| 4 | **v4l2_codec2 decoder/encoder fixes** (surface-switch freeze, stateful-decoder spec compliance, device discovery, bufferpool crash) | LineageOS Gerrit (`android_external_v4l2_codec2`, which this build uses), then AOSP | 0010 fixes the playback freeze after Home/Recents/lock (ExoPlayer, Gallery), which affects any v4l2_codec2 device on the AIDL Codec2 HAL. 0007 is a V4L2-spec compliance fix. 0001/0002/0005 matter for every SoC where camera and codec both register `/dev/video*`. | Medium. Split 0010, drop the `vendor.a6l.*` properties, add real commit messages. |
| 5 | **`sdm630.dtsi` idle states: the PSCI parameters are rejected by Qualcomm TZ** | Linux, linux-arm-msm (bug report first, patch after the phase-2 test) | Mainline `sdm630.dtsi` (identical in torvalds master) declares `0x40000002` and `0x400000f2/f3/f4`. On the A6L the TZ rejects all of them: about 10 M rejections per CPU overnight, gold cores busy-loop in s2idle, about 349 mA drain, false "hard LOCKUP" and RCU stall reports. Probably every SDM630/636/660 device on Qualcomm's TZ is hit, which would make this the biggest power bug on the platform. | Small to write. Run phase 2 (`dt-psci-idle/`) first, and ask other owners to check their `rejected` counters. |

### What about the hardware ISP?

The most interesting work, technically, is the hardware ISP: the VFE 4.8 PIX Bayer path plus libcamera `DebayerHwIsp`. It is also the **least upstream-ready**, for three reasons:
- **Kernel interface.** The kernel side uses ad-hoc V4L2 controls on `msm_vfe0_pix` and about 15 `a6l_*` module parameters. Upstream ISP drivers (rkisp1, mali-c55) take per-frame parameter and statistics *buffers* through the v4l2-isp extensible-params format; libcamera's `libipa/v4l2_params` already speaks it. Controls are not the accepted interface.
- **libcamera structure.** On the libcamera side, the hardware ISP is a `Debayer` backend inside `SoftwareIsp`, selected by an Android property. Upstream would want either a CAMSS pipeline handler with an ISP, or a hardware-ISP abstraction in the simple pipeline.
- **Statistics.** Statistics still come from the CPU (NV12), not from the VFE statistics engines.

Writing it again for upstream is a large, months-long project. The cheap and valuable step is to **publish the knowledge**:
- an RFC mail to linux-media and linux-arm-msm (Cc the CAMSS maintainers) and to libcamera-devel;
- it would link the register map in `firmware/extracted/hw-isp-20261006/README.md` (sections 1.1-1.4) and the working prototype, and ask whether there is interest.

Priority **maybe (long-term)**. The RFC/announcement is **worth it**.

---

## Before sending anything (applies to every item)

1. **Authorship and DCO.**
   - Most patches carry placeholder authors (`a6l <a6l@local>`, `A6L port maintainers`, `a6l@localhost`) or no header at all.
   - Every upstream (kernel, libcamera, AOSP, LineageOS, freedesktop) needs a real author and a `Signed-off-by:` that only Pierre can give.
   - The one exception is `docs/upstream/hi846-set-ctrl.patch`, which is already correct.
2. **AI-assisted code.**
   - Much of this code was written with AI agents. Check each project's current policy on AI-assisted contributions and disclose as it asks.
   - The bigger point: whoever signs must be able to explain and defend every line in review.
3. **Remove A6L scaffolding.**
   - `a6l_*` module parameters, `persist.vendor.a6l.*` / `vendor.a6l.*` properties, `A6L:` comments, diagnostics and kill switches.
   - Upstream wants the fixed behaviour unconditionally, or keyed on a compatible, hardware version or firmware version.
4. **One logical change per patch**, rebased onto the target's current tree (media.git / linux-next / libcamera master / AOSP or Lineage branch), and `checkpatch.pl --strict` clean for the kernel.
5. **Provenance.**
   - The imx576 and s5k3t1 register tables were replayed from stock Hisense blobs (`libmmcamera_*_hmct.so`), and the CCMs were decoded from the stock chromatix.
   - Say this openly. Maintainers usually accept register sequences taken from a vendor driver, but extraction from a proprietary binary is a grey area. Ask before sending.
6. **Offer to test.** Most maintainers have no SDM660 hardware. Say "Tested-on: Hisense A6L" and offer to test v2s.

Mailing-list mechanics (git send-email, plain text, get_maintainer.pl) are described in `docs/upstream/hi846-set-ctrl-HOWTO.md`. libcamera uses the same model: `libcamera-devel@lists.libcamera.org`, tracked on patchwork.libcamera.org, with DCO sign-off. Its git hosting moved to gitlab.freedesktop.org/camera/libcamera, but review is still on the list.

---

## 1. libcamera

Sources:
- `device/hisense/a6l/camera/libcamera/patches/0001-0029`, which apply on v0.7.2 (see `tools/build-libcamera-a6l.sh`, step `src`);
- the later series under `firmware/extracted/`:
  - `hw-isp-20261006/m2,m3,m4/libcamera/0101-0103`;
  - `camera-round2/3-20261006/patches/0030-0037`;
  - `hw-isp-20261006/m3,m4/libcamera/0038-0039`;
  - `camera-twostream-20261008/0104`;
  - `camera-iq-20261008/core-patch/0105-0119`.

**Upstream changes to plan for:**
- `src/ipa/simple` is now **`src/ipa/softisp`** on master.
- There is a new GPU debayer, `debayer_egl.cpp`.
- A Software ISP buffer-sharing rework is in review (Milan Zamazal, RFC v4, June 2026).
- Every IPA patch therefore needs a real rebase, not just `git am`.

### 1a. Android HAL fixes (`src/android`): **worth it**

| Change | Where | Upstream state (master a92285e9) | Work |
|---|---|---|---|
| **Repeating requests lose their settings.** `processCaptureRequest()` sets `lastSettings_ = {}` inside `if (state_ == State::Stopped)`, after the first request has copied it. Every following request with NULL settings (the camera3 meaning is "same as the last") therefore gets *empty* settings: no control translation, AF state INACTIVE in results. Fix: clear `lastSettings_` in `configureStreams()` instead. | `firmware/extracted/camera-iq-20261008/core-patch/0118-libcamera-a6l-hal-keep-settings-light-preview.patch` (the `lastSettings_` hunks) | **Bug present**: line 1115, `lastSettings_ = {};` in the Stopped branch | Take only the two `lastSettings_` hunks. Drop the `a6lRequests_`/`a6lNullSettings_` counters. Write a commit message that cites the camera3 contract. One small patch. |
| Translate only the controls the camera supports (ScalerCrop, FaceDetectMode, TestPatternMode) instead of setting them always. Otherwise the validator logs one ERROR per request. | same patch 0118, other hunks | Present upstream (unconditional `controls.set`) | Separate patch. Probably welcome. |
| `Thread::setThreadAffinityInternal()` uses `pthread_setaffinity_np()`, which bionic does not have. Use `sched_setaffinity(pthread_gettid_np())` on Android. | `.../patches/0002-base-thread-android-bionic-affinity.patch` | Still `pthread_setaffinity_np` | Trivial build fix for NDK/bionic builds. Send as is with a commit message. |
| `apps/common` lacks the libevent dependency (include path) | `.../patches/0003-apps-common-use-libevent-include-path.patch` | Still `[libcamera_public]` only | Trivial build fix. Check first that it is not an artefact of the static-libevent sysroot. |
| Request sensor-native orientation (rotated sensors fail the strict Adjusted check) | `.../patches/0013-android-request-native-sensor-orientation.patch` | No `config->orientation` set in camera_device.cpp | Good patch with a clear message already. Needs a sign-off. |
| Accept internal-only Adjusted results (bufferCount/stride) instead of returning -EINVAL | `.../patches/0012-android-accept-internal-stream-adjustments.patch` | Still `return -EINVAL` on Adjusted | Expect design discussion: maintainers may prefer the simple pipeline not to adjust. Send as an RFC. |
| Update the camera metadata tag tables to Android 17 (the framework sends tags the bundled schema does not know, and the clone path asserts) | `.../patches/0014-android-update-camera-metadata-schema.patch` (6.6 k lines, generated) | `ANDROID_HEIC` absent upstream | Regenerate from AOSP `system/media`. **Keep the SPDX line, which the patch deletes.** Upstream may want it scripted. |
| Report sensor timing / frame durations from the simple pipeline; keep preview modes below 30 fps | `.../patches/0009-simple-android-frame-duration.patch`, `0010-simple-configure-sensor-timing.patch`, `0011-android-preserve-slow-preview-streams.patch` | Not present | Medium. 0010 touches `soft.mojom` and must be rebased onto `softisp`. |
| Import minigbm plane layout (`cros_gralloc_handle`) with the generic Android platform; copy aligned NV12 | `.../patches/0015-android-import-minigbm-plane-layout.patch` | Not present | Maybe. Useful for GloDroid/AOSP-mainline devices with minigbm. The copied `cros_gralloc_handle.h` needs licence care. |
| Deterministic camera IDs (`order:`) and "aux" cameras in `camera_hal.yaml` | `.../patches/0008-android-a6l-hal-camera-order-aux.patch` | Not present | Maybe. Generic need, but upstream may prefer a different config key. RFC. |
| AF mapping in the HAL (AF mode/trigger/regions to `AfMode`/`AfTrigger`/`AfWindows`, AF state machine in results): tap-to-focus | inside `.../patches/0016-a6l-focus-exposure-capture-completion.patch` and `core-patch/0109-libcamera-a6l-tap-to-focus-core-hal.patch` | Upstream HAL advertises AF modes but maps no AF controls | **Worth it, after 1b.** Useful to any libcamera Android device with AF (rkisp1, RPi). Must be split out of the A6L stats/hw-ISP hunks. |
| Torch/flash | `core-patch/0119-libcamera-a6l-hal-torch-flash.patch` | No flash API in libcamera | Not worth it as is: it drives an A6L sysfs LED from the HAL (`a6l_flash.h`). Upstream first needs flash controls in libcamera core. |
| Single-stream YUV crop, dma-buf allocator system heap first | `.../patches/0005-*`, `0007-dma_buf_allocator-a6l-system-heap-first.patch` | CMA first upstream | Maybe. The system-heap fallback when the CMA heap is absent (typical on Android) is generic, but the "system first" order is a policy choice. |

### 1b. Software ISP / soft IPA features: **worth it as RFC series**

| Change | Where | Generic? | Work |
|---|---|---|---|
| **Contrast-detect autofocus for the soft IPA.** A sharpness statistic in `swstats_cpu`, a simple-pipeline lens hook (`focusLens()`, `V4L2_CID_FOCUS_ABSOLUTE`), and an AF algorithm (Manual/Auto/Continuous, trigger, windows) with settle frames. | `.../patches/0016-*` (AF parts); `core-patch/0108-*-af-robust-continuous.patch`, `0110-*-af-tap-to-focus.patch`; AF-local-rescan `firmware/extracted/camera-round3-20261006/patches/0034-*` | **Yes.** Upstream soft IPA has no AF and the simple pipeline has no lens support. It would help every SoftISP user with a VCM: many Intel IPU6 laptops (dw9714/dw9719 VCMs), Surface Go and phones. | Large. Split into (1) lens plumbing in the simple pipeline, (2) the sharpness statistic, (3) the AF algorithm. Rebase onto `softisp`, remove the A6L DAC ranges (take them from tuning files) and the logs. Post it as an RFC. |
| NV12 output from `debayer_cpu` (Android needs YUV; upstream soft ISP outputs RGB only) | `.../patches/0004-software_isp-debayer_cpu-nv12-output.patch` | Yes | Medium. Coordinate with the EGL debayer and the buffer-sharing rework. Ask on the list first. |
| Sensor helpers + tuning: **Hi-846** (gain = 1 + code/16, black 64, delays) | `.../patches/0006-a6l-hi846-sensor-helper-delays-tuning.patch` | Yes (Librem 5 and others). Upstream has hi846 sensor properties but **no helper** | Small. Send the helper and delays. The tuning yaml is optional (upstream ships `uncalibrated.yaml`). **Worth it.** |
| Sensor helpers + tuning: IMX576, S5K3T1 | `.../patches/0001-a6l-imx576-s5k3t1-sensor-support.patch` | Only once mainline kernel drivers exist (section 2c) | Not worth it yet: libcamera prefers sensors that have upstream drivers. |
| AGC/AWB/adjust tuning changes (bounded AGC, partial grey world, AWB damping, black point/DRC, chroma denoise) | `.../patches/0021-*`; `camera-round2/3-20261006/patches/0030-0037`; `core-patch/0114-*` | Some ideas are generic (AWB damping, AGC convergence) | Maybe, idea by idea and after the `softisp` rename. Several are tuned to this phone. |
| Performance work (FOV raw downsample, workers, mapping cache, native read lifecycle, stage profiles, raw calibration) | `.../patches/0017-0020`, `0022-0029` | Mostly A6L/CPU-specific, with heavy `a6l_*` headers and diagnostics | Not worth it, except that the ideas could inform upstream softISP optimisation discussions. |
| Hardware-ISP backend `DebayerHwIsp` and its follow-ups (shared controls, black point/LUT, AWB stats, sharpen, detail, grain, state timing, light preview, no input sync) | `hw-isp-20261006/m2/libcamera/0101-*`, `m3/libcamera/0102-*`, `core-patch/0105-0107`, `0111-0117` | The concept is generic for CAMSS VFE 4.x; the code is A6L-shaped | See "What about the hardware ISP?" above. Maybe, long-term. |

---

## 2. Linux kernel

Find the lists and maintainers with `scripts/get_maintainer.pl` on the current tree.

### 2a. linux-media: camera sensor and camss

| Item | Where | Generic? | Maturity | Work | Priority |
|---|---|---|---|---|---|
| **hi846 set_ctrl fix** | `docs/upstream/hi846-set-ctrl.patch`, `docs/upstream/hi846-set-ctrl-HOWTO.md` (device copy `device/hisense/a6l/kernel/camera/patches/hi846-set-ctrl-fix.patch`) | Every Hi-846 user | Attended t38 PASS; checkpatch clean; bug still in torvalds master on 13 Oct | Send it. No evidence it has been sent yet. | **worth it, now** |
| **camss: genpd NULL detach on unbind** (`camss_genpd_cleanup()` detaches a NULL `camss->genpd` when every PD is a VFE PD, as on SDM660, so rmmod/unbind oopses) | `firmware/extracted/hw-isp-20261006/patches/0000-camss-genpd-cleanup-null-guard-rom1.patch` | Every SoC whose PDs are all VFE PDs | Code inspection plus the oops; torvalds master still detaches without a check | Rewrite the comment without "A6L". Add `Fixes:`. | **worth it** |
| **camss SDM660: instantiate ISPIF** (`ispif_res_660` exists but 660 is missing from the allocation list, so the VFE never sees SOF) + the CSID parent ops follow-up | `device/hisense/a6l/kernel/camera/patches/camss-sdm660-rom1.patch` (camss.c hunks; history in `camss-sdm660-camfix*-cumulative.patch`, `docs/camfix-20260924.md`) | All SDM630/660 | Attended PASS (t35, round 28 camera); torvalds master still allocates ISPIF only for 8x16/8x39/8x53/8x96 | Extract as its own patch. Check the media graph change against the bindings. | **worth it** |
| **camss SDM660: CSIPHY clock mux ioremap** (CSIPHY2 to CSID1 routing) | same patch, camss-csiphy.c | All SDM660 | PASS | One-line patch | **worth it** |
| **camss SDM660: CSIPHY/CSID digital clocks** (`cphy_csidK` 200 MHz, `csiK` 310 MHz; without them csiphy_clk_src stays at 19.2 MHz and every packet is an ECC error) | same patch (ad-hoc `clk_get` by name + hard-coded rates) | All SDM660 | PASS; `csiK` "not proven needed" (`docs/camera-rom-20260929.md` §1) | Rewrite as clock entries + frequency tables in `csiphy_res_660`. Update the `qcom,sdm660-camss` binding and `sdm630.dtsi` clocks. Prove whether `csiK` is needed. | **worth it (medium)** |
| camss VFE 4.8: write master PING/PONG MAX address (bus errors without it), CGC override, BUFFER_CFG | same patch, camss-vfe-4-8.c (static `a6l_wm_sz[2][8]`, `a6l_wm & 2`) | All VFE 4.8 (SDM660/630) | PASS | Rewrite properly: compute from the buffer, store it per WM, no module parameters | worth it |
| camss: clock floors (CSID/CSIPHY timer ≥ 269/310 MHz, VFE ≥ 404 MHz, otherwise the write-master unified buffer overflows) | same patch (`A6L_*_MIN_HZ`) | Reveals a generic flaw: the rate is computed from the pixel clock and ignores bus/UB bandwidth | Works | Report it. A real fix needs a better rate calculation or table, not floors. | maybe |
| camss: nested VFE PD refcount (ISPIF reset nesting), ICC `vfe-mem` path | same patch | Generic once ISPIF is used | PASS | Rework as a camss refcount; ICC values are made up | maybe |
| camss `a6l_wm` WM copy modes | same patch, camss-vfe-gen1.c | A6L hack | — | — | not worth it |
| **VFE 4.8 PIX Bayer ISP** (CAMIF, demux, BLS, demosaic+WB, CCM, gamma via DMI, chroma enhance, scaler, ABF/BPC) | `firmware/extracted/hw-isp-20261006/patches/0001-0003-*-rom1.patch`, `m3/kernel/0004-camss-vfe48-hw-isp-m4-rom1.patch`, `src-new/camss-vfe-4-8-isp.[ch]` | Concept: all VFE 4.7/4.8 | Phone-tested: all 3 sensors 27-30 fps, in the ROM by default | New uAPI: params/stats buffers instead of controls; add VFE stats engines; ~15 module params to remove | maybe (long-term); **RFC worth it** |
| New sensor drivers IMX576, S5K3T1 | `device/hisense/a6l/kernel/camera/imx576_a6l.c`, `s5k3t1.c` | Other phones use these sensors; no mainline drivers | Work on the phone | Remove debug params (`a6l_regs`, `a6l_rd`, `a6l_hts`, `dbg_fc`), rename `sns_*`, YAML bindings, document register-table provenance (see "Before sending" §5) | maybe |
| GT9769 VCM | `device/hisense/a6l/kernel/camera/gt9769.c` | Same register layout as dw9719 (its own header says so) | Works (AF dac=647) | Add `giantec,gt9769` to `dw9719.c` instead of a new driver; binding update | maybe (small) |
| Venus on SDM660 (HFI 3xx, VIDEO.VE.4.4) | `firmware/extracted/venus-impl-20261006/kmod/0001-*` to `0015-*` (cumulative: 0015 has everything, 1.7 k lines, 11 files) | **All SDM660 boards.** Mainline has `qcom,sdm660-venus`, but the firmware reports no per-codec capabilities, so every format clamps to 0x0 and no session starts. Some fixes may apply to every HFI 3xx SoC. | Encoder and decoder in the ROM (rounds 13e-27), Pierre-tested | Split into logical patches: (1) fill missing caps from the v4 defaults (`0001` is already a clean, well-described patch); (2) HFI 3xx boot-status check (CTRL_INIT/ARG0 stale); (3) cold power collapse (warm PC broken on this firmware); (4) shared video subcore power; (5) EOS with a real buffer; (6) `bytesused` vs `data_offset`; (7) H264 transform 8x8 packing; (8) unknown-size / crop handling. Drop the ~15 `a6l_*` params and traces. Each needs a justification the venus maintainers can check against downstream. | **worth it (start with 0001 and the obvious ones), rest maybe** |

### 2b. linux-arm-msm: SoC DT, clocks, pinctrl

| Item | Where | Generic? | Maturity | Work | Priority |
|---|---|---|---|---|---|
| **`sdm630.dtsi` idle-state PSCI parameters** (`0x40000002`, `0x400000f2/f3/f4`) rejected by the TZ. Stock uses the standard Qualcomm platform-coordinated encoding (CPU retention `0x00000002`, CPU PC `0x40000003`, L2 `0x41000023/33/43`, system PC `0x42000343`). | `firmware/extracted/battery-20261012/README.md` (the measurement table, about line 92); phase-2 overlay `firmware/extracted/battery-20261012/dt-psci-idle/a6l-idle-states-psci.dtso`; shipped workaround `firmware/extracted/battery-20261012/fix-cpuidle/0001-device-a6l-cpuidle-pc-only.patch` | Probably every SDM630/636/660 on Qualcomm TZ: the values in torvalds master are identical, and the TZ encoding is the standard one. **Needs a confirmation from another device.** | Rejections measured (only WFI and `0x40000003` work); the userspace workaround is live-tested; the DT fix (phase 2) is **not tested** | (1) Run phase 2. (2) Mail linux-arm-msm and the sdm660-mainline chat with the `rejected` counters and ask Sony nile/ganges and Xiaomi owners to run `cat /sys/devices/system/cpu/cpu*/cpuidle/state*/rejected`. (3) Then a `sdm630.dtsi` patch with stock IDs, or remove the broken states. Also worth mentioning: an always-rejected deepest state makes s2idle spin (cpuidle core behaviour). | **worth it** |
| **MMSS NoC AHB (`MNOC_AHB_CLK`) dependency of MMSS peripherals.** CCI needs MNOC_AHB/BIMC_SMMU clocks first, otherwise async suspend races the SMMU gating and the I2C bus hangs, leading to a watchdog reset. MDSS warns "mdss_ahb_clk status stuck at 'on'" on every suspend for the same reason. | CCI: `firmware/extracted/pm-async-race-20261006/patches/0001-a6l-camera-v75-dtso-cci-mmss-noc-clocks.patch`, alternative `0003-ALT-mmcc-sdm660-mnoc-ahb-critical.patch`, `docs/sleep-async-workaround-20261006.md`. MDSS: `firmware/extracted/mdss-ahb-20261007/msm-mdss-mnoc-ahb.patch` + README | All SDM630/660 | CCI fix attended 6/6 + 3/3; the MDSS fix was **not installed** | One root cause, two symptoms. Discuss on linux-arm-msm whether the right fix is the mmcc driver (critical/parent clock) or DT clocks per consumer. Needs i2c-cci / mdss binding changes for the DT route. | **worth it (report + patch)** |
| LPASS LPI pinctrl: TERT MI2S functions on gpio4-7 | `device/hisense/a6l/kernel/a6l-lpi-ter-mi2s-v74.patch` | SoC data for every SDM660 with a TERT-MI2S amplifier | Speaker works through it | Small. Check the function numbers against the downstream DT. | **worth it** |
| LPASS LPI pinctrl: pads lost across system suspend (LPASS power collapse), restored from a shadow on resume | `firmware/extracted/audio-silent-20261008/fix/a6l-lpi-resume-restore.patch` + README | Likely every SDM660 LPI user | Pierre-verified (round 16) | Report first: maintainers may prefer a different mechanism, or the cause may be the AFE port left running (see the sm8250 item below). | maybe |
| `sm8250.c` machine driver holds a wakeup source while a DSP back end runs | `firmware/extracted/audio-silent-20261008/fix/a6l-sm8250-be-wakeup-source.patch` | Policy workaround | Works | Not acceptable as is (module param, policy). Use it as evidence in the LPI report. | not worth it |
| CPR3/CPRh + OSM for SDM660 (CPU DVFS) | `device/hisense/a6l/kernel/cpr-sdm660/r5/0001-pmdomain-qcom-cpr3-cprh-sdm660.patch`, `0002-cpufreq-qcom-hw-cprh-osm-sdm660.patch` (+ generators `gen_sdm660_cpr.py` etc.) | SDM630/660. Built on SoMainline's never-merged cpr3 series. | Open loop; both clusters reach top frequency | Contribute the SDM660 data/OSM to whoever carries cpr3 (SoMainline / sdm660-mainline). The "placeholder" scale values taken from sdm630 must be justified. | maybe |
| OPP debugfs: skip an existing link; cpufreq-dt-platdev blocklist for sdm630/660 | `device/hisense/a6l/kernel/cpr-sdm660/r5/0003-opp-debugfs-skip-existing-link.patch`, `0004-cpufreq-dt-platdev-blocklist-sdm630-sdm660.patch` | Generic small fixes. 0004 only matters once a qcom-cpufreq-hw driver exists for 660. | Works | Send 0003 alone (linux-pm). Send 0004 with the CPR/OSM series. | 0003 maybe-worth, 0004 with CPR |
| `pinctrl`/`q6asm-dai` `of_xlate_dai_name` by DAI id | `device/hisense/a6l/kernel/a6l-q6asm-dai-xlate-v75.patch` | Small robustness fix. Only triggered by fdtoverlay reordering children. | Phone-tested | Check that mainline really resolves by child order; send if so | maybe |
| **Board DTS `sdm660-hisense-a6l.dts`** | today: bring-up `device/hisense/a6l/kernel/sdm660-hisense-a6l-probe.dts` + about 40 `*.dtso` overlays composed by `tools/build-rom-v2-dt.sh` onto `firmware/extracted/recovery-v74-candidate-20260923/base.dtb` | The classic contribution | All hardware works through the overlays | Fold everything into one DTS. Drop `clk_ignore_unused`/`pd_ignore_unused`/`regulator_ignore_unused`. Keep only nodes whose drivers and bindings are upstream (display can start as simple-framebuffer). Add the `hisense` vendor prefix if missing and a board compatible (today `hisense,hlte730t`). Fix the binding issues listed in section 4. Go to sdm660-mainline first, then mainline once the camss/panel pieces exist. | **worth it (sdm660-mainline first)**, mainline maybe |

### 2c. Other kernel subsystems

| Item | Where | Target | Generic? | Work | Priority |
|---|---|---|---|---|---|
| **drm/msm a5xx: `hlsqregs` freed without being NULLed (double free/UAF) + 1 MiB BO leak on allocation failure** | `firmware/extracted/gpu-recovery-candidate-20261002-170137/a5xx-hlsq-cleanup.patch` | dri-devel + linux-arm-msm (freedreno) | All a5xx | 22 lines. Verify against drm-msm-next, add `Fixes:`, send. | **worth it** |
| drm/msm a5xx: fault "block" string is stack-allocated but kept in crashstate (use-after-return, reproduced with ASan) | `device/hisense/a6l/kernel/rom-v2/drm-msm-owned-fault-source.patch`, `docs/gpu-investigation-20261002.md` | same | All a5xx | Check that current upstream still passes a stack buffer; rewrite as a clean fix | worth it |
| drm/msm a5xx: stale CP ring pointers after suspend make the CP replay old ring words (RBBM soft reset at hw_init start) | `firmware/extracted/pm-logging-20261005/gpu-cp-reset-fix-20261006/gpu-cp-reset.patch`, `docs/gpu-cp-reset-fix-20261006.md` | same | A512, maybe all a5xx | A controlled test passed, but one real-use crash is still unexplained. Report as a bug with the evidence (and the `msm_gpu_submit` ignored `hw_init` return) rather than a patch. | maybe |
| DRM CRTC fence BUG_ON | `device/hisense/a6l/kernel/rom-v2/drm-crtc-fence-signal-race.patch` | — | Backport of upstream 57acb869490b | Nothing to send | not worth it |
| drm/msm DPU: fixed planes on legacy CTLs, stale CTL flush warning | `firmware/extracted/eink-round6-20261007/kernel/0001-drm-msm-dpu-fixed-planes-on-legacy-CTLs-stale-CTL-flush-warning.patch` | dri-devel | SDM660 DPU (legacy CTL) with two displays | Check whether the behaviour exists upstream. Possibly a report rather than a patch. | maybe |
| ath10k WCN3990 HL1.0 (WLAN.HL.1.0.1.c2-00681): the firmware crashes (whole MSS restarts) on every disconnect, on the TXBF vdev param sent after VDEV_DOWN | `firmware/extracted/wifi-20261008/0005-ath10k-wcn3990-disassoc-quirks.patch` (supersedes 0004) + README | linux-wireless / ath10k | All SDM660/630 WCN3990 boards with this firmware family | Bisected with a debug trace, live A/B. Replace the module param with a firmware-version/hw quirk, or simply reorder (TXBF before VDEV_DOWN, bit 2) if that also works. Send with the trace. | **worth it (as a bug report + patch)** |
| regulator tps65185: clear latched faults (INT1/INT2 read) before power-up, retry once on PG timeout (an e-ink frozen after an empty-battery boot was a latched UVLO) | `firmware/extracted/eink-tps-20261013/tps65185-clear-latched-faults.patch` + README; device copy `device/hisense/a6l/kernel/a6l-tps65185-0001-clear-latched-faults.patch` | regulator list (Mark Brown) + Andreas Kemnade (driver author) | Any IRQ-less TPS65185 board (e-readers) | Shipped in round 29+ (ko 0cc74f3b); host-tested. Rebase onto mainline `drivers/regulator/tps65185.c` (the A6L copy is a fork). | maybe |
| input edt-ft5x06: FT8719 TDDI behaviour (panel follower, no I2C while the LCD is off, lift contacts) | `device/hisense/a6l/kernel/a6l-edt-ft5x06-ft8719-tddi.patch` | linux-input | TDDI users of edt-ft5x06 | Hacky (`a6l_tddi`, `a6l_panel` with a hard-coded compatible lookup). The upstream form is `drm_panel_follower` through a DT `panel` phandle, as i2c-hid does. | maybe |
| Panel: FT8719 Tianma 1080x2340 | `device/hisense/a6l/kernel/panels/panel-ft8719-tianma-1080x2340.c` (+ `.dtsi`) | dri-devel (drm-misc) | Panel used in other phones? Unknown. Mainline has `panel-ebbg-ft8719.c` with a very similar init. | Generator output with FIXMEs. Remove `skip_init`/`restart_keep_awake` and the reboot notifier, add regulators, fix the compatible (`mdss,` is not a vendor prefix). Consider making it a variant of the ebbg-ft8719 driver. | maybe (with the DTS) |
| Panels: e-ink DSI (`panel-a6l-epd*.c`, `panel-epd-eink.c`), `a6l-clk-hold.c`, `a6l-reg-hold.c` | `device/hisense/a6l/kernel/panels/` | — | A6L only | The DSI one reprograms the PHY/INTF from the panel driver; the others are bring-up experiments | not worth it |
| stk3310: STK3338 ID + vdd/vio regulators | `device/hisense/a6l/kernel/stk3338/stk3338_a6l.c` | linux-iio | Generic part only | Split out "add STK3338 + regulators" from the A6L address search and presets | maybe (small) |
| TMD3702 ALS/prox driver | `device/hisense/a6l/kernel/tmd3702/` | linux-iio | No upstream driver | Minimal polled driver with A/B params; substantial work | not worth it (for now) |
| GPIO vibrator, PM660 haptics fork | `device/hisense/a6l/kernel/a6l-gpio-vibrator`, `a6l-pm660-haptics` | — | `gpio-vibra.c` already covers it; PM660 haptics unused by stock | Possibly the small `mutex_init(play_lock)` fix in the haptics driver (wherever that driver lives) | not worth it |
| TFA98xx (out-of-tree NXP driver port) | `device/hisense/a6l/kernel/tfa98xx-a6l`, `a6l-tfa98xx-000{2,4,5}-*.patch` | — | Out-of-tree, no mainline TFA9894 driver | — | not worth it |
| USB diagnostics (manual connect, dwc3 event trace), `a6l_keep_boot_domains`, earlycon, simplefb | `device/hisense/a6l/kernel/a6l-manual-usb-connect.patch`, `a6l-usb-event-trace.patch`, `a6l_earlycon.c`, `a6l_simplefb.c` | — | Bring-up diagnostics | — | not worth it |

---

## 3. sdm660-mainline and related community trees (GitHub PRs / chat)

These drivers are not in torvalds; their maintainers are the sdm660-mainline, msm8916-mainline and msm8953-mainline communities. This is also where the board DTS should land first.

| Item | Where | Generic? | Maturity | Work | Priority |
|---|---|---|---|---|---|
| **pmi8998_fg: charger status never resolved** (`power_supply_get_by_reference()` returns NULL before qcom_smbx registers, with no -EPROBE_DEFER) + **inverted fallback sign** (treats current < 0 as charging) + notifier error path / no unregister (UAF on rmmod) | `firmware/extracted/bms-20261009/0001-pmi8998_fg-live-charger-status-and-fallback-sign.patch` + README | Every pmi8998/pm660 user of the driver | Charging status OK (Pierre) | Replace the hard-coded `"pm660-charger"` name lookup with -EPROBE_DEFER or a devm notifier; split into 2-3 patches | **worth it** |
| qcom_smbx: HVDCP / QC2-QC3 5-9 V | `device/hisense/a6l/kernel/power/smbx/qcom_smbx-a6l-hvdcp.patch` (958 lines) | Generic feature for SMB2 chargers | Attended QC PASS at 9.06 V | Rewrite the module params/sysfs knobs (`hvdcp_enable`, `hvdcp_regs`, `hvdcp_rerun`) as PSY properties (`usb_type`, voltage); split | maybe |
| qcom_smbx: FCC max / JEITA / float clamp | `device/hisense/a6l/kernel/power/smbx/qcom_smbx-a6l-fcc-jeita.patch` | Should be DT `simple-battery` properties | Works | Rewrite against `power_supply_battery_info` | maybe |
| q6voice additions: RX volume, TX mute, **ADSP SSR use-after-free during a call** | `device/hisense/a6l/kernel/kvoice/a6l-q6voice-{rx-volume,tx-mute,ssr}-v75.patch`, `a6l-q6voice-sdm660-v75.patch` | Generic for the q6voice series (Stephan Gerhold et al.) | Volume/mute attended; SSR fix host-tested | Send to the q6voice series maintainers (msm8916-mainline). The VoLTE session param (`a6l-q6mvm-volte-session-v75.patch`) is a hack. | worth it (SSR), maybe (rest) |
| q6routing per-direction session tables (ADM EBADPARAM when one frontend routes RX and TX) | `device/hisense/a6l/kernel/audfix/a6l-q6routing-per-direction-v75.patch` | q6routing **is mainline**: check against torvalds, and if the bug is there, send to alsa-devel / linux-arm-msm | Attended PASS | Rebase and check | worth it (if present in mainline) |
| q6adm `endpoint_id_2 = 0xFFFF` like stock | `device/hisense/a6l/kernel/audfix/a6l-q6adm-endpoint2-v75.patch` | Mainline q6adm | Partly tested | Justify against downstream | maybe |
| msm8916-wcd-analog (PM660L) MBHC headset-mic detection | `device/hisense/a6l/kernel/audfix/a6l-wcd-analog-mbhc-mic2-v75.patch` | Mainline codec; upstream only detects a headset mic after a BTN0 press | Attended PASS | Remove the module param/polling, rework | maybe |
| In-call record/playback AFE pseudo ports | `device/hisense/a6l/kernel/kvoice/a6l-btcall-incall-v75.patch` | Changes dt-binding port IDs | Not phone-tested | Discuss first | not worth it (yet) |
| ipa2-lite: SDM660 (IPA v2.6L) data + QMI filter-rule replies | `device/hisense/a6l/kernel/ipa/ipa2-lite/a6l-ipa2-lite-*.diff` (upstream copy `ipa2-lite/upstream-7.1.3/`) | SDM660/630 mobile data on mainline | Attended (data call, DNS, HTTP) | Send to msm8953-mainline (Vladimir Lypak's ipa2-lite). New compatible `qcom,sdm660-ipa-lite` needs agreement. | maybe |
| btqca: MSFT extension off (`enable_msft`) | `device/hisense/a6l/kernel/bluetooth/a6l-btqca-no-msft.patch` | Module param form not acceptable | On-phone check pending | Report the malformed MSFT event with the firmware version (crbtfw21.tlv) to linux-bluetooth instead | maybe (report) |

---

## 4. DT binding issues to fix before any DTS goes upstream

These came out of the overlay audit:
- **Wrong form:**
  - `mdss,ft8719-tianma-1080x2340`: `mdss` is not a vendor prefix;
  - `epd,eink` and `eink,ed052tc2`: not real vendors;
  - `qcom,apss-wdt-sdm660`: should follow the `qcom,apss-wdt-<soc>` order used in the binding (check `qcom,wdt.yaml`).
- **Downstream:** `timed-gpio`.
- **Need a new binding plus driver:**
  - `hisense,a6l-gpio-vibrator` (use `gpio-vibrator` instead);
  - `sensortek,stk3338` (add it to stk3310);
  - `giantec,gt9769` (add it to dw9719);
  - `samsung,s5k3t1`, `sony,imx576`;
  - `qcom,sdm660-ipa-lite`, `qcom,sdm660-cprh`, `qcom,sdm660-cpufreq-hw`.
- **Check against the existing bindings:** `ti,tps65185`, the `nxp,tfa98xx` downstream binding, and the camss `qcom,sdm660-camss` clocks (section 2a).

---

## 5. Android: AOSP, LineageOS, freedesktop

Channels:
- **LineageOS Gerrit** (review.lineageos.org) for the projects this build takes from LineageOS: `external/v4l2_codec2` (`LineageOS/android_external_v4l2_codec2`, branch lineage-24.0) and `packages/apps/Aperture`. This is the shortest path.
- **AOSP Gerrit** (android-review.googlesource.com) for `tinyalsa_new`, `hardware/interfaces`, `frameworks/*`. This needs a Google CLA. Google now develops most of AOSP internally and publishes periodic source drops, so external CLs can take a long time.
- **freedesktop GitLab** for `drm_hwcomposer` (gitlab.freedesktop.org/drm-hwcomposer).

### 5a. external/v4l2_codec2: **worth it**

The patches are bare diffs: they need commit messages and real authorship. All are in `device/hisense/a6l/rom/android/patches/external/v4l2_codec2/`.

| Patch | What | Generic? | Tested | Priority |
|---|---|---|---|---|
| `0010-a6l-venus-decoder-surface-switch-slots.patch` (430 lines) | With the AIDL Codec2 client, `setOutputSurface()` does not migrate buffers. The patch: buffer identity by dmabuf inode; per-pool retry state (the function-`static` state shared by all pools in the process is a real bug); retry of transient fetch errors (BR_FROZEN_REPLY) instead of wedging; no livelock on unknown ids; spare CAPTURE slots; safe slot hand-over | Any v4l2_codec2 device with the AIDL HAL (playback freeze after Home/Recents/lock) | Pierre, 12 Oct: Gallery freeze gone (round 27) | **worth it.** Split into (b) per-pool state, (c) transient retry, (d) unknown-id livelock, (a)/(e)/(f) slot management; drop `vendor.a6l.c2dec.spare_capture` or make it a sane default |
| `0007-a6l-venus-decoder-no-initial-output-before-headers.patch` | Handles drivers that refuse CAPTURE G_FMT before headers (allowed by the V4L2 stateful decoder spec) using the SOURCE_CHANGE flow; fixes drain with no key frame | Spec compliance, every stateful decoder | b11d ACCEPT | **worth it** |
| `0009-a6l-venus-decoder-bytebuffer-bufferpool.patch` | ByteBuffer mode on recycled bufferpool2 buffers; fail instead of crashing on a basic block with no pool data | The crash fix is generic | b11d | worth it (crash part) |
| `0001-*-discover-encoder-by-driver-name.patch`, `0002-*-advertise-encoder-only-with-device.patch`, `0005-*-advertise-decoders-only-with-device.patch` | Pick `/dev/video*` by driver name (never open camera nodes); advertise only codecs whose device exists | SoCs where CAMSS and the codec both register video nodes | Stage B, rounds 13e+ | worth it |
| `0006-a6l-venus-decoder-visible-rect-from-sps.patch` | SPS crop parser when the driver reports no crop (1920x1088) | Generic fallback; arguably a venus kernel bug (see the venus `0014` crop work) | MD5-checked | maybe |
| `0008-a6l-venus-decoder-input-mmap-merge-csd.patch`, `0003-a6l-venus-mmap-coded-output.patch` | MMAP bitstream input/coded output with copy; merge CSD-only buffers | Venus HFI 3xx needs; generic as opt-in | Yes | maybe |
| `0004-a6l-venus-encoder-defaults-high-vbr-idr.patch` | Split MB_RC_ENABLE so an unsupported control does not drop the GOP (a bug); High-profile default (policy) | The split is generic | Yes | maybe (the split only) |

Also worth documenting for other integrators, in a README or a mailing-list post rather than code: `media.c2.hal.selection=aidl` is required to see the v4l2 store (`device/hisense/a6l/video/venus/init.a6l.codec2-selection.rc`).

### 5b. Other AOSP / Lineage items

| Item | Where | Target | Generic? | Priority |
|---|---|---|---|---|
| **tinyalsa `pcm_set_config` uses the requested, not negotiated, sizes for buffer_size/avail_min/start/stop thresholds** (a 1440x4 negotiated ring kept stop=4096, leading to XRUNs and premature stops) | `device/hisense/a6l/rom/android/patches/external/tinyalsa_new/0001-pcm-negotiated-defaults.patch` | AOSP `external/tinyalsa_new` + github.com/tinyalsa/tinyalsa | Every tinyalsa user whose driver rounds the period/buffer | **worth it.** Check upstream master first. |
| Bluetooth HAL transport: returns after `initializationComplete(UNABLE_TO_OPEN_INTERFACE)` (FD_SET(-1) FORTIFY abort); waits for PF_BLUETOOTH when BT is a module | `device/hisense/a6l/rom/android/patches/hardware/interfaces/0001-a6l-bluetooth-transport-startup.patch` | AOSP `hardware/interfaces` | The abort fix is generic; the module wait is generic for mainline-kernel devices | worth it (abort fix), maybe (wait) |
| Audio AIDL example HAL: `populateConnectedDevicePort` for analog wired headset (the default returns EX_ILLEGAL_STATE, so the jack is dropped) | `device/hisense/a6l/audio/patches/0006-a6l-wired-headset-connection.patch` | AOSP `hardware/interfaces/audio/aidl/default` | Real gap for any device using the example HAL with ALSA | maybe |
| Audio example HAL: capture without MonoPipe's synthetic clock regulator | `device/hisense/a6l/audio/patches/0005-a6l-native-primary-capture-buffering.patch`, `rom/android/patches/hardware/interfaces/0022-a6l-native-capture-backpressure.patch` | same | Arguable | maybe |
| Audio HAL 0001-0004, 0007 (capture PCM property, call route, voice volume, BT call bridge, MM1 back end) | `device/hisense/a6l/audio/patches/` | — | Device-specific | not worth it |
| WebViewZygote NPE when the child zygote fails to start | `device/hisense/a6l/rom/android/patches/frameworks/base/0001-webview-handle-child-zygote-start-failure.patch` | AOSP `frameworks/base` | Generic (seen on slow devices) | maybe |
| UiMode: respect an explicit day overlay in battery saver | `.../frameworks/base/0002-uimode-respect-day-overlay-in-battery-saver.patch` | AOSP | Policy | maybe (low) |
| NEON RGB to I420, bit-exact with the scalar path | `.../frameworks/av/0003-a6l-exact-rgb-i420-neon.patch` | AOSP `frameworks/av` (Codec2BufferUtils) | aarch64 performance | maybe (needs benchmark + CTS) |
| AVC stage profilers | `.../frameworks/av/0001-*`, `0002-*` | — | Diagnostics | not worth it |
| Aperture: Auto FPS clears the explicit FPS request | `.../packages/apps/Aperture/0001-a6l-allow-auto-video-fps.patch` | LineageOS Gerrit | Probably generic | maybe |
| Aperture 0002-0004 (native JPEG preference, capability gate, HD preview ratio) | `.../packages/apps/Aperture/0002-0004` | LineageOS Gerrit | Semi-generic for libcamera-HAL devices | maybe |
| Aperture 0005 user-facing mirror, Launcher3, Settings, frameworks/base 0002-0006 e-ink patches | `.../packages/apps/{Aperture/0005,Launcher3,Settings}`, `.../frameworks/base/000{2..6}-a6l-*` | — | E-ink/A6L | not worth it |
| Camera provider: torch strength levels | `device/hisense/a6l/camera/provider/` (fork of the LineageOS AIDL provider, `A6lTorch`) | LineageOS (`hardware/lineage`/camera provider) | Generic idea; the sysfs LED path is device data | maybe |
| Orange FR APN IPV4V6 | `.../vendor/apn/0001-FR-Orange-World-IPV4V6.patch` | LineageOS `android_vendor_apn` | Generic | maybe (only proven with IPv4) |

### 5c. drm_hwcomposer (freedesktop GitLab)

| Item | Where | Generic? | Priority |
|---|---|---|---|
| Ignore connectors + hand a DRM lease of them to a trusted client over a socket | `device/hisense/a6l/eink/patches/external/drm_hwcomposer/0001-drm_hwcomposer-ignore-and-lease-connectors.patch` | Generic feature (auxiliary panels, VR, kiosk side displays) | maybe (needs a cleaner API than properties; discuss on the GitLab issue tracker first) |
| Blank an independent backlight at power-off | `device/hisense/a6l/rom/android/patches/external/drm_hwcomposer/0003-a6l-display-power-backlight.patch` | Semi-generic | maybe |
| Keep the CRTC's resources across power-off; detach planes before the power-off commit | `.../drm_hwcomposer/0004-*`, `0005-*` | Workarounds for msm DPU resource-manager behaviour | not worth it as hwc patches; report the DPU behaviour to dri-devel instead |
| 0002 dual-screen hold, 0006 GPU compose on e-ink | `eink/switcher/patches/.../0002-*`, `rom/android/patches/external/drm_hwcomposer/0006-*` | A6L | not worth it |

### 5d. Mesa / freedreno (Adreno 512)

Nothing new to send. The a5xx blit/stencil fixes that were tested (66d34e1a9f83, eca654871ed0, 1453845c317b) are upstream backports, and no new Mesa bug was found (`docs/gpu-investigation-20261002.md`). The Adreno 512 findings are kernel-side (section 2c).

### 5e. Standalone HALs that could interest other mainline-Android projects

These are complete HALs written for a mainline Qualcomm kernel, Apache-2.0 with SPDX headers. They do not belong in a single upstream, but they could be published as reusable projects, for example offered to GloDroid, the Linaro AOSP device trees (db845c / RB5) or anyone doing Android on mainline Qualcomm:

| HAL | Where | Notes |
|---|---|---|
| **RIL over QMI/QRTR + rmnet** (AIDL radio on AOSP `libminradio`; own QMI client for DMS/UIM/NAS/WMS/VOICE/WDS/WDA, SMS, USSD, IMS) | `device/hisense/a6l/radio/` (`qmi/src`, `hal`, `minradio`) | The most interesting one: there is no public AIDL RIL for mainline Qualcomm modems that I know of. Strip `volte4/` (PDC/EFS NV patching, device-specific). **worth publishing.** |
| GNSS over QMI LOC on AF_QIPCRTR | `device/hisense/a6l/gnss/` | Generic for mainline Qualcomm modems; has host tests. worth publishing. |
| Wi-Fi legacy vendor HAL for mainline ath10k/nl80211 (COUNTRY via NL80211_CMD_REQ_SET_REG, MAC) | `device/hisense/a6l/wifi/` (`hal`, `driver_cmd_a6l.c`) | Generic for any nl80211 device. maybe. |
| USB gadget AIDL on configfs; MAC/BT-address helper | `device/hisense/a6l/usb/`, `device/hisense/a6l/hals/macs/` | Small; comparable tools exist. maybe. |

---

## 6. Device support: LineageOS, postmarketOS, public tree

**LineageOS official support: not realistic.**
- The product inherits `lineage_gsi_arm64.mk` and `generic_arm64/BoardConfig.mk`, with `TARGET_NO_KERNEL`/`TARGET_NO_RECOVERY`.
- The kernel, modules, Mesa and firmware are staged prebuilts (`device/hisense/a6l/rom/rom.mk`), and some camera libraries are tracked prebuilts.
- LineageOS official devices need a native board config, a kernel built from source in the tree, and `lineage_<device>.mk` conventions, plus a committed maintainer.
- Other gaps today: no /data encryption, the open stability (SMPL/HRST resets), and the e-ink stack depends on the stock proprietary `libtcon_eink.so`.
- Fixing the build-system part would be a large restructuring for a single, rare device.

**Realistic options:**
1. **Public unofficial tree** (the repo already has the remote `github.com/PaddleStroke/hisense-a6l-lineageos`; check whether it is public).
   - Add a LICENSE, update the stale `device/hisense/a6l/README.md` (it still describes a "compile probe"), and add install instructions.
   - Then announce it on XDA or a similar forum.
   - Make sure no stock blobs or signing keys are tracked: `ipa-priv-key.pem` and the release keys must stay out. `firmware/`, `logs/` and the e-ink `proprietary/` directory are already gitignored. The tracked binaries `camera/libcamera/prebuilt/*.so` and `tools/volte4-pdc/gmf-fix/libgmffix.so` need a licence review.
2. **postmarketOS device port** (gitlab.postmarketos.org, pmaports MR, "testing" category).
   - SDM660 devices there already use the sdm660-mainline kernel (`linux-postmarketos-qcom-sdm660`).
   - An A6L port needs the board DTS in sdm660-mainline (section 2b) plus `device-hisense-a6l`. Phone, LCD, Wi-Fi, modem and audio would come mostly from the same kernel work.
   - The e-ink would need its own Linux userspace; the current e-ink daemon is Android-specific and blob-based.
   - This is the natural home for the Linux-side bring-up knowledge.
3. **sdm660-mainline wiki/chat**: share the cpuidle, camss, venus and MMSS NoC findings even before patches. Many owners of other SDM660 phones would benefit right away.

---

## 7. Suggested order of work

1. **This week (no rework needed):**
   - send the hi846 patch;
   - post the `sdm630.dtsi` cpuidle finding as a bug report with the counters (linux-arm-msm + sdm660-mainline);
   - send the a5xx `hlsqregs` fix.
2. **Small series:**
   - libcamera HAL fixes: `lastSettings_`, bionic affinity, orientation, controls-only-if-supported;
   - camss genpd NULL guard + ISPIF-for-660 + clk_mux;
   - LPI TERT MI2S functions;
   - pmi8998_fg fixes to sdm660-mainline;
   - the ath10k WCN3990 disconnect crash (report + patch).
3. **Medium:**
   - camss CSIPHY clocks / VFE 4.8 write master rewritten properly;
   - Venus HFI 3xx series starting with 0001;
   - v4l2_codec2 series to LineageOS Gerrit;
   - tinyalsa;
   - MMSS NoC AHB discussion;
   - phase-2 cpuidle test, then the DT patch.
4. **Long-term / RFC:**
   - soft IPA autofocus + lens plumbing + HAL AF mapping (libcamera);
   - VFE 4.8 PIX hardware ISP (kernel uAPI + libcamera architecture);
   - board DTS (sdm660-mainline, then mainline);
   - sensor drivers;
   - postmarketOS port;
   - publish the QMI RIL/GNSS HALs.
