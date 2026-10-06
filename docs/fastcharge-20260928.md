# Fast charging (Quick Charge / HVDCP) for the A6L — power28, 28 Sep 2026

Agent power28, night 27→28 Sep. **Offline only**: nothing touched the phone, no ROM image, nothing committed or deleted.
The attended test needs **Pierre present and a Quick Charge 3.0 (or 2.0) wall charger** (see §4).

## 0. Power sim fixed first (run-power.sh 28 Sep thresholds)
`device/hisense/a6l/power/test/sim-power.sh` now expects the thresholds Pierre approved on 28 Sep: monitor abort at
**T_ABORT ≥ 50.0 C** (was 40.5 C in the sim), start refused at **T_START_MAX ≥ 45.0 C**, stock JEITA hot 55 C (guard unchanged).
New cases: 49.5 C no abort, 50.0 C abort + input suspend, T_ABORT override, chg-on refused at 45.0 C / passes the temp gate at 44.9 C.
Result: `A6L_POWER_SIM PASS` (was FAIL 2 after the merge). The QC monitor cases (§3) are in the same sim.

## 1. What stock does vs. what we had
Stock DT (`firmware/extracted/device-trees/stock-00.dts`, `qcom,qpnp-smb2` ~l.2562): fcc-max 2.4 A, usb-icl 2.0 A,
**hvdcp-usb-icl 2.0 A**, fv 4.40 V, warm-fcc 0.9 A, cool-fcc 0.85 A, step charging, no `qcom,hvdcp-disable` (QC on),
`dpdm-supply` = QUSB PHY, `qcom,usb-pdphy@1700` with default sink caps **5 V/3 A, 9 V/3 A**. Stock = QC 2.0/3.0 up to 9 V
(PM660 cannot do 12 V; downstream `smblib_set_adapter_allowance` maps every 12 V allowance to 9 V on PM660) + USB PD.
Mainline `qcom_smbx` (our r4 patch): 5 V only, APSD SDP/CDP/DCP, `HVDCP_EN` explicitly cleared in its init sequence, no PD.

## 2. HVDCP in qcom_smbx — `kernel/power/smbx/qcom_smbx-a6l-hvdcp.patch` (on top of `qcom_smbx-a6l-fcc-jeita.patch`)
Registers (downstream msm-4.4 `smb-reg.h`, USBIN_BASE 0x1300 = charger base 0x1000 + 0x300; checked against the
mainline defines already in qcom_smbx.c): APSD_STATUS 0x307 (QC_CHARGER, HVDCP_CHECK_TIMEOUT), APSD_RESULT 0x308
(QC_3P0 bit6 / QC_2P0 bit5 with DCP), CMD_HVDCP_2 0x343 (FORCE_12V/9V/5V, SINGLE_INC/DEC), HVDCP_PULSE_COUNT_MAX 0x35B
(QC2 field 7:6, QC3 pulses 5:0 as on the Longcheer SDM660/PM660 trees), USBIN_ADAPTER_ALLOW_CFG 0x360, USBIN_OPTIONS_1 0x362
(HVDCP_AUTH_ALG_EN bit6, AUTONOMOUS bit5, HVDCP_EN bit2), INT_RT_STS 0x310 (USBIN plugin/OV).

Module parameters:
| param | default | meaning |
|---|---|---|
| `hvdcp_enable` (0644) | **0** | 0 = the driver is **register-identical to r4** (host trace test, §3). Writing 0 at runtime returns the adapter to 5 V and clears HVDCP_EN. |
| `hvdcp_max_uv` (0444) | 9000000 | target, clamped 5.6..9.0 V, QC3 in 200 mV steps; QC2 only does 9 V (lower target → stays 5 V). 12 V is never requested. |
| `hvdcp_icl_ua` (0644) | 2000000 (stock) | USB input limit at high voltage, clamped 0.5..2.0 A. The test passes 1.0 A. |
| `hvdcp_status` (ro) | | `state=off/idle/wait/dcp-5v/verify/active/failed qc= pulses= target_uV= usbin_uV= usbin_uA= icl_uA= enable= reason=` |

