# E-ink display transport: DSI → Toshiba TC358762 → parallel RGB → panel (offline analysis, 20 Sep 2026)

Source: stock DT + stock kernel ELF (`tc358762_*`, `dsi2dpi_init`, data symbols `tc358762_cmd`, `tc358767_cmd`).

- Bridge: **Toshiba TC358762 DSI→DPI**, stock node `tc358762@0b` on `i2c@c176000` (same bus as the TPS65185); a second
  table for a TC358767 exists in the driver but the A6L DT binds `dz,tc358762` for both nodes. Power/enables:
  `dsi2dpi-vdcc-en` = TLMM gpio45, `epd-i2c-en` = gpio56, `epd-pwr-on` = gpio42, `epd-xon` = gpio61 (from the DSI panel node).
- Stock programs the bridge **over I²C**, 12 writes of {16-bit register, 32-bit LE value} (`tc358762_send_init_cmd`):

| reg | value | mainline `tc358762.c` name / mainline value |
|---|---|---|
| 0x047c | 0x00000000 | SYSPMCTRL (leave sleep) |
| 0x0210 | 0x00000007 | DSI_LANEENABLE: clock + 2 data lanes (mainline: per `dsi->lanes`) |
| 0x0164 / 0x0168 | 0x00000004 | PPI_D0S/D1S_CLRSIPOCOUNT (mainline 5) |
| 0x0144 / 0x0148 | 0x00000000 | PPI_D0S/D1S_ATMR (same) |
| 0x0114 | 0x00000003 | PPI_LPTXTIMECNT (same) |
| 0x0450 | 0x00000060 | SPICMR (mainline 0) |
| 0x0420 | 0x00000150 | LCDCTRL (mainline 0x00100150: differs in bit 20) |
| 0x0464 | 0x00000205 | SYSCTRL (mainline 0x040f) |
| 0x0104 | 0x00000001 | PPI_STARTPPI |
| 0x0204 | 0x00000001 | DSI_STARTDSI |

- The mainline driver `drivers/gpu/drm/bridge/tc358762.c` programs the same registers through DSI generic writes. Plan
  ("E2"): DSI1 host → `toshiba,tc358762` bridge → `panel-dpi` 384×725@85 (h 126/125/6, v 4/4/2, 24 bpp, 2 lanes,
  non-burst sync pulse, LP11 init, clock lane forced HS), with a small local patch to take the three differing register
  values (SPICMR, LCDCTRL, SYSCTRL) from the stock table. Fallback if DSI-side register access does not work on this
  board: a 40-line I²C init helper replaying the table above.
- Prerequisite: the MSM DRM display driver must own MDSS, which also means a native driver for the main LCD
  (FT8719 Tianma 1080×2340, init sequence available in the stock DT) — otherwise the LCD goes dark when `msm.ko` takes
  over from the boot framebuffer. That panel driver is the next display work item and also fixes screen off/on.
- Update cadence: the TCON library produces 384×725 XRGB frames (see `eink-swtcon-abi-20260919.md`); a small daemon flips
  them at 85 Hz via DRM atomic while TPS65185 rails are up (E1), then powers the rails down.
