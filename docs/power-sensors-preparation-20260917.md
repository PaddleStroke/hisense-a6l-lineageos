# Battery, thermal and sensor preparation — 17 September 2026

Prepared offline while the user was away. Only read-only stock ADB queries were made. No reboot, charging setting change, sensor enable, PMIC register write, profile upload or diagnostic installation occurred on the phone.

## Battery management is a separate bring-up milestone

Showing a battery percentage is only the first step. The port needs voltage/current/temperature/charge reporting, charger detection and limits, temperature-dependent charging, over/under-voltage handling, full-charge termination, low-battery shutdown, suspend/wakeup and Android Health/Thermal integration. Standby drain and charging while powered off also need physical validation. The existing successful kernel/USB/display tests do not validate these functions.

Stock evidence:

- PM660 SMB2 primary charger at SID0 offset `0x1000`; PM660 third-generation fuel gauge at `0x4000`; RRADC `0x4500` and ADC rev2 `0x3100`.
- A separate SMB1351 parallel charger at I²C `0xc176000`, address `0x1d`. Stock Android exposes a `parallel` power supply. Our pinned mainline tree has no exact SMB1351 driver; do not substitute SMB347 or ignore the shared charging policy.
- The saved A6L battery profile is nominally **3800 mAh**, maximum **4.4 V**, stock maximum fast-charge current **2.4 A**. These are stock configuration values, not recommended new test settings. Full stock constraints and the 224-byte profile are archived with hashes.
- Stock JEITA profile thresholds are 0/10/44/55, with warm/cool current reductions and a warm voltage reduction. Cutoff/empty values are 3300/3250 mV. Preserve downstream semantics and hysteresis; copying just the maximum voltage/current is insufficient.
- `thermal-engine.conf` adds skin/board-temperature charge mitigation and CPU/GPU limits. `quiet_therm` charge levels have thresholds 36/39/42/46 °C and distinct recovery thresholds; their action indexes are not direct current values.
- Read-only stock baseline reported full/100%, 4372 mV, battery temperature 250 (25.0 °C), USB attached. This is one stock snapshot, not proof of new-kernel charging or battery condition.

Current driver audit:

- Our kernel includes `pmi8998_fg`, and `pm660.dtsi` already uses that compatible for its third-generation fuel gauge. Probe adjusts memory-interface interrupt/control bits and clears IMA/DMA errors. It must not be described as passive register reading, even though the first intended function is telemetry.
- `qcom_smbx` explicitly matches `qcom,pm660-charger`. Probe initializes the charger, enables charging, changes USB/Type-C configuration, disables watchdogs in its initial sequence, sets float voltage and sets a **hard-coded 1.95 A** fast-charge value. Its own comment expects temperature-dependent control to be added. It is therefore **not approved here for activation merely because it compiles**. Review A6L policy, parallel charger state and fault handling first; driver initialization happens before some battery-info validation.
- The existing V38 tree leaves both the mainline charger and fuel gauge disabled. Prepared modules are not loaded onto the phone. Existing firmware-initialized charging behavior is not a completed kernel charging port.
- Stock thermal sysfs exposes mixed legacy units (e.g. battery 25000, TSENS around 269, board thermistors around 25); the mainline test must validate the units of each driver. Do not copy stock raw thresholds into standard millidegree interfaces without conversion.

## Sensors identified

Stock Android's Sensor List has **61 logical entries**. The HAL reports 44 hardware-provider entries, but these include calibrated/uncalibrated and wake-up duplicates. They are not 44 or 61 separate physical chips.

| Function | Evidence | Planned route |
|---|---|---|
| Accelerometer and gyroscope | Live stock names identify Bosch BMI160 | ADSP/Qualcomm Sensor Manager (SMGR) first; avoid competing direct AP bus access while DSP owns it |
| Compass | Live stock name identifies AKM AK09918 | SMGR magnetometer path; use live identity ahead of the generic registry's AK09911 comment |
| Front ambient light and proximity | Stock Sensortek endpoints; DT STK3338 at QUP6 `0xc1b6000`, address `0x47`, IRQ GPIO71 | Port/audit exact part support. Mainline stk3310 supports other listed variants, not STK3338; don't invent compatibility |
| Rear ambient light and proximity | Stock AMS endpoints; DT TMD3702 at `0xc176000`, address `0x49`, IRQ GPIO113 | Separate exact-device driver work; no exact TMD3702 binding/driver found in our tree |
| Hall/magnet switch | Stock `hall` endpoint; DT Hisense Hall device on GPIO75 | Characterize state/polarity/debounce and wakeup before exposing a standard input switch |
| Capacitive sensing | DT ABOV A96T346, address `0x20` on QUP6, IRQ GPIO55 | Identify its actual stock role and interface; not yet mapped to an Android endpoint or validated live |
| Temperatures | Battery, PM660/PM660L, SoC TSENS, board/skin/PA and `epd_therm` visible in stock thermal classes | PMIC ADC/TSENS plus board-specific thermistor conversion and policy; e-paper temperature belongs in that display's integration |
| Rotation, gravity, steps, gestures | QTI/AOSP/Hisense logical entries in stock list | Fusion/algorithm and Android Sensors HAL work after raw streams, timestamps and coordinate frames are correct |