Flow with `hvdcp_enable=1`: adapter window USBIN_ADAPTER_ALLOW = **5–9 V** (never 12 V), QC2 max 9 V, QC3 max pulses =
target, HW autonomous (INOV) off, HVDCP_EN + AUTH_ALG on. If HVDCP was switched on with a DCP already in (APSD ran without
QC detection) APSD is re-run **once, only on a DCP** (never SDP/CDP → ADB on the laptop is never disturbed).
DCP seen → ICL ≤ 1 A while waiting (≤ 6 s) for QC_2P0/QC_3P0 or HVDCP_CHECK_TIMEOUT (→ plain DCP, 5 V, DCP ICL restored).
QC → ICL min(hvdcp_icl, 1 A) (downstream does the same before a voltage request) → QC2 `FORCE_9V` / QC3 200 mV
`SINGLE_INCREMENT` pulses (100 ms apart, abort on overshoot during the ramp) → USBIN read back through the rradc must reach
**8.4–9.6 V** (target ± 0.6 V) within 3 s → `ACTIVE`, ICL = hvdcp_icl_ua.
While active, every 1 s: USBIN > 9.6 V or the USBIN OV status → **immediate abort**; below 8.4 V 3× → abort; USBIN current >
ICL + 10 % + 100 mA 2× → abort; charger status BAT_OV / too hot / too cold → abort; userspace may lower the ICL (guard) but
not raise it above hvdcp_icl_ua. **Abort** = QC3 decrement pulses + `FORCE_5V`, ICL 500 mA, state `failed` (no retry until
unplug); if USBIN is still > 6 V 2 s later the USB input is **suspended**. Unplug (USBIN plugin RT status, which stays set
while the input is suspended) → idle. Unbind/rmmod (devm action) and `.shutdown` → `FORCE_5V`.
Absolute check in every state: USBIN > 9.6 V → abort. JEITA hard limit, FCC cap and float 4.40 V clamp are unchanged.
dmesg markers: `A6L_HVDCP QC3 detected`, `requested 9000000 uV (20 pulses)`, `A6L_HVDCP ACTIVE QC3 usbin … uV, ICL … uA`,
`A6L_HVDCP ABORT …`, `A6L_HVDCP back to 5V: …`, `A6L_HVDCP unplugged (state active) -> idle`.

## 3. Offline checks (28 Sep, `tools/build-power28.sh` in WSL → `A6L_POWER28_BUILD_PASS`)
| check | result |
|---|---|
| qcom_smbx.ko (upstream + fcc-jeita + hvdcp), clang r584948, W=1, v67 tree | **0 warnings**, vermagic `7.2.3-a6l-probe+`, 0 unresolved, params fcc_max_ua jeita_hard hvdcp_enable hvdcp_max_uv hvdcp_icl_ua hvdcp_status; sha256 **22a976b4…** |
| host register-level test `power/test/hvdcp/` (driver .c compiled unchanged against a fake regmap/iio/workqueue + PM660 SMB2/adapter model, gcc ASan/UBSan) | **`A6L_HVDCP_SIM PASS (58 checks)`**: off = nothing touched; SDP (no APSD rerun, 500 mA); plain DCP (5 V, ICL restored); QC2 9 V; QC3 20 pulses 9.0 V; QC3 7 V target; QC2 with 7 V target stays 5 V; overshoot 10.2 V / 9.7 V abort; adapter not raising → abort after 3 s; sag 8.0 V abort after 3 samples; overcurrent abort; battery too hot abort; adapter ignores FORCE_5V → input suspended; QC3 400 mV steps → abort during ramp; runtime enable/disable (5 V, HVDCP_EN cleared, ICL back), re-enable (one APSD rerun), unplug/replug, SDP → QC charger move, userspace ICL clamp/lower, hvdcp_icl_ua clamp 2 A, suspend at 9 V then disable, unbind + shutdown → 5 V, 12 V never requested |
| hvdcp_enable=0 register write trace vs. the r4 (fcc-jeita) driver | **`A6L_HVDCP_OFF_TRACE SAME`** (probe + unplug + DCP plug, 21 writes) |
| `power/test/sim-power.sh` (dash) | **`A6L_POWER_SIM PASS`**: chg monitor (new thresholds), QC monitor (QC_PASS once, end → HVDCP off + ICL 500 mA, USBIN 9.7 V / 50.0 C / 4.43 V / USBIN 1.3 A / battery 1.7 A at FCC 1.5 A aborts, 9.6 V and 1.15 A no abort, driver `failed` reported without stopping the monitor, start refusals 45.0 C / QICL 1.6 A / 12 V / FCC 2.0 A / ko without HVDCP), chg-off (HVDCP off, then suspend), ROM guard unchanged |

