# PM660 haptics drive-path diagnostic — procedure (NOT EXECUTED ON HARDWARE)

**Script:** `haptic-drive-readback.sh`. Runs later, by the main agent, over
authenticated ADB in the `7.2.3-a6l-probe+` RAM environment, attended. One
short run. Read-centric; no amplitude above stock 3200 mV; pulses ≤150 ms.

## Goal
Explain why the V46 corrected-brake 100 ms pulse produced **no perceptible
vibration** even though the command path returned success. Decide between four
non-exclusive causes without guessing.

## What it does
1. Gate on kernel `*a6l-probe+*`; locate the pmic@1 SPMI regmap in debugfs.
2. **Stage 1 (read):** load the *unchanged* R4 module, read the haptic register
   window `c000+{46,48,4B,4E,4F,51,52,54,55,56,58,5C,70}` — the *actual*
   programmed drive state.
3. **Stage 2 (read during play):** fire the unchanged ~1276 mV / 100 ms pulse;
   poll `STATUS_1 (0x0A)`, `EN (0x46)`, `LRA_AUTO_RES (0x4F)`, `VMAX (0x51)`,
   `PLAY (0x70)` during play; capture dmesg.
4. **Stage 3 (bounded voltage discriminator):** re-fire at ~3132 mV (magnitude
   88, just under stock 3200 mV), ≤150 ms. Ask the user if anything was felt.
5. **Stage 4 (OPTIONAL, gated `A6L_ALLOW_REG_POKE=1`):** write stock-legal
   `INT_PWM=505 kHz` (0x56/0x58) and `ILIM=800 mA` (0x52), re-fire the *low*
   pulse. Tests the drive-config hypothesis **without** building the patch.

## Register checkpoints and what they tell us (add base 0xc000)
| Off | Reg | Read it for |
|----|----|----|
| 0x0A | STATUS_1 | BUSY(bit1) set during play? SC_FLAG(bit3)=short? AUTO_RES_ERROR(bit4)? |
| 0x46 | EN_CTL | module enable bit7 latched during play |
| 0x4E | SEL | play-mode source bits5:4 — confirms DIRECT (0) actually set |
| 0x4F | LRA_AUTO_RES | auto-res mode/cal actually written (mainline ZXD_EOP vs stock qwd) |
| 0x51 | VMAX | effective drive voltage (expect ~11 steps low-V; ~27 strong) |
| 0x52 | ILIM | 0=400 mA (mainline default) vs 1=800 mA (stock) |
| 0x54/55 | RATE | resonance period (~1333 for 6667 µs) |
| 0x56/58 | INT_PWM/PWM_CAP | **expected unprogrammed by mainline — key evidence** |
| 0x5C | BRAKE | R4 brake byte (expect 0x0F from 3/3/0/0) |
| 0x0B/0C | AUTO_RES_LO/HI | measured resonance readback (LRA actually moving?) |
| 0x70 | PLAY | play bit7 latched |

## Expected outcomes per hypothesis
- **H1 drive carrier unconfigured (INT_PWM/PWM_CAP):** Stage 1 shows 0x56/0x58
  at a non-505 kHz reset value; Stage 4 poke makes the *same low-voltage* pulse
  perceptible → H1 confirmed as decisive. If poke changes nothing, H1 is not
  sufficient alone.
- **H2 voltage too low:** Stage 3 (3132 mV) is felt while Stage 2 (1276 mV) was
  not → the low test magnitude, not the driver, explained V46. VMAX(0x51) should
  read ~27 steps in Stage 3.
- **H3 current limit 400 vs 800 mA:** ILIM(0x52)=0 in Stage 1 confirms the gap;
  Stage 4 sets it to 1. Weakness that eases at 800 mA implicates H3.
- **H4 auto-res mode wrong (ZXD_EOP vs qwd)/no drive-freq correction:** 0x4F
  mode field differs from stock qwd; AUTO_RES_LO/HI (0x0B/0x0C) stay 0 or wild
  during play → LRA not tracking resonance. Not directly pokable here; if H1–H3
  are excluded and drive still fails, escalate to a driver auto-res change.
- **If BUSY never sets / PLAY never latches / SC_FLAG sets:** the fault is
  enable/short-circuit, not tuning — STOP and preserve logs.

## Limits, stop conditions, cleanup
- Never exceed 3200 mV or 150 ms. Read-only unless Stage 4 is explicitly armed.
- STOP on any "Short circuit"/"disabling haptics" dmesg or SC_FLAG set.
- If the regmap debug write interface is uncertain, do **not** poke.
- Cleanup: `rmmod qcom_spmi_haptics`; remove /tmp nodes; return to stock Android
  and verify, as in the V46 sequence.

## Not covered / follow-up
- H4 (auto-res qwd + `correct-lra-drive-freq` + clk-trim-error) needs driver
  changes beyond a register poke; only pursue if H1–H3 are ruled out.
- This procedure does not enable Android's vibrator HAL; it is a bounded probe.
