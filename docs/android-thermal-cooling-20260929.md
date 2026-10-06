# Thermal follow-ups: PM660 ADC, CPU cooling-maps, shutdown path (29 Sep 2026)

Follow-ups 1-3 from `docs/android-thermal-20260929.md`. Offline only: no phone, no adb, nothing built with `m`. **Not phone-tested.**

## 1. qcom-spmi-adc5 in the ROM (PM660 VADC: pm660-thermal + board thermistors)
- **DT check (r5 DT = V74 base + V75 overlays).** `pm660_adc: adc@3100` is `qcom,spmi-adc-rev2`. Its channels are
  ref_gnd, vref_1p25, die_temp, xo_therm 0x4c, msm_therm 0x4d, emmc_therm 0x4e, pa_therm0 0x4f, pa_therm1 0x50, quiet_therm 0x51,
  vph_pwr and vcoin. The thermistor channels are ratiometric, decimation 1024 and settle 200 us.
  - pm660-thermal takes `io-channels = <&pm660_adc 6>` (die_temp).
  - Stock (`stock-dtbo-20260914/stock-00-merged.dts` vadc@3100, qpnp-vadc-hc) has the same channels and the same 100k-pullup
    thermistor scaling (xo: scale fn 4 = xotherm, same curve). Decimation and settle time are equivalent.
  - **Difference:** stock labels channel 0x50 **`epd_therm`** (the e-ink panel thermistor; stock thermal zone "epd_therm").
    The r5 DT labels it `pa_therm1` (the mainline pm660.dtsi name).
- **Driver.** adc5 sets `extend_name` = the DT label and leaves the channels non-indexed, which gives
  `in_temp_<label>_input` (processed, m°C). This is what the HAL's `scanIio()` reads. The thermistor table is rev2
  `THERM_100K_PULLUP`.
- **Modules.** `qcom-spmi-adc5.ko` depends on `qcom-vadc-common`, and both are in the ROM now:
  - `rom/modules/misc.txt` loads vadc-common, then adc5, at the head of the misc group.
  - `tools/stage-rom-v2-prebuilts.sh` has the O67 copy line.
  - **v67:** `out-a6l-phone-v67/modinst` builds, byte-identical to the V67 candidate `modules.tar.gz`: adc5 47a83a7a, vadc-common 9900f8ba.
  - **r5:** `out-a6l-rom-r5/modinst`, strip-debug like collect-verify-r5.py: adc5 b4847f6b, vadc-common e2464f64. These were added to
    `firmware/extracted/kernel-r5-20260930/modules` (+ SHA256SUMS, modules-provenance.txt), so r5 now has **124** modules.
    Every import CRC equals r5 Module.symvers (providers vmlinux and vadc-common). Vermagic has modversions.
  - A W=1 recompile of both objects against the r5 source with the r5 and v67 configs (scratch O=/tmp) gives 0 warnings.
- **Checks.** Stage dry runs pass for v67 and r5 (MODULE_ORDER PASS, STAGE_ROM_V2_PREBUILTS_PASS).
- **HAL config.** The `pa-therm1` row becomes `epd-therm SOC iio:epd_therm|iio:pa_therm1`. It accepts both labels, and the stock
  one wins if the DT is ever relabelled.
- **HAL rescan.** Sensors are re-resolved every poll for 60 s, then once a minute. Before, they stopped after 60 s, and the misc
  group, which is ordered after adsp, could load adc5 later than that. Once adc5 loads, skin switches to quiet_therm.

## 2. CPU cooling-maps: `device/hisense/a6l/kernel/a6l-thermal-cooling-v75.dtso`
- This overlay is **not** in the `build-rom-v2-dt.sh` OVLS. Apply it after `a6l-cpufreq-v75` once cpufreq (H52) is merged and
  proven. Coordinate with the cpufreq/watchdog owner.
