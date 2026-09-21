# Attended session results — 21 Sep 2026 (V69 + V70 recoveries)

Phone left on stock Android, laptop host services resumed. Raw logs: `logs/attended-20260921/` (local only, git-ignored).

## Works on hardware

| Item | Result |
|---|---|
| V69 / V70 recovery install + boot | OK (EDL write + full read-back each time) |
| GPU (Adreno 512, `msm.ko separate_gpu_kms=1`) | `card0` + `renderD128`; Mesa freedreno GLES 3.1 drives SurfaceFlinger. Setup-wizard activities draw in 1–2 s instead of 10–23 s |
| TPS65185 e-ink PMIC | Binds once TLMM gpio42 (`epd_pwr_on`) and gpio56 (`epd-i2c-en`) are held high. Without them: ENXIO |
| Rear touch (ft5x06 id 0x82, 3-0038) | event4, ~10 taps counted correctly by the user |
| E-ink NOR | Read and backed up (MX25U4033E), VCOM 2400 mV |
| ADSP | Boots; QMI services 64, 66, 43, 51, 24, 769 on node 5 |
| Audio DSP stack | APR, q6afe/q6adm/q6asm, LPASS digital codec, pm660l analog codec all probe |

## Does not work yet

**Native display (D1/E2).** msm master binds (needs tps65185 + tc358762 loaded with GPIOs held). DSI-1 1080x2340 and DPI-1 384x725 both "connected/enabled", but the LCD stays backlit black and the e-ink does not change. Kernel WARNs: `mdp_clk_src: rcg didn't update its configuration` then `pclk0_clk_src: ...` during `msm_dsi_host_power_on`. clk_summary shows mmpll5 → mdp_clk_src 330 MHz and dsi0pll → pclk0 171 MHz as enabled in the framework, so the hardware is not following. Leads, in order:
1. `mmcc` has no CX power-domain / performance vote in sdm630.dtsi; MMPLL5 at 825 MHz may not lock at the boot corner.
2. RCGs were left running by the bootloader's splash (simpledrm was live when msm loaded). Try parking the RCGs on XO first, or loading msm with simpledrm never bound.
3. The "A6L genpd preserving boot provider mdss" hack keeps the GDSC in its boot state; check that the MDSS AHB/AXI branch clocks really toggle.

**Sound card.** After adding `pinctrl-lpass-lpi` + `pinctrl-sdm660-lpass-lpi`, `snd-sm8250` probes but `failed to instantiate card -19`. q6routing prints a route failure for every LPI_MI2S_RX_* mixer (noise, same on other boards). -19 points at a DAI link or audio-routing widget in `a6l-audio-internal.dtso` that does not exist. `asoc.txt` has the registered components/DAIs to compare against.

**Front ALS STK3338.** ENXIO at 0x47 — probably also behind a supply or enable GPIO; check the stock DT for its regulator.

**Motion sensors.** No SMGR service on the ADSP: needs the sensor registry (sns.reg) served over rmtfs/QMI before the DSP starts sensors.

## Lessons for the bundles

- GPU `run.sh` must pass `separate_gpu_kms=1`; never `rmmod msm` (oops in `adreno_remove`).
- gpio42/gpio56 must become proper DT (regulator-fixed or hogs) instead of `a6l_gpio_hold`.
- Guards/launcher must accept the v70 marker.
- A fresh recovery boot is required before each framework run (BPF leftovers).

## Not run

Modem/Wi-Fi and Bluetooth (RF; need explicit approval at run time).
