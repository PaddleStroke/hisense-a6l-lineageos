# SUMMARY — Why the PM660 pulse is not felt, and what to do next

**Date:** 2026-09-18 · **Scope:** offline analysis only. No phone/laptop touched;
nothing built or flashed. See `EVIDENCE.md` for paths, hashes and sources.

## Bottom line
The corrected brake parsing (R4) was a real fix but, as V46 showed, not the
reason for silence. The deeper reason is structural: **the port drives a PM660
LRA with the pmi8998 mainline `qcom-spmi-haptics` driver, in direct mode, and
that driver never programs several PM660-specific drive parameters the stock
`qpnp-haptic` driver does.** A command can therefore "pass" (EN/PLAY/VMAX all
latch, no error) while the actuator barely moves.

## Ranked causes (confirmed discrepancies first, then their weighting)
All of D1–D6 are **confirmed from source**; their *causal weight* is the
hypothesis the diagnostic is designed to settle.

1. **D1 — Internal-PWM drive carrier never programmed (regs 0x56/0x58).**
   Stock sets 505 kHz; the mainline driver has no such register at all. In
   direct-mode LRA drive this carrier synthesises the waveform, so an
   unprogrammed PM660 carrier is the most plausible single cause of a
   latching-but-imperceptible pulse. *Highest.*
2. **D6 — Test requested only ~1276 mV vs stock 3200 mV.** A deliberately low
   helper magnitude (32%). On its own would give a weak-but-usually-felt pulse;
   combined with D1/D2 → nothing. *High, and cheap to test.*
3. **D2 — Current limit 400 mA vs stock 800 mA** (mainline hardcodes 400). Halves
   drive current. *Medium-high.*
4. **D3 — Auto-resonance mode ZXD_EOP vs stock "qwd", no `correct-lra-drive-freq`
   / clk-trim correction.** PM660 supports only zxd/qwd; wrong tracking saps LRA
   output over the pulse. *Medium.*
5. **D4 — VMAX overdrive left disabled** (driver `TODO: pm660 …`). *Low.*
6. **B0 — Brake u32/byte bug.** Already fixed (R4) and shown insufficient. Not a
   current cause. *Resolved.*

## Confidence
- High that D1–D6 are true discrepancies (direct source/DT reads).
- Medium on which is *decisive*. D1 is the leading single cause; realistically
  D1+D2+D6 compound. Not proven without the register readback below.

## Proposed next action
**Run one bounded, read-centric diagnostic before any code change** —
`diagnostic/haptic-drive-readback.sh` (+ `TEST-PROCEDURE.md`). It reads the live
haptic registers to show what actually took effect, fires the existing pulse
while sampling STATUS/VMAX/PLAY, then re-fires once at a **stock-bounded** ~3132
mV (< 3200) to separate "voltage too low" (D6) from "drive path broken" (D1/D2),
and offers an optional, gated, stock-legal register poke that tests the
INT_PWM+ILIM hypothesis **without building anything**. One attended run
distinguishes all four live hypotheses. It never exceeds stock limits and is
read-only unless the poke stage is explicitly armed.

**If, and only if, the readback confirms D1/D2**, apply the prepared candidate:
- `patch/0001-spmi-haptics-pm660-int-pwm-ilim.patch` (against the R4 driver) —
  adds opt-in INT_PWM/PWM_CAP programming and a DT current-limit read; brake
  logic and all validated defaults unchanged.
- `patch/a6l-haptics-candidate.dtso` — adds `qcom,int-pwm-freq-khz=505` and
  `qcom,ilim-ma=800` only.
Both are **untested on hardware**; build via the existing R4/R5 QEMU-checked
flow, then re-run the diagnostic. Isolated encoding check already passes
(`isolated-test/`, `A6L_INT_PWM_ILIM_TEST_PASS`).

## Deliberately not proposed
No amplitude/duration bump as a "fix". The one higher-voltage pulse in the
diagnostic is a bounded *measurement* at the stock operating point, clearly
labelled, not a workaround.

## Unresolved questions
- The PM660 **reset default** of INT_PWM (0x56) is unknown from artifacts; the
  readback resolves whether it is already 505 kHz (which would demote D1).
- D3 (qwd auto-res + drive-freq correction) is not expressible against the
  current mainline driver and cannot be poked; pursue a driver-side auto-res
  change only if D1/D2/D6 are excluded.
- Whether Android's vibrator HAL adds its own gain/effect mapping is out of
  scope here (framework agent owns it); this analysis covers the kernel path.

## Deliverables in this folder
- `SUMMARY.md`, `EVIDENCE.md`
- `patch/0001-spmi-haptics-pm660-int-pwm-ilim.patch`, `patch/a6l-haptics-candidate.dtso`
  (+ `candidate.c.orig`/`candidate.c.new` for review)
- `isolated-test/int_pwm_ilim_encoding_test.c` (+ built `t`) — passes
- `diagnostic/haptic-drive-readback.sh` + `diagnostic/TEST-PROCEDURE.md`
  — **NOT EXECUTED ON HARDWARE**
