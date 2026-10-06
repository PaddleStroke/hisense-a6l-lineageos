# Quick Charge retest prep (power29), 29 Sep 2026

This follows up the failed QC1 test (`docs/fastcharge-20260928.md` §4). Agent power29 worked **offline only**: no phone,
no adb, no ROM image, nothing deleted.
Result: the root cause is found in the driver and fixed, a probable physical cause (the USB gadget D+ pull-up) is handled
by the new test script, and the bundle `power-20260929` builds and passes all checks (`A6L_POWER29_BUILD_PASS`).
**The laptop was unreachable** (ssh to 192.168.1.22 timed out twice), so `v75/power29` is not staged yet.
Run `tools/stage-power29-laptop.sh` in WSL before the attended test.

## 1. What the 29 Sep QC1 logs show
Logs (clean copies): `research/volte6-20260929/a6l-qc.log.clean` (Huawei SuperCharge) and `a6l-qc-stock.log.clean`
(Hisense stock 9 V/2 A).
- Before plug-in, the monitor was armed on the laptop: `hv[... enable=1]` on SDP. So HVDCP_EN was already set **before**
  the wall charger was plugged in, which means "HVDCP enabled too late" was not the cause.
- Unplug: `hv[state=idle usbin_uV=0 usbin_uA=0]`, so the HVDCP work ran and saw the cable go.
- Wall charger: `chg[type=... [DCP] icl=800000 usbin_uA=755136]`, while `hv[state=idle ... usbin_uV=5136718 usbin_uA=469658 icl_uA=0]`
  stays **frozen for the whole ~2–3 min**.
  - The hv values differ from the live chg values and never change, so the work ran **once**, right after the plug-in
    (current still ~470 mA), and never again.
  - `icl_uA=0` means it never reached the DCP branch, which would have set the ICL.
  - There was no `A6L_HVDCP` line in dmesg: no rerun, no "plain DCP" timeout, no QC.
- Identical signature on both chargers.

## 2. Root cause
### 2a. Driver (power28 hvdcp patch). Confirmed by a host reproduction.
1. **Type mismatch.** Mainline `smb_apsd_get_charger_type()` reports `DCP` for **DCP, OCP and FLOAT** APSD results
   (APSD_RESULT 0x1308 bits 3/1/4), so the psy said `[DCP]`. The HVDCP work only accepted `DCP_CHARGER_BIT`.
   On OCP/FLOAT it did `break` with no log and no re-poll, leaving it `idle` forever.
2. **Stale poll window.** The IDLE APSD poll deadline was armed only when `hvdcp_enable` was written (6 s).
   On a later plug-in, one look at an unfinished or empty APSD result also ended with no re-poll.
- Reproduction: the power28 driver in the new model (`firmware/extracted/power-20260929/src/sim-power28-repro.c`) gives
  exactly the field status in both cases: `state=idle qc=0 ... icl_uA=0 enable=1 reason=-`.
  - Case 1: OCP result.
  - Case 2: result register 0 until 2 s after APSD start.

### 2b. Physical (probable, not yet proven): the D+ pull-up of our USB gadget
- In V75-usb recovery, `dwc3-qcom-legacy` forces the VBUS-valid override. `a6l_android_probe` writes
  `soft_connect=connect`, and the `a6lusbwd` watchdog keeps re-connecting (L1–L5 escalations every ~12 s when the UDC is
  not configured, which is always the case on a wall charger). So D+ carries the 1.5 kΩ pull-up to 3.3 V even on a charger.
- BC1.2 DCD/primary/secondary detection and the QC handshake need D+ free:
  - The PMIC drives D+ to 0.6 V for ≥ 1.25 s.
  - The QC adapter then opens its D+/D- short.
- With the pull-up, APSD most likely returns **OCP** (proprietary levels) or **FLOAT**, which fits 2a-1 exactly, and the
  HVDCP algorithm never starts.
- Stock only starts the USB peripheral for SDP/CDP (never on a DCP) and gives the charger a `dpdm-supply` (QUSB PHY).
- The next test decides it: the new `hvdcp_regs` dump shows the raw 0x1308 value.
  - `02` = OCP, `10` = FLOAT, which confirms the pull-up theory.
  - `08` = true DCP.
  - `28`/`48` = QC2/QC3.

