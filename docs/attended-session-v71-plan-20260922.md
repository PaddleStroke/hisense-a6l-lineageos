# Attended session plan — V71 (prepared 21 Sep 2026, offline)

V71 = V70 image with the DT rebuilt for the 21 Sep findings. Same kernel Image and ramdisk. Candidate
`417164b7…bcf9`, captured-ABL emulation PASS, transition/protocol tests PASS, staged on the laptop (`v71/`).
One unified bundle: `firmware/extracted/v71-attended-bundle-20260922` (all areas, marker `v71`).

## What changed and why

| Finding (21 Sep) | Root cause | Fix in V71 |
|---|---|---|
| TPS65185 ENXIO without `a6l_gpio_hold` | gpio42 `epd_pwr_on` / gpio56 `epd-i2c-en` low | two always-on `regulator-fixed` nodes chained into `epd-pmic-vin` |
| Front ALS STK3338 ENXIO | pm660l L3 sat at 1.71 V (window 1.71–3.6 V, no consumer request) | L3 pinned to 3.0 V |
| Sound card `-19`, 584 q6routing route failures | **fdtoverlay reverses new child nodes**, so the MI2S links (q6routing) probed before the MultiMedia links (q6asm-dai); route failures are fatal | dai-links written in reverse in the source; `Prepare-RecoveryV71` asserts MultiMedia1 is first in the merged DTB. LPI pinctrl modules added to the audio area |
| No SMGR service on the ADSP | nobody served the Sensor Registry | tree already has `qcom_sns_reg.ko`; bundle ships this phone's own `sns.reg` (from the 14 Sep persist backup, RAM only) |
| GPU waited for the display chain; `rmmod msm` oops | msm loaded without `separate_gpu_kms=1` | `load()` always passes it; never rmmod |
| Guards refused V70 | marker allowlist | v68–v71 accepted (supervisor rebuilt, payload re-verified in the VM) |

## Native display: still open, now instrumented

The 21 Sep dmesg was captured 1 s after the takeover, so we do not know what happened afterwards. The two
`rcg didn't update` WARNs happen while the bootloader-left branch clocks keep the RCG roots ON and the new
parent is not running yet (DSI PLL is only started after `set_rate`); they may be harmless. The `display` area now captures,
under `/tmp/display-diag/`: MMCC registers before/after (PLL mode/L/status, RCG CMD/CFG, CBCRs, GDSC), DSI0 host and
INTF1 registers, DRM atomic state, modetest connector dump, full dmesg 12 s after the takeover.

Experiments, one per recovery boot, in this order (each is one env var):

1. `A6L_PANEL_PARAMS=skip_init=1 A6L_DISPLAY_PATTERN=1` — panel keeps the bootloader state (no reset, no DCS).
   Picture => our reset/init sequence is the problem. Black => the video stream is the problem.
2. `A6L_DISPLAY_PATTERN=1` — full init, with pattern, for the register/dmesg capture.
3. `A6L_DISPLAY_QUIESCE=1 A6L_DISPLAY_PATTERN=1` — gate the bootloader-left MDSS branches first so the RCG roots are off.
4. `A6L_MSM_PARAMS=prefer_mdp5=0|1` — the other KMS driver (DPU vs MDP5), only if 1–3 are black.

## Order of the session (about 8 recovery boots are NOT needed; most areas share a boot)

Boot A: `sensors-adsp PHASE=pre` → adsp (v68 bundle, leave running) → `sensors-adsp` → `audio` → `eink-pmic` → `touch` → `front-als` → `gpu` → display experiment 1.
Boot B: gpu → display experiment 2 (+ `eink-dsi`). Boot C: experiment 3. Then framework run with `A6L_EGL=mesa` on the best display state.
RF areas (`modem-wifi`, `bluetooth`) only with Pierre's explicit go at that moment.

## E-ink: first real draw is prepared

- New panel driver `panels/panel-a6l-epd.c` (DT compatible `hisense,a6l-epd-panel`): sequences TPS65185 v3p3 → ±15 V → VCOM
  around the video stream; `hv=0` by default, so the high-voltage rails stay off unless the draw step asks for them.
- Drive frames are computed OFFLINE: the stock software TCON ran in QEMU with this panel's own waveform
  (`Test-EinkSwtconQemu.py 6 <waveform> --dump=25`) → `update1-t25.a6lepd` (117-frame clear) + `update2-t25.a6lepd`
  (39 frames, grey bars). Checked: no illegal `11` drive code, both sequences end with no-drive frames.
- `a6l_epd_play` (static, libdrm) validates the files, modesets DPI-1 with an idle frame, flips one frame per vblank at 85 Hz,
  reports repeated frames, and always switches the CRTC off at the end (rails down).
- Area `eink-draw`: step 1 transport only (rails off), step 2 `A6L_EPD_HV=1` with Pierre watching the rear screen.
  Depends on the native display pipeline (msm KMS) being up, so it follows the display experiments.

Known side effect: `a6l_epd_nor_read` now gets EBUSY on gpio42 (owned by the regulator). The NOR is already backed up.
