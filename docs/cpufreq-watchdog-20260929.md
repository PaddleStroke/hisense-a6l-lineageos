# cpufreq/CPR + APSS watchdog: offline prep (29 Sep 2026)

Agent: cpufreq-watchdog. **Offline only**: no phone, no adb/fastboot/EDL, nothing flashed, no `m`. Items H52 (no cpufreq)
and H64 (hardware watchdog) of `docs/completeness-audit-20260929.md`.

## 1. Status

| Item | Ready offline | Needs an attended test | Needs Pierre |
|---|---|---|---|
| APSS watchdog: DT node, `qcom-wdt.ko` in the base group, watchdogd service | yes. It is in the ROM sources for the next build (r6). It stays inert until it is enabled. | W1-W3 (section 4) | whether to switch it on by default after W3 |
| CPR/OSM cpufreq kernel on r5 (`boot-r5-cpr-*.img`, RAM boot only) | yes. Built, ABI-identical to r5, DT cross-checked against stock. | C0, F2a check, F3a step, F4 soak (section 5) | must be present for every voltage step |
| Read-only fuse checks (stock S1, optional F1b) | yes (power-20260926 tools, re-bundled) | S1 on rooted stock | S1 needs rooted stock Android |

## 2. APSS hardware watchdog (H64)

- **DT**: new `device/hisense/a6l/kernel/a6l-watchdog-v75.dtso` defines `watchdog@17817000`. It has
  `"qcom,apss-wdt-sdm660", "qcom,kpss-wdt"`, `clocks = <&sleep_clk>` (32.764 kHz), GIC SPI 3 (bark) and `timeout-sec = <30>`.
  It follows the stock `qcom,wdt@17817000` (msm-watchdog: SPI 3/4, bark 11 s, pet 10 s). SPI 3 and 4 are unused in the
  V74 base. The overlay is added to `tools/build-rom-v2-dt.sh` (OVLS end, plus a check line). Merge: A6L_V75DT_BUILD_PASS,
  and the node resolves to the sleep clock.
- **Module**: `qcom-wdt.ko` is built out of tree with W=1 against both kernels, from each tree's own in-tree source:
  - V67 and r5: 0 warnings.
  - r5: 25 imports, 0 CRC mismatches against the kernel-r5 Module.symvers.
  - Its `.text` is identical to the in-tree `.ko`.
  - Files: `firmware/extracted/wdt-20260929/{v67,r5}/qcom-wdt.ko` plus SHA256SUMS. `tools/stage-rom-v2-prebuilts.sh` stages it for
    `A6L_KERNEL=v67|r5`, and `rom/modules/base.txt` loads it (early-init, display group).
  - Stage dry runs: v67 and r5 both give STAGE_ROM_V2_PREBUILTS_PASS.
  - The probe only **reads** WDT_EN/WDT_STS. The stock kernel pets this block from EL1, so it is not XPU-protected.
    Nothing is armed. If the bootloader had left it running, the kernel would pet it (`WATCHDOG_HANDLE_BOOT_ENABLED=y`).
- **Userspace**: new `device/hisense/a6l/watchdog/`:
  - `init.a6l-watchdog.rc` runs the AOSP `watchdogd` (`/system/bin/watchdogd 10 20`, already in the system image), which
    sets a 30 s timeout and pets every 10 s. It has `seclabel u:r:watchdogd:s0` and uses platform policy only: no sepolicy change.
  - The service starts **only when `persist.vendor.a6l.watchdog=1`** and only after `sys.boot_completed`, so a boot-time
    stall cannot become a reboot loop.
  - There is deliberately no stop trigger: killing watchdogd leaves the hardware armed, and it bites 30 s later. To
    disable it, set the property to 0 and reboot.
  - Note: AOSP init (`reboot.cpp`) starts the service named `watchdogd` on every shutdown. A hung shutdown therefore ends in a reset.
  - `watchdog.mk` is inherited from `rom/r6/r6.mk`. A6L_R6_STATIC_TEST PASS; A6L_ROM_STATIC_TEST PASS.
  - The hardware maximum is 0xfffff / 32764 = 32 s. The kpss pretimeout (bark) fires 1 s before the bite and is only logged
    (no pretimeout governor).
- **Test tool**: `a6l_wdtctl` (`watchdog/tools/a6l_wdtctl.c`, NDK static, `firmware/extracted/wdt-20260929/a6l_wdtctl`) has three commands:
  - `info`: support, timeout, bootstatus, then a magic close.
  - `pet <timeout> <interval> <n>`: behaves like watchdogd.
  - `bite <timeout>`: arms the watchdog and stops petting.