## 3. Fix
### 3a. Patch `device/hisense/a6l/kernel/power/smbx/qcom_smbx-a6l-hvdcp.patch` (power29)
The power28 version is kept as `firmware/extracted/power-20260929/src/qcom_smbx-a6l-hvdcp-power28.patch`.
- **Detection window:**
  - Re-armed (10 s, `A6L_HV_PLUG_MS`) at every plug-in edge seen by the work and at every `status_change_work`.
  - APSD is polled every 200 ms until it finishes with a non-zero result.
- **Charger types:**
  - DCP, OCP and FLOAT are handled alike: ICL ≤ 1 A, then wait for QC or the timeout.
  - OCP/FLOAT logs `A6L_HVDCP APSD result 0x.. = OCP|FLOAT, not a BC1.2 DCP: D+/D- disturbed (USB pull-up?)`.
  - SDP/CDP are never touched (ADB safe).
- **Logging:** every APSD status/result change is logged: `A6L_HVDCP APSD status 0x.. result 0x.. (QC2 qc_charger auth_done hvdcp_timeout) opt1 0x.. state ..`.
- **QC_AUTH_DONE:** after QC_CHARGER the work waits up to 3 s for QC_AUTH_DONE (the downstream `hvdcp_3p0_auth_done`
  point) before using the QC2/QC3 bits.
- **New parameters:**
  - `hvdcp_rerun` (0200, write 1): one APSD rerun, only if `hvdcp_enable=1`, the state is idle/dcp-5v, USBIN < 6 V and the
    result is not SDP/CDP. At most 3 per plug-in. Otherwise it logs `rerun refused: ...`.
  - `hvdcp_regs` (0444): dump of 0x1306–0x1310, 0x1330/31, 0x1340, 0x1358, 0x135B, 0x1360, 0x1362/63/65/66/69, 0x1370,
    0x1607, 0x160B, 0x1006/07.
- **Status line:** `hvdcp_status` gains `apsd=0xSS/0xRR reruns=N` (before `reason=`).
- **Unchanged:** `hvdcp_enable` default 0 (register trace identical to r4), 5–9 V window (12 V never),
  `hvdcp_icl_ua` ≤ 2 A, 9.6 V absolute abort, FCC cap, JEITA hard limit, float 4.40 V clamp.

### 3b. `device/hisense/a6l/power/bundle/run-power.sh` (MODE=qc / qc-mon / qc-off)
- **MODE=qc:** stops `a6lusbwd` (`setprop ctl.stop`, marker `/tmp/a6l-qc-wd-stopped`) and dumps the registers.
- **Monitor sampling:** every 2 s, with `regs[...]` on every line.
- **Gadget pull-up (QDISC=1):**
  - `online=0` → `soft_connect=disconnect`, so the charger's first APSD sees free lines.
  - `[DCP]` → disconnect. If the driver sits in `dcp-5v`, exactly **one** `hvdcp_rerun`, logged as `QC_RERUN`.
  - `[SDP]`/`[CDP]` → `connect`, so ADB returns on the laptop.
- **End, ABORT or qc-off:** reconnect and restart the watchdog. The end summary is `QC_RESULT PASS|FAIL|NONE`.
- **Safety aborts unchanged:** T ≥ 50.0 C, < 0 C, Vbat > 4.42 V, USBIN > 9.6 V, USBIN I > QICL+10 %+100 mA ×2,
  Ibat > FCC+5 %+50 mA ×2, health; QICL ≤ 1.5 A, FCC ≤ 1.5 A (FORCE for more), QV ≤ 9 V, start refused ≥ 45 C.