Bundle `firmware/extracted/power-20260928/` = the power-20260927 files (unchanged hashes) + **qcom_smbx.ko 22a976b4…** +
**run-power.sh 92942afd…** + logs; staged on the laptop as a NEW folder **`~/A6L-usb-20260915/v75/power28`** (`sha256sum -c` bad=0).

## 4. Attended test QC1 (Pierre present; CURRENT/VOLTAGE-AFFECTING from `MODE=qc` on)
**Needs a Quick Charge 3.0 wall charger** (QC logo, e.g. "5V⎓3A 9V⎓2A 12V⎓1.5A", USB-A port) + a USB-A→C cable. A QC 2.0
charger also works (QC2 path). A PD-only charger (e.g. Apple USB-C) will simply stay at 5 V (`state=dcp-5v`) — that is a
valid "plain DCP" result but not a QC test. An inline USB-A power meter, if available, is the best independent check (9 V).
Phone on the desk, not in a case, battery ideally **≤ 80 %** (near full the charger tapers and little current flows), battery
< 45 C, **fresh V75-usb recovery boot** (qcom_smbx must not be loaded yet: an older chg-on build has no HVDCP → `refused`).
```
cd ~/A6L-usb-20260915 && (cd v75/power28 && sha256sum -c SHA256SUMS)      # all OK
adb -s HLTE730T-PROBE push v75/power28 /tmp/power28
P='export PATH=/tmp/bin:$PATH; export D=/tmp/power28'
adb -s HLTE730T-PROBE shell "$P MODE=chg-ro; sh /tmp/power28/run-power.sh" 2>&1 | grep -v linker          # read-only: CHG_DT_OK, RRADC_OK
adb -s HLTE730T-PROBE shell "$P MODE=qc DUR=420; sh /tmp/power28/run-power.sh" 2>&1 | grep -v linker   # QICL=1000000 QV=9000000 FCC=1500000
```
PASS so far: `HVDCP on: max 9000000 uV, ICL at 9 V 1000000 uA, FCC cap 1500000 uA, JEITA hard Y`, `status: state=idle …`
(on the laptop SDP nothing happens: no APSD rerun, ADB stays up), `QC monitor started`.
**Then within ~1 min: unplug the laptop cable, plug the phone into the QC wall charger.** Leave it ~6 min (DUR=420 s: the
monitor switches HVDCP off at the end **while still on the QC charger** → proves 9 V → 5 V). With a power meter: 5 V → ~9 V
within ~5 s of plugging, back to 5 V at the end. Then unplug, plug back into the laptop:
```
adb -s HLTE730T-PROBE shell "$P MODE=qc-status N=100; sh /tmp/power28/run-power.sh" 2>&1 | grep -v linker > qc1.txt
adb -s HLTE730T-PROBE shell 'dmesg | grep -E "A6L_HVDCP|A6L: "' > qc1-dmesg.txt
adb -s HLTE730T-PROBE shell "$P MODE=chg-off; sh /tmp/power28/run-power.sh" 2>&1 | grep -v linker     # HVDCP off (5 V) + input suspend
```
**PASS markers**: dmesg `A6L_HVDCP QC3 detected` (or QC2), `A6L_HVDCP QC3: requested 9000000 uV (20 pulses)`,
`A6L_HVDCP ACTIVE QC3 usbin 8.4–9.6 V`; log `A6L_PWR QC_PASS HVDCP active: qc=3 usbin_uV≈9000000 usbin_uA≤1100000
bat_uA<0` (battery current more negative than the 5 V runs, capped by FCC 1.5 A), `QCMON` lines stay active with
temp < 45 C, then `QCMON_DONE … hv[state=off … usbin_uV<6000000]` (**9 V → 5 V on the charger**), and after replug
`A6L_HVDCP unplugged … -> idle` (or state off). **No** `ABORT`, **no** `QC_DRIVER_ABORT`.
Second pass (only if QC1 clean and temps < 40 C): same with `QICL=1500000` (no FORCE needed).
**Not a pass, but safe** (tell the agent, send qc1*.txt): `state=dcp-5v` on the QC charger (QC not detected: charger/cable,
or D+/D- not reaching the PMIC — see risk 2), `QC_DRIVER_ABORT … did not reach the window` (adapter or rradc reading),
`ABORT …` from the monitor (input suspended — `MODE=chg-resume` on the laptop to charge again at 5 V).

## 5. Risks
1. **HW authentication**: with HVDCP_AUTH_ALG_EN the SMB2 may raise VBUS on its own right after QC detection (downstream QC2
   phones end at 9 V without software). The model does not simulate that; the driver covers it: ICL ≤ 1 A from the DCP result
   on, the 5–9 V window (12 V impossible), absolute 9.6 V abort, and "high USBIN without a QC result" abort.