- **QEMU** (`tools/build-watchdog.sh qemu`): QEMU virt has no APSS WDT, so `softdog` stands in for the device.
  - Setup: r5 Image, initramfs with `/init = wdt-qemu-init`.
  - Checks: `qcom-wdt.ko` loads (vermagic plus CRCs) and registers `qcom_wdt`; info; the watchdogd contract `SETTIMEOUT 30 -> 30`
    with magic close; 6 pings at 1 s with a 4 s timeout and no reset; `bite 3`, after which the VM resets 3 s after arming.
  - Result: **A6L_WDT_QEMU PASS** on the r5 Image and on the CPR r5 Image.
  - Not emulated: the qcom-wdt MMIO probe itself and the SDM660 reset path.

## 3. CPR/OSM review (docs/rest-20260924.md, docs/power-20260926.md, kernel/cpr-sdm660, a6l-cpufreq-v75/f2a)

Findings:
- **R1, fixed**: the SDM660 was not in the `cpufreq-dt-platdev` blocklist, and neither tree had it. With `operating-points-v2` on
  the CPU nodes, `cpufreq-dt` would also create a device and race `qcom-cpufreq-hw`. Without CPU clocks, it would only vote
  CPRh levels while the frequency stays put. The fix adds `qcom,sdm630` and `qcom,sdm660` to the blocklist (patch 0004).
  The msm8998 entry is the upstream precedent.
- **R2, fixed**: the 24 Sep CPR tree carried an `opp/debugfs.c` fix (duplicate symlink when a pmdomain consumer's OPP table
  is created in `attach`) that was missing from the saved diffs. It is now patch 0003.
- **R3, fixed**: W=1 found 2 warnings in the forward-ported `qcom-cpufreq-hw.c`: an unused `freq` and an unused `qcom_cpufreq_set_bw`.
  Patch 0005 makes no functional change. The per-target bandwidth vote stays off, because
  `dev_pm_opp_set_opp()` would also vote the CPRh required-opps that the OSM hardware owns.
- **R4, checked OK**: new `kernel/cpr-sdm660/check_cpr_dt.py` compares the merged DTB with the stock DT:
  - Every OPP's pll-override, spare, pll-div and L-val matches the stock OSM LUT **in all 4 speed bins**.
  - Every required CPRh level matches the stock LUT corner.
  - All 8 `qcom,opp-fuse-level` pairs match the stock `qcom,cpr-corner-fmax-map`.
  - CPU links are right by MPIDR (cpu@1xx = perfcl/apc1).
  - All 40 nvmem cells lie in qfprom rows 38 and 65..71. Rows 0..37 reset the phone on 24 Sep.
  - Results: f2a PASS and v75 PASS. Host test `test_check_cpr_dt.sh`: A6L_CPR_DT_TEST PASS, with 5 mutations caught.
  - F2a = pwrcl 300/633.6/902.4 MHz (corners 1-3, fuse corners 1-2) plus perfcl 300/1113.6 MHz (corners 1-2, fuse corner 1).
    Stock ceilings for these corners are 724 mV and floors are 588-596 mV. It is open loop only.
- **R5**: the old CPR Image (24 Sep, e6191dae) is `7.2.3-a6l-probe` without the `+` and is pre-r5, so no ROM module would load on it.
  It is replaced by the r5-based build below.
- **R6**: r5 has `THERMAL_EMERGENCY_POWEROFF_DELAY_MS=0`, so a critical trip does not power off (android-thermal-cooling finding).
  The CPR build sets it to 100 (`kernel/configs/a6l-thermal.config`).
- **R7**: the old `run-power.sh cpr-step` had several gaps:
  - It checked temperature only after each hold, loaded 1 CPU per cluster, and used TMAX 70 C with no battery abort.
  - It assumed `policy0/4` (logical CPU 0 is the boot CPU cpu@0 = pwr cluster), did not check that the OSM followed, and kept no synced evidence log.
  - Superseded by `power/cpr-r5/run-cpr-r5.sh` (section 5).
- Still unverified and only answerable on the phone:
  - whether TZ allows the OSM sequencer `qcom_scm_io_writel` on SDM660;
  - the CX AO vote as the CPR power domain;
  - the closed-loop constants (closed loop is disabled).