- **Mapping.** From stock thermal-engine: VIRTUAL-CLUSTER1 = tsens 3..7 is gold, and CLUSTER0 = tsens 1,2 is silver. In the DT,
  cpu0-3 = cpu@100.. are gold/perfcl and cpu4-7 are silver/pwrcl. The overlay maps:
  - cpu0..3-thermal and pwr-cluster-thermal, on their passive 70 °C trip, to the gold cpu0-3.
  - cpuss0/1 to the silver cpu4-7. These zones only had a 125 °C "hot" trip, so the overlay adds a passive 70 °C trip **and a
    critical 110 °C trip** there. The base DT has no kernel shutdown trip for the silver cluster.
- **Checked.** A fresh `build-rom-v2-dt.sh` into /tmp was followed by fdtoverlay with and without a6l-cpufreq-v75. Both merge, and
  every map resolves to the intended trip and CPU phandles. The 70 °C passive value is the mainline one: review it against stock
  (thermal-engine throttles on quiet_therm 45/47 °C) when cpufreq lands.
- **Note.** A fresh build-rom-v2-dt.sh now gives rom-v2.dtb 1c46a7f5, not the kit-r5 5d186e79. Overlay sources changed since
  kit-r5, from other workers.

## Safe shutdown path (until cpufreq)
- **Kernel, v67.** A critical trip runs `orderly_poweroff(true)`. /sbin/poweroff does not exist, so the call fails and the
  kernel forces `kernel_power_off`. **This is safe.**
- **Kernel, r5: BROKEN.** `STATIC_USERMODEHELPER=y` with path "" makes `call_usermodehelper` return 0 without running anything.
  The force fallback therefore never runs, and `THERMAL_EMERGENCY_POWEROFF_DELAY_MS=0` queues no backup. A critical trip only
  logs "HARDWARE PROTECTION shutdown".
  - **Fix (proposed for the next kernel build):** `device/hisense/a6l/kernel/configs/a6l-thermal.config`,
    `CONFIG_THERMAL_EMERGENCY_POWEROFF_DELAY_MS=100` (GKI value). Its merge onto the r5 config changes that one symbol only; it
    is used only in thermal_core.c, so the module ABI is unchanged.
- **HAL.** `thermal-a6l.conf` sets SHUTDOWN thresholds below the kernel critical trips (110 °C). Android's
  ThermalManagerService shuts down cleanly on SHUTDOWN severity for CPU/GPU/SKIN/BATTERY sensors, so this acts first on every
  kernel.
  - CPU (cpu0-3, cpuss0/1): SHUTDOWN 107.
  - GPU: SHUTDOWN 105.
  - Skin (58) and battery (60) are unchanged.
  - The PMIC temp-alarm keeps its hardware stage-3 shutdown.

## 3. Tests
- `rom/r6/thermal/tests/test_sources_cooling.cpp` (run-tests.sh) now runs 172 checks, up from 58, and PASSES. The
  A6L_THERMAL_TEST 47 checks also PASS. The new checks cover:
  - the shipped config against a simulated adc5 sysfs, before and after the module loads. Skin moves from the battery to
    quiet_therm (m°C); xo/msm/emmc/pa0 resolve; epd-therm picks pa_therm1 and prefers epd_therm; the pm660-thermal zone appears.
  - skin severities (45 = MODERATE, 58 = SHUTDOWN).
  - every CPU/GPU sensor: SHUTDOWN is in [100, 110), with its severity and hysteresis checked.
  - every CPU/GPU/SKIN/BATTERY sensor has a SHUTDOWN threshold, and all thresholds strictly increase.
- `rom/tests/test-rom-static.sh` PASS; `rom/r6/tests/test-r6-static.sh` PASS 0. Sepolicy was not touched: the pm660 adc genfs
  and the IIO dir read are already in place.

## Attended (after install)
1. `ls /sys/bus/iio/devices/iio:device*/in_temp_*` shows die_temp/xo/msm/emmc/pa_therm0/pa_therm1/quiet_therm. Idle values
   should be about 26-27 °C, like stock `thermal.txt`.
2. `/sys/class/thermal/*/type` includes pm660-thermal.
3. `logcat -s thermal-a6l` shows "sensor skin -> .../in_temp_quiet_therm_input".