## 4. Offline checks (`tools/build-power29.sh` in WSL → **A6L_POWER29_BUILD_PASS**)
| check | result |
|---|---|
| qcom_smbx.ko (upstream + fcc-jeita + hvdcp power29), clang r584948, W=1, v67 tree | 0 warnings; vermagic `7.2.3-a6l-probe+`; 0 unresolved; params + `hvdcp_rerun`, `hvdcp_regs`; sha256 **b7612370…** |
| `power/test/hvdcp` host test (ASan/UBSan) | **A6L_HVDCP_SIM PASS (76 checks)**: all 58 power28 checks plus the new cases (below) |
| hvdcp_enable=0 write trace vs r4 | **A6L_HVDCP_OFF_TRACE SAME** (21 writes) |
| `power/test/sim-power.sh` (dash) | **A6L_POWER_SIM PASS** with 13 new p29 cases (below) |

New host-test cases:
- The QC1 field case: armed on SDP, then a QC2 charger with the pull-up giving OCP.
  - power29 ends in `dcp-5v` (not a silent idle), with the APSD log, the warning, `apsd=0x01/0x02` and the regs dump.
  - With the pull-up removed, `hvdcp_rerun` → QC2 active 9 V with ICL ≤ 1 A at the request.
- The FLOAT variant: rerun → QC3 active, 20 pulses.
- `hvdcp_rerun` refused while active and on SDP.
- The limit of 3 reruns, reset by unplug.
- Late APSD result → QC2 active.

New sim-power cases:
- SDP untouched.
- Unplug → disconnect.
- dcp-5v → exactly one rerun.
- SDP → connect.
- regs on every line.
- `QC_RESULT FAIL`/`PASS`.
- Watchdog restarted at end, on abort and in qc-off.
- 9.8 V and 51 C aborts still work.

Bundle `firmware/extracted/power-20260929/`:
- power-20260928 files (same hashes), **qcom_smbx.ko b7612370…**, **run-power.sh 58f9bfdb…**.
- `src/` (both patches, the power28 patch, the reproduction), logs, `SHA256SUMS`.
- Laptop: **pending**, `tools/stage-power29-laptop.sh` → `~/A6L-usb-20260915/v75/power29` + `sha256sum -c`
  (`A6L_POWER29_LAPTOP_OK`).

## 5. Attended test QC2 (Pierre present; CURRENT/VOLTAGE-AFFECTING from `MODE=qc` on)
**Setup:**
- Hisense stock 9 V/2 A charger. The Huawei one is FCP, not QC, so plain DCP is expected there. Use it only as a control.
- Battery ideally ≤ 80 %. At 97 % the 29 Sep test drew almost nothing.
- Battery < 45 C.
- **Fresh V75-usb recovery boot:** qcom_smbx must not be loaded yet (an older build → `refused`).

```
cd ~/A6L-usb-20260915 && (cd v75/power29 && sha256sum -c SHA256SUMS)          # all OK
adb -s HLTE730T-PROBE push v75/power29 /tmp/power29
P='export PATH=/tmp/bin:$PATH; export D=/tmp/power29'
adb -s HLTE730T-PROBE shell "$P MODE=chg-ro; sh /tmp/power29/run-power.sh" 2>&1 | grep -v linker            # CHG_DT_OK, RRADC_OK
adb -s HLTE730T-PROBE shell "$P MODE=qc DUR=420; sh /tmp/power29/run-power.sh" 2>&1 | grep -v linker | tee qc2-start.txt
```
Expected start output:
- `USB watchdog a6lusbwd stopped`
- `HVDCP on: max 9000000 uV, ICL at 9 V 1000000 uA, FCC cap 1500000 uA`
- `regs 1306=.. ...`: 1362 has bits 0x44 set (HVDCP_EN + AUTH) and 0x20 clear, 1360=08, 135b=54
- `QC monitor started`

**Then: unplug the laptop cable, count to 5 (the monitor drops the D+ pull-up), plug the phone into the Hisense charger.**
- Leave it about 6 min. At the end the monitor switches back to 5 V on the charger.
- With a USB power meter: about 9 V within about 10 s (or after the one `QC_RERUN`, about 15 s).
- Then plug back into the laptop. ADB returns because the monitor reconnects the gadget on SDP. If it has not returned
  after 20 s, unplug and replug.

