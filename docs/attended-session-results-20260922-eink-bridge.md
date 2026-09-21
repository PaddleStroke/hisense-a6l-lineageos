# E-ink: rooted-stock tracing results and the real bridge (22 Sep 2026, attended evening session)

State at the end: phone in **rooted stock Android** (Magisk v30.7 in the real boot partition, `su` granted to shell), V71 in the
recovery slot, laptop host-control **resumed**. Nothing was written to the phone in this session except RAM payloads in recovery
and files under `/data/local/tmp` on stock.

## Findings (in order of importance)

1. **The bridge is a Toshiba TC358767 at I²C 0x0f, and stock DOES configure it.** On rooted stock, polling `/dev/i2c-2`
   while Pierre triggered e-ink updates: address 0x0f ACKs *only while INTF2 is enabled* (2/120 polls, exactly the active ones),
   0x0b never. `IDREG 0x0500 = 0x00006601`. Read-back during an update matches the stock `tc358767_cmd` table:
   `0448=86 0450=03f00100 0454=007d0005 045c=00040002 0510=1 0918=110 (REF_FREQ 19.2 MHz) 0914=11c201 0800=11100 06a0=3080
   0210=7 0134=7 013c=30005 0114=3 0164..0170=4`; also `0458=000400a0 0460=000800f0 0464=0 0104=0 0204=0 050c=ff 0508=0`.
   The earlier conclusion "stock never configures the bridge (ret=-107)" was wrong: the -107 lines are the 0x0b attempt /
   attempts while the chip is unpowered. Raw data: `research/eink-bridge-20260922/`.
2. **On our kernel 0x0f never ACKs**, even with DSI1 running (HS clock forced), vddc(45)/42/56 high and reset(12) high. So the
   remaining e-ink problem is the bridge being unpowered / unclocked / held in reset, not the DSI stream.
3. **Lead: REFCLK.** Stock programs `SYS_PLLPARAM` for a 19.2 MHz reference. Stock clock dump: `ln_bb_clk1_ao en=1` (idle and
   active) and `rf_clk1_pin en=1`; our `clk_summary` (21 Sep logs): every `ln_bb_clk*`, `rf_clk1*`, `div_clk1` has enable
   count 0 (mainline switches unused RPM clocks off). Hypothesis: the bridge REFCLK is PM660 LN_BB_CLK1. Experiment module
   `panels/a6l-clk-hold.c` (`insmod a6l-clk-hold.ko ids=80`) holds RPM-SMD clocks without a DT change.
4. **The darkening is XON.** Holding XON (gpio61) high *before* the rails rise: no darkening at all (Pierre, twice). XON low
   = all gates on while ±15 V/VCOM ramp → uniform speckled darkening. Driver now takes an optional `xon-gpios` (V72 DT).
5. **byte_intf clock:** stock runs `byte{0,1}_intf_clk` at byte_clk/2 (30.03 MHz for DSI1); mainline only sets
   `byte_intf_clk_div_2` for the 10 nm+ PHY timing functions (v3/v4), not `msm_dsi_dphy_timing_calc_v2` (14 nm). Forcing the
   MMCC divider (0x0c8c2380 = 1) live gave the stock rate but no visible change (bridge dead anyway). Keep as a to-do: proper
   fix is one line in `dsi_phy.c` (upstreamable); the LCD works either way.
6. Register comparison stock vs ours during an update (stock debugfs `mdp/off+reg`, `dsi1_ctrl`, `dsi1_phy`; ours `a6l_mmio`):
   INTF2 block identical; DSI1 controller identical except tuning values (`HS_TIMER_CTRL` abs 0xbc 0003fd08 vs 0000ffff,
   `CLKOUT_TIMING_CTRL` abs 0xc4 041c vs 0a20, `TRIG_CTRL` BLOCK_DMA bit, abs 0x50 = 0x30 vs 0) and status registers;
   `DISP_INTF_SEL` 0x00010000 (stock, active) vs 0 (ours; writing it changed nothing on 21 Sep). PHY lane timing from the
   stock DT vs ours: `1e 1c 04 06 02 03 04 a0` vs `1f 1c 04 06 03 03 04 a0` (clk lane `1e 0f 04 05 02 ..` vs `1f 0f 04 05 03 ..`);
   strength/regulator/lane-config identical. NOTE: DSI 6G register offsets in `dsi.xml.h` are +4 in absolute terms
   (EOT_PACKET_CTRL is abs 0xcc and reads 1 on both; an `eot=0` experiment was a misread and is reverted to default on).
7. Stock debugfs limits: `dsi1_*_off` ignores offsets (always 0..0x100); `regmap/c996400.qcom,mdss_dsi_pll/registers` returns a
   constant; no `/dev/mem`. `/sys/kernel/debug/gpio` on msm-4.4 prints the *function*, not the level.
8. The unknown chip at 0x60 reads like a FAN53555-class buck (`83 83 82 a8 01 81` = VSEL0/1, CONTROL, ID1, ID2, MONITOR);
   stock has no DT node for it. Left alone.

## Prepared for the next attended session
- `panel-a6l-epd-dsi.c`: `bridge=1` (default) writes the stock TC358767 table over I²C in `prepare` (LP-11) and retries in
  `enable`; logs `bridge (prepare|enable): IDREG ret=… id=…`. Optional `xon-gpios`. `eot` param (default on).
- `a6l-clk-hold.ko`, bundle `v71/bundle-r8` on the laptop (r6 + new panel module), `v72/br2.sh` on the laptop: probes 0x0f,
  walks clock ids 80 (LN_BB_CLK1), 81, 82, 94, 46, 14 until it ACKs, then re-prepares (table), rails on, replays our update and
  the stock capture. Needs: recovery, `prep.sh`, push `br2.sh a6l-clk-hold.ko a6l_dsi2dpi_init a6l_i2c16_read stock-eink.a6lepd`.
- If no clock makes it ACK: next suspects are the power ORDER (stock: 42↑, XON↑, TPS VIN gpio2, 45↑, 56↑, 5 ms, reset 0/10 ms/1/10 ms;
  ours has 42/56 always-on from boot) and pm660_l11 (`wqhd-vddio`).
- **Frontlight** (offline candidate, untested): `a6l-eink-frontlight.dtso` = PM660L LPG channel 4 (`pwm@b400`, 54 µs period,
  DTEST2) out on PM660L GPIO6 (`dtest2`), `pwm-backlight` "a6l-eink-frontlight", default 0. Needs `leds-qcom-lpg.ko` (already in
  modules.tar.gz) and a V72 recovery DT.
