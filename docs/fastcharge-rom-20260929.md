# Fast charge in the ROM (fastcharge-rom), 29 Sep 2026

Offline only: no phone, no adb/fastboot/EDL, nothing flashed, no `m`. Follows `docs/fastcharge2-20260929.md` §6
(power29) after the attended QC PASS (Hisense 9 V charger: QC3, usbin 9.06 V held, no abort).

## 1. What changed
| Area | File | Change |
|---|---|---|
| Kernel series | `device/hisense/a6l/kernel/rom-v2/series` | + `power/smbx/qcom_smbx-a6l-hvdcp.patch` right after fcc-jeita (power29 + bug hunt P4 abort-suspend release + P5 plug-in IRQ kick) |
| ROM module (v67 + r5) | `firmware/extracted/fastcharge-rom-20260929/{v67,r5}/qcom_smbx.ko` (+ `src/`, `SHA256SUMS`, `build-info.txt`, `hvdcp-sim.log`) | built by new `tools/build-fastcharge-rom.sh`: baseline `qcom_smbx.c` + fcc-jeita + hvdcp. v67 `e2f64c51…`, r5 `08dbe9eb…`, same srcversion `A4A97FD6182ECC379C15A63`. W=1 0 warnings, vermagic OK, imports resolved, r5 CRCs 58/58 OK; baseline+fcc-jeita == r5 tree file |
| Staging | `tools/stage-rom-v2-prebuilts.sh` | v67 `setko` fastcharge-rom (replaces merge-20260928 `0627b124`); r5 copies the r5 build after the kernel-r5 replacement; SHA256SUMS checked |
| Module options | `device/hisense/a6l/rom/modules/charger.txt` | `qcom_smbx.ko fcc_max_ua=2400000 jeita_hard=1 hvdcp_enable=1 hvdcp_max_uv=9000000 hvdcp_icl_ua=2000000` = stock (FCC 2.4 A, hvdcp-usb-icl 2.0 A, 5–9 V adapter window, never 12 V) |
| Guard | `device/hisense/a6l/power/rom/a6l-chg-guard.sh` | Q1–Q5 below |
| SELinux | new `rom/sepolicy/vendor/a6l_fastcharge.te`; `genfs_contexts` + `/module/qcom_smbx/parameters` → `sysfs_a6l_chg_param` | guard: rw parameters, write `sysfs_udc` soft_connect |
| Tests | `power/test/sim-power.sh` (+32 Q cases), `rom/tests/test-rom-static.sh` (+5), `tools/check-rom-v2-kernel-series.sh` (qcom_smbx series build W=1 + srcversion == staged) | |

## 2. Guard: 9 V awareness and the USB pull-up (`a6l-chg-guard.sh`)
- **Q1 back to 5 V first.** Any non-normal condition makes the guard write `hvdcp_enable=0` BEFORE the lower input limit
  or the suspend:
  - cool / warm / cold / hot;
  - warm-voltage hold, OV (4.42 V);
  - no telemetry.
  It then waits up to 4 s for the driver to leave `verify`/`active` (QC3 decrement pulses + FORCE_5V). The reason: the
  same ICL gives ~1.8× the battery current at 9 V.
- **Q2 QC back on** only in the normal zone with nothing suspended, after the 5 V limits were written. Re-enabling needs
  12.0–43.0 C (2 C hysteresis: no APSD rerun per 0.1 C around 10/45 C). With a cable in, the driver re-runs APSD by itself
  (`hv_apsd_stale`). Switch: `setprop persist.vendor.a6l.chg.qc 0` keeps 5 V without a rebuild.
- **Q3 ramp ownership (interaction bug found here).** The guard's 2.0 A write went through the driver's `current_max`
  setter, which in `verify` clamps only to `hvdcp_icl_ua` (2 A). That bypassed the driver's "≤ 1 A until 9 V is
  verified" ramp, and in `idle`/`wait` it also bypassed the DCP ≤ 1 A detection limit. Fix: in the normal zone with QC on,
  the guard does not write `current_max` while the driver is in idle/wait/verify. Its lower limits (cool/warm) still go
  through, after Q1.
  - Driver-side hardening (clamp VERIFY/WAIT writes to 1 A) is left as a suggestion. It would need a module rebuild and
    a sim case.