Our kernel has a SMGR driver that binds to QRTR service `0x100`, version 1, instance 50. It can expose accelerometer, gyro, magnetometer and proximity sensors through IIO after the DSP/service stack starts. This avoids needing to replace all DSP-side chip drivers. It does **not** establish that the AP-side front/rear light/proximity sensors appear through that service, nor implement all vendor gesture/fusion endpoints.

The stock registry indicates BMI160 SPI bus `0x1001`, interrupt GPIOs 68/69 and separate accelerometer/gyro configuration. Defaults map acceleration/gyro axes `-X,-Y,+Z`, compass `+X,-Y,-Z`; these are evidence to verify, not a transform to apply twice if DSP output already incorporates it. Persisted calibration may override default registry settings. Existing calibration backups remain untouched.

## Prepared artifacts and verification

`firmware/extracted/power-sensors-prep-20260917` contains:

- **15 modules with complete dependency order and hashes**: fuel gauge, primary charger, RRADC/ADC, QRTR and SMGR sensor classes. They came from the existing isolated peripheral build, not a change to the validated V38 output.
- **33/33 QEMU checks passed**: load/unload all modules in the exact validated kernel, cleanup, no panic. QEMU has no physical PMIC or sensors, so the test does not execute their hardware probes or prove charging safety.
- A compiled `a6l-battery-telemetry.dtbo` and `candidate-merged.dtb` for review only. The candidate adds a stock-sized simple-battery description and enables the fuel-gauge node. Full property diff: **7 changes**, confined to that battery/FG/symbol; charger remains disabled and no charger dependency was added. This is not a flashable image. Final packaging must apply the changes before the bootloader's adjustments.
- Exact 224-byte stock battery profile and original thermal/sensor configuration with source hashes. No profile was uploaded.

Source candidate: `device/hisense/a6l/kernel/a6l-battery-telemetry.dtso`.
Read-only stock capture: `research/power-sensors-20260917`. Sensor list stores identity metadata only, not app clients or sensor event history. Stock power-supply attribute reads were unavailable to the shell; supply names and Battery Service output were available. Some thermal attributes denied/failed; failures remain in the capture.

## Next physical work

1. Prioritize battery/temperature telemetry before extended radio/audio or endurance runs. Package the reviewed FG candidate and use bounded samples to check percentage, voltage, current sign, temperature, update IRQs and USB stability. Compare against stock under similar conditions. Do not label this a charging test.
2. Validate ADC/PMIC/SoC temperatures, thermistor scaling and protection paths. Establish charger/parallel-charger state, then review and implement a conservative current policy and health/thermal handling before charger activation. Test unplug/replug, full-charge termination, suspend/wakeup and normal shutdown; do not deliberately heat or over-discharge the battery to force faults.
3. With ADSP and its services running, enumerate SMGR/IIO identities, then test accelerometer/gyro/compass samples, timestamps, coordinate frames, rates, batching and wakeup. No simultaneous direct-chip driver probing while DSP owns the bus.
4. Add exact front/rear light/proximity drivers and Hall handling; verify each face independently and then dual-screen behavior. Connect Android Sensors/Health/Thermal HALs and power policy. Measure idle and suspended drain only after functional correctness.

## Primary references

- [PM660 fuel-gauge support proposal](https://www.mail-archive.com/linux-kernel@vger.kernel.org/msg2605613.html), checked against the actual current local `pm660.dtsi` and `pmi8998_fg.c`.
- [Qualcomm Sensor Manager driver proposal](https://lists.openwall.net/netdev/2025/07/10/98), checked against local `drivers/iio/common/qcom_smgr` and class drivers.
- [Upstream Qualcomm SMB charger source](https://github.com/torvalds/linux/blob/master/drivers/power/supply/qcom_smbx.c); the activation sequence and 1.95 A value above were read from our pinned local source, not assumed from current web contents.

Original V38 Image/config/Module.symvers unchanged; phone remains on stock Android. No sensor or battery feature has been declared physically working under the port from these offline checks.