2. **D+/D- path**: stock gives the charger a `dpdm-supply` (QUSB PHY regulator) for APSD/HVDCP; mainline has no such link.
   APSD SDP/DCP already works on mainline (28 Sep: SDP), so the lines reach the PMIC, but QC signalling (D+ 0.6 V / D- 3.3 V
   pulses) is unproven → worst case QC is not detected (safe, 5 V).
3. **rradc USBIN readings**: 28 Sep the monitor showed usbin_uV=4882812 / usbin_uA=469658 constant for minutes (1-LSB
   quantisation or a stale read). If USBIN_V were stale at 9 V, the verify step fails → abort to 5 V (safe); but the ACTIVE
   overvoltage check relies on it. First look in qc1.txt: does usbin_uV move 5 V → 9 V → 5 V?
4. **Warm/cool at 9 V**: the ROM guard approximates warm/cool FCC (0.9/0.85 A) by the *input* limit at 5 V (750/700 mA); at 9 V
   the same input limit gives ~1.8× the battery current. The driver has no runtime FCC knob. **Before HVDCP goes into the ROM**
   the guard must switch `hvdcp_enable=0` outside the normal zone (10–45 C) or scale the ICL by USBIN voltage.
5. PM660 QC3 pulse-count field (0x35B bits 5:0) and QC2 field come from the Longcheer SDM660/PM660 downstream trees, not from a
   datasheet. Wrong values would only limit the voltage (the software window check still applies).
6. Heat: 9 V × 1 A ≈ 9 W input vs. 2.5 W on the laptop; the 50 C monitor abort and JEITA hard limit stay, but watch the log temps.
7. No ADB while on the wall charger: the monitor runs nohup'd (as the 28 Sep unplug/replug test); if the recovery killed it,
   the driver still enforces its own limits and the log simply stops.

## 6. USB PD (PM660 pdphy) — feasibility, not implemented
Mainline has `drivers/usb/typec/tcpm/qcom/` (qcom_pmic_typec + pdphy, 1.7 k lines) for **pm8150b and pmi632 only**. The PD PHY
block itself (PM660 `pdphy@1700`, 7 IRQs sig-tx…msg-rx-discarded) is the same downstream `qpnp-pdphy` IP as pm8150b's, so the
pdphy half is probably reusable with a new compatible. The **port** half is not: on PM660/PMI8998 Type-C CC detection lives in
the SMB2 USBIN block (TYPE_C_STATUS_1..5 0x30B–0x30F, TYPE_C_INTRPT_ENB_SOFTWARE_CTRL 0x368, type-c-change IRQ in usb-chgpth),
not in a separate typec peripheral, and VBUS/VCONN come from the charger (smb2-vbus/smb2-vconn). PD would need: a new
qcom_pmic_typec port backend on the SMB2 registers, VBUS/VCONN regulators in qcom_smbx, TCPM sink caps (stock 5 V/3 A, 9 V/3 A),
charger coupling (USBIN_ADAPTER_ALLOW for the negotiated voltage, ICL from the contract, APSD off while PD is active) and
PD-vs-HVDCP arbitration (stock holds PD until the HVDCP check timeout). Estimate ≥ 1.5 k lines + DT + several attended sessions,
with a USB-C PD charger. **Decision: document only; QC (HVDCP) first.** 9 V/2 A over QC ≈ stock fast-charge power anyway.

## 7. ROM status / next
- **Not in the ROM**: the hvdcp patch is not in `kernel/rom-v2/series`; r4 ships the fcc-jeita qcom_smbx (merge-20260928).
  After an attended QC PASS: add `power/smbx/qcom_smbx-a6l-hvdcp.patch` after the fcc-jeita patch in the series, decide the
  ROM default (`hvdcp_enable=0` until the guard handles risk 4), `charger.txt` params, then the guard change (risk 4).
- Files: `kernel/power/smbx/qcom_smbx-a6l-hvdcp.patch`, `power/bundle/run-power.sh` (MODE=qc/qc-mon/qc-status, chg-off turns
  HVDCP off first), `power/test/sim-power.sh`, `power/test/hvdcp/{shim.h,sim-smb2.c,run-hvdcp-sim.sh,linux/*}`,
  `tools/build-power28.sh`, bundle `firmware/extracted/power-20260928/`, laptop `v75/power28`.