- **Q4 USB gadget D+ pull-up** (the recovery `a6lusbwd` issue, same in the ROM: `a6l_manual_usb=1` + always-bound gadget):
  - unplugged or `[DCP]` → `soft_connect disconnect` (re-asserted at every evaluation on DCP, in case the gadget HAL
    rebinds);
  - `[SDP]`/`[CDP]` → `connect` (adb/MTP enumerate ~1–2 s after plug-in);
  - once per plug-in, when the driver has settled at `dcp-5v` with the pull-up already off → one `hvdcp_rerun`;
  - SIGTERM (`stop a6l_chg_guard`) reconnects first;
  - `setprop persist.vendor.a6l.chg.pullup 0` leaves the gadget alone.
  Stock only starts the peripheral on SDP/CDP.
- **Q5 monitoring.** Every `hvdcp_status` state change goes to kmsg (`A6L_CHG_GUARD qc state=…`) and to `vendor.a6l.chg.qc`.
  The source/QC-state change wakes the guard within a 1 s tick.
- **Without the HVDCP parameters** (charger not loaded yet, or an older module), the guard behaves exactly as before
  (sim cases).
- **Unchanged driver aborts:** > 9.6 V / USBIN OV, sag 3×, overcurrent 2×, battery hot/cold/OV, stuck adapter → input
  suspend, released at unplug (P4).

## 3. Checks (all PASS)
- `tools/build-fastcharge-rom.sh` → **FCROM_PASS**:
  - A6L_HVDCP_SIM PASS (85);
  - A6L_HVDCP_OFF_TRACE SAME (21 writes).
- `power/test/sim-power.sh` → **A6L_POWER_SIM PASS**:
  - 32 new Q cases, in WSL and in the Cowork VM, repeated;
  - note: the pre-existing p29 "exactly one APSD rerun" monitor case failed once in 5 runs on the loaded VM (timing, not
    touched here).
- `rom/tests/test-rom-static.sh` → A6L_ROM_STATIC_TEST PASS.
- `check-a6l-sepolicy.sh user rom/sepolicy/vendor` → PASS (userdebug PASS).
- `check-rom-v2-kernel-series.sh` → A6L_KSERIES_PASS (series qcom_smbx srcversion = staged).
- Stage dry runs:
  - v67 → STAGE_ROM_V2_PREBUILTS_PASS (`OVERRIDE qcom_smbx.ko 0627b124 -> e2f64c51`);
  - r5 → PASS (124 modules replaced, MODULE_ORDER PASS, staged r5 qcom_smbx `08dbe9eb`, staged charger.txt carries the
    options).

## 4. Needs an attended test (installed ROM, charger=1)
1. **Laptop plug:** adb/MTP still enumerate. `logcat -b kernel | grep A6L_CHG_GUARD` shows `usb: soft_connect connect`
   after `[SDP]`.
2. **Hisense 9 V charger:** USB meter ~9 V within ~15 s. `cat /sys/module/qcom_smbx/parameters/hvdcp_status` shows
   `state=active`; `getprop vendor.a6l.chg.qc` = `active`. Check the input ≤ 2.0 A, battery 25–40 C, and whether a
   `qc: APSD rerun` line appeared.
3. **Warm test:** at ≥ 45 C (or with the thresholds lowered), the log shows `qc: hvdcp_enable=0` BEFORE the 750 mA limit,
   and the meter shows 5 V.
4. **Unplug from the charger, then plug into the laptop:** adb returns without a replug.
5. **`stop a6l_chg_guard` on the charger:** the gadget reconnects.
6. **Off-mode charging** with the 9 V charger (guard runs after offcharge).
7. **Gadget HAL function switch** (MTP↔PTP) while on the charger: the pull-up is re-dropped within one guard period, and
   QC stays up.

## 5. Needs Pierre
- **OK to ship QC on by default in r6.**
  - Fallback without a rebuild: `persist.vendor.a6l.chg.qc=0`.
  - Fallback by build: remove `hvdcp_enable=1` from charger.txt.
- **Needs charger=1** (r6 default patch 0002). With charger=0, none of this loads.