**CPR r5 kernel**: `tools/build-cpr-r5.sh src config build check dt boot bundle`.
- Sources and outputs:
  - Source: `/home/a6l/kernel/a6l-rom-r5-cpr-src`, a copy of the r5 tree plus `kernel/cpr-sdm660/r5/0001..0005` and `cpr.h`.
  - Out: `out-a6l-rom-r5-cpr`.
  - Config: r5 plus `QCOM_CPR3=y` plus the thermal DELAY_MS=100.
- Build results:
  - Image: 0 warnings. W=1 on the CPR, cpufreq-hw, platdev and opp objects: 0 warnings.
  - `7.2.3-a6l-probe+`.
  - **vmlinux ABI**: 12165 r5 exports, 0 missing, 0 CRC changes, 9 added (cpr-common). So every r5 ROM module loads.
- Boot images: `boot-r5-cpr-c0.img` (boot-r5 DTB, no CPR node) and `boot-r5-cpr-f2a.img` (plus a6l-cpufreq-f2a and
  a6l-thermal-cooling-v75). They use the same ramdisk, cmdline and header v1 as boot-r5 (ae05419b): only the kernel and the
  appended DTB differ, which the script asserts. They are for `fastboot boot` only.
- Once an r6 boot directory built with `A6L_KERNEL=r5` exists, rebuild the images against it:
  `A6L_CPR_BOOT=/home/a6l/rom-v2/boot-r6 bash tools/build-cpr-r5.sh dt boot bundle`. The r6 DTB also carries the watchdog node.
- Test tool `run-cpr-r5.sh` modes:
  - `pre`: read-only, with GO/NOGO.
  - `check`: read-only. Verifies the DT marker, `qcom-cpufreq-hw`, 2 policies and every OPP within the caps.
  - `step`: voltage. Needs `I_AM_ATTENDED=1`. Loads the whole cluster; polls every 1 s and aborts at any zone >= 65 C or battery >= 45 C,
    then returns to the lowest OPP and restores the governor. Checks that the OSM followed (5%) and keeps a synced log.
  - `soak`: schedutil with a bursty load and the same aborts.
  - `restore`.
  - The cluster is taken from the CPU `of_node` (cpu@1xx = perf); an unknown cluster is limited to 300 MHz.
  - Host simulation: A6L_CPR_R5_SIM PASS under dash, busybox sh and sh.
- QEMU (virt, no CPR node): the CPR r5 Image boots, loads modules and runs the watchdog test. PASS.

## 4. Attended: watchdog (after the first install of an r6 ROM; phone on USB)
- **W1, read-only**:
  - Run `adb shell "lsmod | grep qcom_wdt; ls -l /dev/watchdog*; dmesg | grep -i wdt"`. Expect the module and `/dev/watchdog0`.
  - Push `a6l_wdtctl` to /data/local/tmp and run `a6l_wdtctl info`. Expect `identity=... options ... settimeout magicclose pretimeout cardreset`,
    `timeout=30`, `bootstatus=0x0`. It opens and magic-closes the device, which stops it.
- **W2**: `a6l_wdtctl pet 30 10 4` (40 s, no reset) then `a6l_wdtctl pet 5 2 10`. Expect no reset.
- **W3 (reset test)**: `a6l_wdtctl bite 5`. The phone must reset about 5 s later.
  - **Record where it lands**: normal reboot, crash-dump or download mode (9008/900E on the laptop), or a stuck screen.
  - After the reboot, `a6l_wdtctl info` should show `bootstatus` with CARDRESET.
  - If it lands in EDL/ramdump, stop: the watchdog must stay off until the download-mode cookie is handled (qcom_scm `download_mode=0`, IMEM).
- **W4**: `setprop persist.vendor.a6l.watchdog 1`, reboot, then check `ps -A | grep watchdogd` and the dmesg `watchdogd started (interval 10, margin 20)`.
  Then suspend/resume twice with the screen off for 2 minutes (qcom_wdt stops in suspend): no reset expected.
  Then `adb reboot` and `reboot -p`: both must be clean.

