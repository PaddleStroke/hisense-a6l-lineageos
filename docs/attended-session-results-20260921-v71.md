# Attended session results — V71, 21 Sep 2026 (afternoon)

V71 (`417164b7…bcf9`) installed and left installed; stock intact; laptop services resumed. Logs on the laptop: `v71/logs/`.

## Milestones
- **Native display works.** MDSS/DPU + DSI0 + our FT8719 panel driver: modetest colour bars with `skip_init=1` AND with the
  full reset/init sequence. The 21 Sep "backlit black" was simply nothing drawing (no fbcon on the msm fb). The
  `mdp_clk_src: rcg didn't update` WARN still fires once and is harmless.
- **LineageOS on native display + GPU + touch**: boot completed in ~2 min, welcome screen, Start works, UI usable
  ("It works"). No GPU faults, no underruns, simpledrm copy worker gone.
- Motion sensors: `qcom_sns_reg` + phone's `sns.reg` → SMGR up → IIO accel, gyro, mag devices.
- Sound card registers ("Hisense A6L") with the dai-link order fix (0 route failures).
- TPS65185: binds without the GPIO holder; all six rails reach power-good (PG reg 0xfa, no faults), chip rev 0x66.
- E-ink video link: 196 frames at 85 Hz with 0 repeated frames.

## Findings / bugs fixed on the spot
| Finding | Fix |
|---|---|
| TPS65185 PWR_GOOD is NOT readable on TLMM gpio0 (stays low with all rails good, even with pull-up) | `tools/patch-tps65185-a6l.py`: poll PG register, mark it volatile (it was served from the regmap cache), 20 ms after WAKEUP (first probe used to NAK) |
| VCOM register defaults to 1.25 V after wake | must be set to 2.40 V each wake: today via `a6l_tps65185_step --vcom`; TODO DT constraints + `regulator_set_voltage` in panel-a6l-epd |
| TC358762 held in reset: upstream driver drives "reset" with inverted logic vs our `GPIO_ACTIVE_LOW` DT | module patched to real reset semantics |
| Panel rails only switch in `prepare`; fbdev keeps the DPI output enabled, so `hv` needs an off/on cycle | `eink-run.sh` cycles the CRTC; TODO proper API (sysfs or DRM property) |
| Recovery has no `/dev/dri`, `/dev/i2c-*`, `/dev/gpiochip*` | mknod from sysfs; TODO in bundle scripts. `mmcc-diag.sh` function `r` collides with a mksh alias → rename |
| adsp bundle on laptop still had awk + strict insmod | fixed script pushed (`v71/adsp_diag_r2-noawk.sh`) |
| **Recovery USB never enumerates on the recovery boot that follows a sysrq reboot** (3/3); after a long-press Power restart it works (4/4) | use long-press restarts; TODO find what survives the warm reboot |
| Phone hung once while reading `/sys/kernel/debug/gpio` / regmap after a failed rail enable | not reproduced; avoid full TLMM dumps after rail faults |

## E-ink: not drawing yet
With the bridge in reset (first runs) the panel saw floating lines: ink drifted weakly/noisily. With the bridge out of
reset: **XON (gpio61) high → no change at all; XON low (all gates on) → speckled darkening.** The TC358762 NAKs on I²C at
0x0b and 0x0f even out of reset with vddc (gpio45), gpio42 and gpio56 high, while TPS65185 (0x68) and a TMD3702 (0x49)
answer on the same bus. Conclusion: the bridge is not running (missing REFCLK / supply / reset timing / I²C enable?) or not
configured by the DSI generic writes. Next (offline): disassemble stock `tc358762` driver power-up (clocks, GPIOs, delays),
add an I²C init path identical to stock, verify by register read-back, then revisit byte order (XRGB vs XBGR) and XON.

## Other
- Front ALS: STK3338 never answers at 0x47 on c1b6000 even at 3.0 V; an **AMS TMD3702 answers at 0x49 on c176000** → try
  that sensor (stock DT has both nodes).
- Not run: modem/Wi-Fi, Bluetooth (time).

## Offline follow-up (same evening)

**Stock bridge bring-up order, from the stock kernel disassembly (`mdss_dsi_on`):** panel power GPIOs
(`mdss_epd_power_up`: three GPIOs at ctrl+0x914/918/91c = gpio42, 45, 56) → DSI clocks → DSI sw reset → bridge reset
sequence (low 10 ms, high 10 ms) → **clock lane forced HS** → **`tps65185_active_mode` (rails ON)** → `dsi2dpi_init` over I²C:
`tc358762_send_init_cmd` (12 writes @0x0b), `tc358762_read_id` (reg **0x04a0**, id = byte 1), and if the id says so the
**TC358767 table (25 writes @0x0f)**, both tables extracted from the ELF into `diagnostic/dsi2dpi_init.c`.
So stock talks to the bridge only with the e-paper rails already up and the DSI HS clock running. All my I²C probes were
done with the rails off. Next hardware test (T1): pipeline up, rails ON, `a6l_dsi2dpi_init id` → if it answers, `dump`,
then `init762`/`init767` and play. The `eink-draw` area now does this (`A6L_EPD_I2C=`), plus VCOM, XON and byte-order options.

