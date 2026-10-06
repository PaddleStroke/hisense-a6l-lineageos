# Android thermal (thermal-r5prep, 29 Sep 2026) — offline, no phone/adb, nothing built with `m`

Base: the completeness-audit worker's AIDL IThermal V3 HAL `device/hisense/a6l/rom/r6/thermal` (tsens zones + battery, 5 s
poller, severity callbacks). This worker checked it against the BUILT r5 kernel/DT and extended it in place (the audit worker
adapted its own tests in parallel). It is in the product: `rom/rom.mk` inherits `rom/r6/r6.mk` (apply-r6 run), which adds the
soong namespace + `android.hardware.thermal-service.a6l` (+ required `thermal-a6l.conf`, VINTF fragment, rc);
`BoardConfig-rom.mk` lists `rom/r6/sepolicy/vendor`; the pipeline stage copies all of `rom/` into the tree. The AOSP example HAL
(fake 30 °C) is NOT in rom.mk (only in the unused hals.mk/full.mk), so there is one IThermal/default.

## What the r5 kernel really has (from /home/a6l/rom-v2/kit-r5/rom-v2.dts + out-a6l-rom-r5/.config)
| item | r5 state | consequence |
|---|---|---|
| tsens zones aoss, cpuss0/1, cpu0-3, pwr-cluster, gpu (hw id 0..8) | present. Stock aliases: 1,2 = silver cluster, 3..6 = the gold cores (stock "cpu4..7"), 8 = gpu | mapped (unchanged) |
| pm660-thermal / pm660l-thermal (spmi-temp-alarm, trips 95 passive / 125 critical) | pm660l probes; **pm660 defers forever**: its io-channels = pm660_adc die_temp and `qcom-spmi-adc5.ko` is built but in no module list | added both to the HAL (SOC; SEVERE 95, CRITICAL 105, EMERGENCY 115) |
| PM660 VADC thermistors xo/msm/emmc/pa_therm0/pa_therm1/**quiet_therm** (DT labels, adc5 rev2, 0x4c..0x51) | same adc5 module missing -> no IIO device | HAL reads them over IIO once present (`in_temp_<label>_input`, m°C); skin falls back to the battery until then |
| qcom-battery power_supply (pmi8998_fg) | present, also a tripless thermal zone | battery sensor + skin fallback |
| CPU cooling | **no cpufreq in r5** (a6l-cpufreq-v75 not merged, H52) and **the CPU zones have NO cooling-maps** (passive 70 °C trips act on nothing; only critical 110 = kernel shutdown) | reported when present; DT change below |
| GPU cooling | gpu-thermal cooling-map -> adreno (devfreq cooling, only when msm.ko drives the GPU) | reported as GPU when it appears |
| charger cooling | qcom_smbx / fg expose no CHARGE_CONTROL_LIMIT -> no power_supply cooling device | charging mitigation stays in a6l-chg-guard (JEITA) |
| CONFIG | CPU_THERMAL, DEVFREQ_THERMAL, TSENS, TEMP_ALARM =y; ADC5/VADC/ADC_TM5/LMH =m; THERMAL_NETLINK off | HAL polls (5 s) |

## Changes (all in `device/hisense/a6l/rom/r6/`)
- `thermal/thermal_logic.{h,cpp}`: a sensor source may list alternatives `a|b` (first present wins) with a per-source
  `@scale`; new `iio:<labels>` source + `scanIio()` (non-indexed `in_temp_<label>_input` and indexed `in_tempN_label`);
  types POWER_AMPLIFIER/MODEM/AMBIENT; `resolvedScale`. Paths with '@' (pmic@0) still parse.
- `thermal/cooling_logic.{h,cpp}` (new): scans `/sys/class/thermal/cooling_deviceN`, maps kernel type -> CoolingType
  (cpufreq-cpuN CPU, devfreq-*gpu GPU, psy names BATTERY, ...), unique names, cur/max state.
- `thermal/Thermal.{h,cpp}`, `main.cpp`, `Android.bp`: getCoolingDevices[WithType] real, ICoolingDeviceChangedCallback
  register/unregister + notify on cur_state change; sensors and cooling devices re-scanned during the first ~60 s
  (modules loading after the HAL); sensor paths now read under the lock.
- `thermal/thermal-a6l.conf`: + pm660, pm660l, **skin = `iio:quiet_therm|battery`** (stock skin sensor; thresholds from stock
  thermal-engine: gold throttle 45, silver/GPU 47 -> LIGHT 41 MOD 45 SEV 47 CRIT 50 EMERG 53 SHUTDOWN 58), informational
  xo/msm/emmc/pa therms. CPU/GPU/battery rows unchanged.
- `thermal/tests/test_sources_cooling.cpp` (new, run by `thermal/tests/run-tests.sh`): 58 checks.
- `sepolicy/vendor/genfs_contexts` (new): pm660 adc@3100 -> `sysfs_thermal`; `hal_thermal_a6l.te`: sysfs dir/lnk_file read
  (walk /sys/bus/iio/devices).

## Checks (offline)
- `rom/r6/thermal/tests/run-tests.sh`: A6L_THERMAL_SRC_TEST PASS (58) + A6L_THERMAL_TEST PASS (47), ASan/UBSan.
- `clang++ -fsyntax-only -Wall -Wextra -Werror` of Thermal.cpp / main.cpp / thermal_logic.cpp / cooling_logic.cpp with the
  flags of the tree's thermal example HAL (clang-r596125, V3 NDK gen headers): clean (negative control errors as expected).
- `rom/r6/tests/test-r6-static.sh` PASS 0; `rom/tests/test-rom-static.sh` PASS.
- `check-a6l-sepolicy.sh user rom/sepolicy/vendor` PASS; `... user rom/sepolicy/vendor rom/r6/sepolicy/vendor` PASS.

## Not done here (needs a kernel/module owner)
1. **Load `qcom-spmi-adc5.ko`** (for pm660-thermal and every board thermistor incl. skin): add
   `drivers/iio/adc/qcom-spmi-adc5.ko` to the `$O67`/r5 module copy list in `tools/stage-rom-v2-prebuilts.sh` (line with
   pmi8998_fg) and a line in `rom/modules/misc.txt` before `pmi8998_fg.ko`. Check the adc5 rev2 thermistor calibration vs stock
   (stock quiet_therm reads ~26 °C at idle) before trusting the skin thresholds.
2. **DT (document only): CPU cooling-maps.** When cpufreq (H52) lands, add cooling-maps to cpu0-3-thermal / pwr-cluster
   (gold cpus 4-7) and cpuss0/1 (silver cpus 0-3) passive trips, as sdm660/sdm845 do, e.g.
   `cooling-maps { map0 { trip = <&cpu0_alert0>; cooling-device = <&CPU4 THERMAL_NO_LIMIT THERMAL_NO_LIMIT>, ...; }; };`.
   Without it only the 110 °C critical trip protects the CPU (kernel orderly shutdown).
3. Stock's charging-by-skin table (BATTERY_CHARGING_CTL quiet_therm 36/39/42/46) is not reproduced: a6l-chg-guard uses the
   battery temperature only.

## Attended tests after the first install
1. `dumpsys thermalservice`: HAL connected (not "no HAL"), temperatures for cpu0-3, cpuss0/1, gpu, soc, pwr-cluster, pm660l,
   battery, skin (= battery value until adc5 loads) with sane values; `logcat -s thermal-a6l` lists each sensor path.
2. `ls /sys/class/thermal/*/type` + `cat` -> confirm zone type names (with "-thermal") and whether pm660-thermal exists.
3. `ls /sys/class/thermal/cooling_device*/type` vs `dumpsys thermalservice` cooling list (expected empty/GPU only on r5).
4. After adc5 is added: `readlink -f /sys/bus/iio/devices/iio:device*` (genfs path), `cat in_temp_quiet_therm_input`, skin
   switches to quiet_therm (log line "sensor skin -> .../in_temp_quiet_therm_input").
5. Load test (charge + CPU load): battery/skin status rises (`dumpsys thermalservice` "Thermal Status"), falls back with 2 °C
   hysteresis; no AVC denials for hal_thermal_default in enforcing-trial logs.