## 5. Attended: CPU frequency scaling (**voltage-affecting: Pierre present**, phone cool < 35 C, on USB, battery > 40 %)
Prerequisite: an installed ROM whose vendor modules are the **r5** set (a build with `A6L_KERNEL=r5`). The CPR kernel has the r5 ABI;
V67 modules are refused (MODVERSIONS). The boot image is **RAM-booted** (`fastboot boot`), never flashed. Rollback = a normal reboot.
Recommended: W3 passed and the watchdog on (W4), so that a hang resets the phone.
Laptop folder: `firmware/extracted/cpr-r5-20260929` (SHA256SUMS). On the phone: `adb push run-cpr-r5.sh /data/local/tmp/`, and
`P='cd /data/local/tmp && sh run-cpr-r5.sh'`.
0. **S1 (read-only, rooted stock, optional but recommended)**: `docs/power-20260926.md` §3 S1 (`a6l-stock-cpr-read.sh`,
   `a6l_cpr_openloop.py --stock-log`). Any `DIFF` = stop.
   F1b (`a6l_fuserows`, rows 38/65..71 only) is optional; skip it if S1 is clean.
1. **C0**: `fastboot boot boot-r5-cpr-c0.img` (no CPR node). Expect the same boot as r5.
   Then run `adb shell "MODE=pre $P"` and expect `cpufreq policies: 0` and `PRE GO`. This proves the Image and modules before any voltage change.
2. **F2a check**: `fastboot boot boot-r5-cpr-f2a.img`, then `adb shell "MODE=check $P"`.
   - Expect `CPR_CHECK PASS`: 2 policies with the qcom-cpufreq-hw driver, pwr avail `300000 633600 902400`, and perf `300000 1113600`.
   - Also send the `A6L_CPR_DBG` corner voltages for `a6l_cpr_openloop.py --mainline` against the S1 stock log.
   - If the phone hangs or resets at boot: long-press power, boot normally, stop and report. The CPRh/OSM programming or the TZ `scm_io_writel` was refused.
3. **F3a step**: `adb shell "MODE=step I_AM_ATTENDED=1 HOLD=5 $P"`. Expect `STEP ... ok` for each OPP, then `CPR_STEP DONE`.
   - `ABORT` or any `MISMATCH(>5%)` = stop.
   - After a hang: `/data/local/tmp/a6l-cpr-r5.log` (synced) shows the last step.
4. **F4 soak**: `adb shell "MODE=soak I_AM_ATTENDED=1 MIN=10 $P"` (`CPR_SOAK DONE`), then normal use for 30 minutes and a suspend/resume.
5. Later, only after 3 and 4: the full `a6l-cpufreq-v75` (pwrcl <= 1536 MHz, perfcl <= 1747.2 MHz) as a new boot image.
   Then enable closed loop, and finally merge into the ROM DT together with `a6l-thermal-cooling-v75` (CPU passive cooling).

## 6. Artifacts
- `firmware/extracted/wdt-20260929/`:
  - v67/qcom-wdt.ko `88680c9b…`
  - r5/qcom-wdt.ko `63580d3b…`
  - a6l_wdtctl `d9a0b7ab…`
- `firmware/extracted/cpr-r5-20260929/`:
  - Image.gz `6e7d0f3b…`
  - boot-r5-cpr-c0.img `b9c294a8…`
  - boot-r5-cpr-f2a.img `837b92b1…`
  - f2a.dtb `cb2159cf…`
  - c0.dtb = boot-r5 `5d186e79…`
  - run-cpr-r5.sh `599500a7…`
  - plus the stock/fuse tools, config, vmlinux.symvers and SHA256SUMS.
- Sources:
  - `device/hisense/a6l/kernel/{a6l-watchdog-v75.dtso, cpr-sdm660/{r5/*.patch, cpr.h, check_cpr_dt.py, test_check_cpr_dt.sh}}`
  - `device/hisense/a6l/watchdog/{init.a6l-watchdog.rc, watchdog.mk, tools/a6l_wdtctl.c, tests/wdt-qemu-init.c}`
  - `device/hisense/a6l/power/cpr-r5/{run-cpr-r5.sh, tests/test-run-cpr-r5.sh}`
  - `tools/{build-cpr-r5.sh, build-watchdog.sh}`
- Edited:
  - `tools/build-rom-v2-dt.sh` (watchdog overlay plus check line; rom-v2.dtb changes)
  - `tools/stage-rom-v2-prebuilts.sh` (qcom-wdt)
  - `rom/modules/base.txt`
  - `rom/r6/r6.mk` (inherit watchdog.mk)

## 7. Needs Pierre
- Present for every step in section 5 and for W3.
- After W3 and W4: whether the watchdog becomes default-on. That is a one-line `PRODUCT_VENDOR_PROPERTIES += persist.vendor.a6l.watchdog=1`.
- Whether the first r6 install uses the r5 kernel (`A6L_KERNEL=r5`). The CPR test requires it.