```
adb -s HLTE730T-PROBE shell "$P MODE=qc-status N=300; sh /tmp/power29/run-power.sh" 2>&1 | grep -v linker > qc2.txt
adb -s HLTE730T-PROBE shell 'dmesg | grep -E "A6L_HVDCP|A6L: |A6L_USBWD"' > qc2-dmesg.txt
adb -s HLTE730T-PROBE shell "$P MODE=chg-off; sh /tmp/power29/run-power.sh" 2>&1 | grep -v linker      # 5 V + suspend + watchdog back
```
If ADB does not come back after the test, long-press Power to reboot. The monitor ends at DUR = 7 min and reconnects anyway.

**PASS** (all of these):
- `A6L_HVDCP QC2|QC3 detected`, then `A6L_HVDCP ACTIVE QC.. usbin 8.4–9.6 V`.
- Log `QC_PASS HVDCP active ... usbin_uV≈9000000`.
- `QC_RESULT PASS`.
- `QCMON_DONE ... hv[state=off ... usbin_uV<6000000]` (9 V → 5 V).
- No `ABORT`.

How to read a non-PASS (send qc2*.txt):
- `A6L_HVDCP APSD ... result 0x02 (OCP)` / `0x10 (FLOAT)` **before** the disconnect, then `0x28`/`0x48` after
  `QC_RERUN`: the pull-up theory is confirmed and the fix works.
- `0x02/0x10` even after the rerun with the gadget off: the D+/D- path is disturbed by something else, probably the PHY
  itself. Next step is the QUSB2 PHY state or a `dpdm` equivalent (stock `dpdm-supply`); PHY power-down on DCP is the
  next experiment.
- `0x08 (DCP hvdcp_timeout)` and no `qc_charger` on the Hisense charger: HVDCP ran but the adapter did not answer.
  Check the `1362` bits (expect 0x44 set) and whether the charger really is QC (label "9V⎓2A").
- `qc_charger` without `auth_done` for 3 s: the log shows `QC.. without QC_AUTH_DONE`. It continues with the result bits.
- `QC_DRIVER_ABORT ... did not reach the window`: the adapter did not raise, or the rradc reads stale. See §5 risk 3 in
  fastcharge-20260928.

## 6. ROM path after a PASS
1. Add `power/smbx/qcom_smbx-a6l-hvdcp.patch` after the fcc-jeita patch in `kernel/rom-v2/series`.
2. **Pull-up on chargers:** the ROM has the same forced-VBUS dwc3 and an always-bound gadget, so QC needs the D+
   pull-up off on DCP/OCP/FLOAT.
   - Minimum: an init rule or the guard writing `soft_connect=disconnect` on `[DCP]` and `connect` on SDP/CDP/unplug.
   - Better: qcom_smbx notifying charger type → usb role/extcon (stock behaviour).
   - Without this, QC stays at 5 V (safe).
3. **`charger.txt`:** after the PASS and the guard change below, not before:
   `qcom_smbx.ko fcc_max_ua=2400000 jeita_hard=1 hvdcp_enable=1 hvdcp_max_uv=9000000 hvdcp_icl_ua=2000000`
   (stock: fcc 2.4 A, hvdcp-usb-icl 2.0 A).
   - Better: keep `hvdcp_enable=0` in `charger.txt` and let the guard write 1 only in the normal zone.
4. **Guard awareness of 9 V** (`a6l-chg-guard.sh`):
   - Warm/cool/hot or no telemetry → `hvdcp_enable=0` (5 V) **before** lowering the input limit, because at 9 V the same
     ICL gives about 1.8× the battery current.
   - Normal zone (10–45 C) → `hvdcp_enable=1`.
   - The guard's DCP budget (2.0 A) is also the stock hvdcp ICL.
   - Its `current_max` writes are clamped by the driver to `hvdcp_icl_ua` while QC is active.
   - Extend sim-power guard cases for this.
5. **Monitoring in the ROM:** log `hvdcp_status` changes, as the kernel already logs `A6L_HVDCP`. The driver aborts
   (> 9.6 V, sag, overcurrent, too hot) stay as they are.
6. Also an open question: stock `qcom,hvdcp-autonomous-enable` is not in the stock DT, so software pulsing matches stock.
   PD (pdphy) is still out of scope (fastcharge-20260928 §6).