**Speaker:** stock uses an **NXP TFA9894 (N1A1) smart amplifier** at I²C 0x34 (reset gpio76, irq gpio77, container
`tfa98xx.cnt`). Mainline only has `tfa989x` for TFA9895/9897; the speaker needs a TFA9894 driver port (out-of-tree NXP v6
driver exists). Earpiece/headset/mics go through the internal pm660l codec, which now registers — first Android audio target.

**Light sensor:** TMD3702 has no mainline driver (tsl2772 family is different); needs a small IIO driver or a userspace HAL.

## Third phone session (same day, evening): bridge truth + plain-DSI driver

- I²C scan with rails on: bus c176000 answers only at 0x1d (smb1351), 0x49 (TMD3702), 0x60 (unknown, regs `83 83 82 a8 01 81`),
  0x68 (TPS65185); bus c1b6000: 0x0e (unknown), 0x34 (TFA9894). **No bridge at 0x0b/0x0f.**
- **Stock kernel log (adb bugreport, saved on the laptop `stock-bugreport/`)**: on every e-ink update stock logs
  `NACK slv_addr:0xb`, `tc358762_send_init_cmd, ret=-107`, `tc358762_read_id, id = 0x0`, `tc358767_send_init_cmd, ret=-107`,
  then `mdss_dsi_panel_on: ndx=1 cmd_cnt=0` — and the e-ink works. The DSI→panel chip therefore needs NO configuration;
  the mainline `tc358762` bridge driver (DSI generic writes) is the wrong model.
- Stock power order recovered (`mdss_dsi_panel_power_ctrl`, ctrl+0x910 XON, +0x914 epd_pwr, +0x918 vdcc, +0x91c i2c_en):
  ON = 42↑, **XON(61)↑**, tps power_on(gpio2), 45↑, 56↑, 5 ms, reset low/high 10 ms; OFF = reset, tps sleep, 56↓ 42↓ 45↓ XON↓.
- New driver `panels/panel-a6l-epd-dsi.c`: plain 2-lane RGB888 non-burst-sync-pulse DSI sink, stock reset order, TPS rails
  from `/a6l-epd-panel`, binds to the existing V71 node. DSI1 host registers verified against stock properties
  (VID_CFG0 0x10009030, timings, HS clock forced).
- Result: rails ON (ENABLE 0x3f, PG 0xfa, VCOM 0xf0), XON high, 85 Hz, zero repeated frames — **constant `aa` or `55` drive
  for 11 s each changes nothing**; only a slight speckled darkening appears around rail/reset transitions. Byte order
  (XRGB/XBGR) makes no difference. So the panel logic is not acting on our pixel data.
- Open leads: (1) what exactly stock's hwcomposer writes to fb1 (format/stride/offsets, whether the TCON buffer is
  post-processed) — disassemble the HWC e-ink path; (2) the 0x60 chip; (3) DSI PHY timing table from stock vs computed;
  (4) a register-level diff of the stock DSI1 state is impossible (no root), so lean on (1).
- Recovery USB no-show is NOT strictly tied to sysrq reboots (one warm chain enumerated fine).

## Fourth phone session (late evening): narrowing the e-ink data path

- Stock HWC `DrawEpd` swaps byte 0 and byte 2 of every TCON pixel before writing fb1; `EpdPanelOpen(..., 0)` leaves fb1 at its
  default format (mdss default RGBA_8888). So on the wire: **B = drive data, G = strobes, R = 0** (= DRM XRGB8888 with the TCON
  bytes copied unchanged). Stock `tps65185_active_mode` also writes UPSEQ0 = 0xE1 and ENABLE = 0xBF, VCOM from flash.
- DPU routing verified: crtc-1 → LM1 → CTL3 (CTL_TOP intf = INTF_2), INTF2 timing engine on, and the **INTF2 MISR signature
  changes with the data** (00 → 0x4ab86565, aa → 0xdfa789e5, 55 → 0x00379325): correct pixels leave the MDP towards DSI1.
  `DISP_INTF_SEL` reads 0 and is writable (DPU never programs it); setting INTF1/INTF2 = DSI changed nothing visible.
- DSI1 host registers and PHY timing values match the stock panel properties. XBL's DisplayDxe only knows the LCD, so the
  bootloader does not set up the e-ink path either.
- Decisive negative test: XON low or high, constant `aa`/`55` on either channel, rails ON for 6–11 s → no visible drive.
  The break is between the DSI1 pads and the panel: DSI1 PHY electrical output, the unknown bridge (it NAKs I²C in stock too),
  or a control line we do not know (unknown I²C chip at 0x60 on the same bus).
- Next ideas: (1) dump the full DSI1 PHY/PLL register space and diff against downstream `mdss_dsi_phy_14nm` programming for
  this panel's timing table; (2) check DSI1 ULPS/clamp state in `mmss_misc` (stock maps 0xc828000 for it); (3) find who talks
  to I²C 0x60 in stock (kernel strings / vendor HALs); (4) try 4-lane vs 2-lane lane-enable and `MIPI_DSI_MODE_VIDEO_BURST`.
