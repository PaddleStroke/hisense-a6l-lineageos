# Audio + light-sensor prep — 23 Sep 2026 (audio agent, offline only)

Nothing here was run on the phone. Everything is built against the V67 phone kernel (`7.2.3-a6l-probe+`,
`out-a6l-phone-v67`) that the V71 recovery runs, and is prepared for an attended session.

## Summary

| Item | State | Artifact |
|---|---|---|
| First playback (earpiece, wired headset) + mic capture | **Prepared**, bundle `v74/audio` | static tinyalsa, -30 dBFS tones, mixer files, `run.sh` |
| Speaker (TFA9894) | **Module builds** (NXP v6.7.14 ported to 7.2), not testable yet: needs DT node + TERT MI2S DAI link + LPI pinctrl functions | `snd-soc-tfa98xx.ko`, `tfa98xx.cnt` |
| Light/proximity (TMD3702) | **New minimal IIO driver builds**, bundle `v74/als` | `tmd3702.ko` |

## 1. First playback test (bundle `v74/audio`)

### What the stock phone does (source of the mixer settings)
The stock card is `qcom,model = "sdm660-snd-card"` → the HAL uses `vendor/etc/mixer_paths.xml` (the one with
`TFA Profile`, i.e. the Hisense file; `mixer_paths_mtp.xml` is the generic QTI one with WSA controls). Relevant paths:

| Stock path | Stock controls |
|---|---|
| `deep-buffer-playback` | `INT0_MI2S_RX Audio Mixer MultiMedia1 = 1` |
| `handset` (earpiece) | `RX1 MIX1 INP1 = RX1`, `RDAC2 MUX = RX1`, `RX1 Digital Volume = 84` (raw, = 0 dB), `EAR PA Gain = POS_6_DB`, `EAR_S = Switch` |
| `headphones` | `INT0_MI2S_RX Channels = Two`, `RX1/RX2 MIX1 INP1 = RX1/RX2`, `RDAC2 MUX = RX2`, `RX HPH Mode = HD2`, `COMP0 RX1/RX2 = 1`, `HPHL/HPHR = Switch` (defaults `HPHL/HPHR Volume 9`) |
| `audio-record` | `MultiMedia1 Mixer INT3_MI2S_TX = 1` |
| `headset-mic` → `adc2` | `ADC2 MUX = INP2`, `DEC1 MUX = ADC2`, `ADC2 Volume = 8/6` |
| `handset-mic` → `adc1` | `DEC1 MUX = ADC1`, `ADC1_INP1 Switch = 1`, `ADC1 Volume = 8` |
| `speaker` | `TERT_MI2S_RX Audio Mixer MultiMedia1 = 1` + `TFA Profile = music` (TFA9894 on tertiary MI2S) |

### Translation to mainline control names (from the 7.2 sources)
* Back ends: stock `INT0_MI2S_RX` = mainline AFE port `LPI_MI2S_RX_0` (137), stock `INT3_MI2S_TX` = `LPI_MI2S_TX_3` (144)
  (these are the ports in `a6l-audio-internal.dtso`; `sm8250.c` sets INT0/INT3 IBIT clocks for them).
* q6routing: `LPI_MI2S_RX_0 Audio Mixer MultiMedia1`, `MultiMedia2 Mixer LPI_MI2S_TX_3` (capture on MultiMedia2 so
  MultiMedia1 stays the playback FE).
* msm8916-wcd-digital: `RX1/RX2 MIX1 INP1` (enum ZERO/IIR1/IIR2/RX1/RX2/RX3), `DEC1 MUX` (ZERO/ADC1/ADC2/ADC3/DMIC1/DMIC2),
  `CIC1 MUX` (AMIC/DMIC), `RX1/RX2 Digital Volume` (**S8, value in dB, −84…+40**; stock "84" raw = 0 dB).
* msm8916-wcd-analog: `RDAC2 MUX` (RX1/RX2), `EAR_S`, `HPHL`, `HPHR` (ZERO/Switch), `ADC2 MUX` (ZERO/INP2/INP3),
  `ADC1/2/3 Volume` (0…8, 6 dB steps).
* **No mainline equivalent** (dropped): `EAR PA Gain`, `EAR PA Boost`, `HPHL/HPHR Volume`, `RX HPH Mode`, `COMP0 RX1/2`,
  `INT0_MI2S_RX Channels`, `ADC1_INP1 Switch`. Consequence: EAR/HPH analog gain is the driver's fixed default.
  All names are assumptions from source until `MODE=dump` confirms them on the phone.

Mixer files (`control|value`, applied with read-back): `device/hisense/a6l/audio/v74-test/mixer/`
`common-off.txt`, `earpiece.txt`, `headset.txt`, `headset-mic.txt`, `handset-mic.txt`.

### Hearing safety
* Tone files are **−30 dBFS** 1 kHz, 50 ms fade in/out (no click), 2–3 s.
* Codec digital gain: `LEVEL=1` −30 dB (default), `2` −24 dB, `3` −18 dB. `run.sh` refuses any Digital Volume above
  −18 dB, **reads every volume back** and, if the read-back is not ≤ −18 dB, mutes (−84) and aborts.
  LEVEL 1 = −60 dBFS at the DAC, ~60 dB below stock full scale.
* `tinymix` is called as `tinymix -D <card> -- "<name>" <value>`: this AOSP tinymix uses `getopt_long`, so a negative value
  without `--` would be parsed as an option.
* The first run of each output: earpiece ~10 cm from the ear, headphones held in the hand, not worn.
  3-second announcement before each tone. Everything is set back to `common-off.txt` after each test.

### Attended procedure (V71 recovery, ADSP running from the `adsp` bundle)
```
adb push v74/audio /tmp/audio
D=/tmp/audio MODE=dump sh /tmp/audio/run.sh      # no sound; loads the V71 audio module set if the card is missing
adb pull /tmp/audio-out/tinymix-controls.txt     # keep it: the real control list
```
Expect: `A6L_AUDIO card=0 playback_dev=0 capture_dev=1` (numbers from /proc/asound/pcm), `A6L_MIXER_NAMES_MISSING=0`.
If names are missing: `cp -r /tmp/audio/mixer /tmp/audio-mixer`, fix the names there from `tinymix-controls.txt`, re-run
`dump` with `MIXDIR=/tmp/audio-mixer` (the bundle stays hash-checked; the −18 dB cap and read-back also apply to MIXDIR).
```
D=/tmp/audio MODE=earpiece sh /tmp/audio/run.sh              # LEVEL=1; ask Pierre: heard? loudness?
D=/tmp/audio MODE=headset sh /tmp/audio/run.sh               # headset plugged, NOT worn: left, right, both
D=/tmp/audio MODE=earpiece LEVEL=2 sh /tmp/audio/run.sh      # only if LEVEL 1 was inaudible
D=/tmp/audio MODE=mic sh /tmp/audio/run.sh                   # 5 s headset mic, 5 s main mic; speak/tap
adb pull /tmp/audio-out/ . ; python3 v74/audio/wav-level.py audio-out/cap-*.wav   # on the laptop
D=/tmp/audio MODE=off sh /tmp/audio/run.sh                   # silence everything
```
Watch in the output: `A6L_MIXER … fail=0`, the DAPM lines during playback (`dapm EAR PA: EAR PA: On` or `HPHL PA: On`,
`RX1 INT: On`), `A6L_PLAY_RC … 0`; in the klog tail: q6afe/q6asm errors (e.g. `AFE port start failed`, `-110` timeouts).
Pass = Pierre hears a quiet 1 kHz tone from the right transducer; mic pass = non-zero RMS that rises when speaking.

### Known risks / unknowns
* No MBHC (headset detection) in the mainline pm660l path: the headset test plays blindly; HPH may click.
* `EAR PA` in mainline enables `EAR CP` + both HPH DACs; stock set `EAR PA Gain +6 dB` — we stay at the driver default.
* If `tinyplay` fails with `-EINVAL` on hw_params: check `tinypcminfo` output (`/tmp/audio-out/tinypcminfo.txt`);
  q6asm FE should accept 48 kHz/16-bit/2ch.
* Capture: LPI_MI2S_TX_3 has `qcom,sd-lines = <0 1>`; `run.sh` tries stereo then mono.
* Headset mic bias is "MIC BIAS External2" via the DT routing (`AMIC2`), stock uses an external-cap micbias2 — untested.

## 2. Speaker: NXP TFA9894 (I²C 0x34 on c1b6000 = stock `i2c6`)

Stock: `tfa98xx@34 { compatible = "nxp,tfa98xx"; vdd-supply = pm660_l13 (1.8 V); reset-gpio = <&tlmm 76 0>;
irq-gpio = <&tlmm 77 0>; }`, driver = NXP/CAF tfa98xx (strings: `sound/soc/codecs/tfa9894/tfa_dsp.c`, "TFA9894 detected"),
container `/vendor/firmware/tfa98xx.cnt` (12 697 B, sha256 `0ecfff4c…085c`, = `firmware/extracted/vendor/firmware/tfa98xx.cnt`).
Stock audio back end for the speaker: **TERT_MI2S_RX** (`audio_platform_info.xml`: `SND_DEVICE_OUT_SPEAKER interface="TERT_MI2S_RX"`),
pins **LPI gpio4 (SCK), gpio5 (WS), gpio6 (SD0), gpio7 (SD1), function "func4"** (stock `lpi_pinctrl@15070000` `ter_mi2s_*`).

Options assessed:
* Mainline `tfa989x` (TFA1 family: 9895/9897/9890-ish, no DSP container): TFA9894 is a **TFA2** device (different register
  map, DSP firmware from the container) → extending tfa989x is a large job; not chosen.
* **NXP out-of-tree v6 driver** (github.com/msm8916-mainline/tfa98xx, branch DIN_v6x, CAF v6.7.14, knows TFA9894 N1/N2):
  chosen. Ported to 7.2 with a small compat layer (`device/hisense/a6l/kernel/tfa98xx-a6l/`: `a6l_compat.h`,
  `linux/of_gpio.h` shim on gpiod, `apply-a6l-patches.sh`: kbuild flags, `symmetric_rate(s)`, const `bin_attribute`,
  `snd_soc_component_to_dapm`, `snd_kcontrol_chip`, 1-arg I²C probe / void remove, `FW_ACTION_UEVENT`, `CBC_CFC`,
  `SLAB_MEM_SPREAD`, `devm_gpio_free` no-op, extra compatible `nxp,tfa98xx`). **Builds cleanly** (warnings only:
  enum conversions in tfa_dsp.c). Never loaded. (J0SH1X/tfa98xx `mainline` branch is the same code + 2022 fixes, also checked.)

Still needed before a speaker test (not done, needs the DT/overlay owner):
1. **LPI pinctrl**: mainline `pinctrl-sdm660-lpass-lpi.c` has no MI2S functions on gpio4–7 (groups 4–7 are `_,_,_,_`).
   Add functions (e.g. `ter_mi2s_clk` gpio4, `ter_mi2s_ws` gpio5, `ter_mi2s_data` gpio6/7) in slot 4 (stock "func4") and
   rebuild that module.
2. DT (overlay on the V71 base; labels as in the mainline tree):
```
&blsp2_i2c? /* i2c@c1b6000, stock alias i2c6 */ {
    speaker_amp: audio-codec@34 {
        compatible = "nxp,tfa98xx";          /* driver also accepts "tfa,tfa9894" */
        reg = <0x34>;
        vdd-supply = <&vreg_l13a_1p8>;        /* pm660 L13, 1.8 V (stock) */
        reset-gpio = <&tlmm 76 GPIO_ACTIVE_HIGH>;
        irq-gpio = <&tlmm 77 GPIO_ACTIVE_HIGH>;
        #sound-dai-cells = <0>;
        sound-name-prefix = "Speaker";
    };
};
&q6afedai { dai@18 { reg = <TERTIARY_MI2S_RX>; qcom,sd-lines = <0>; }; };   /* check SD line (stock SD0 = gpio6) */
&sound {
    /* place with the other MI2S links (after the MultiMedia links in the MERGED dtb, see the ordering note) */
    tert-mi2s-dai-link {
        link-name = "Speaker Playback";
        cpu { sound-dai = <&q6afedai TERTIARY_MI2S_RX>; };
        platform { sound-dai = <&q6routing>; };
        codec { sound-dai = <&speaker_amp>; };
    };
};
&lpi_tlmm { ter_mi2s_active: ter-mi2s-active-state { pins = "gpio4","gpio5","gpio6","gpio7"; function = "ter_mi2s…"; drive-strength = <8>; }; };
```
   `sm8250.c` already handles `TERTIARY_MI2S_RX` (TER_MI2S_IBIT at 1.536 MHz = 48 kHz × 16 bit × 2; CPU is clock master).
3. Firmware: `/lib/firmware/tfa98xx.cnt` (stock file, copied next to the module). The driver loads it asynchronously
   (`request_firmware_nowait`); DSP start happens on the first stream. Mixer: `TERT_MI2S_RX Audio Mixer MultiMedia1 = 1`,
   `TFA Profile = music` (name prefix may change it to `Speaker TFA Profile`).
Risks: a smart amp without its calibrated protection can damage the speaker; use the stock container unchanged, low level,
short tones. The chip's MTP holds the factory calibration (stock `tfa98xx_dbgfs_calib*`); do not run calibration.

## 3. Light / proximity: ams TMD3702 (0x49 on c176000 = stock `i2c2`)

* Mainline check: `tsl2772` (TSL/TMD 2x71/2x72/377x) uses the 0xA0 command-bit protocol and different registers;
  `tcs3472`/`tcs3414` are colour-only with command bits; `tmd2725`/`tcs3490` have no mainline driver. **No mainline driver
  covers TMD3702.** The stock kernel has a Hisense `tmd3702` driver (`ams,tmd3702`, polled ALS dwork + PS IRQ on gpio113).
* Register map from the ams TMD3702VC datasheet: direct addressing; ENABLE 0x80 (PON/AEN/PEN/WEN), ATIME 0x81 ((n+1)×2.78 ms),
  PRATE 0x82, WTIME 0x83, PERS 0x8C, PCFG0 0x8E (pulse len/count), PCFG1 0x8F (PGAIN, PLDRIVE 0–8 = 2–19 mA),
  CFG1 0x90 (AGAIN; **bit 5 must be 1**, else VCSEL current doubles), REVID 0x91, ID 0x92 (= 0x10), STATUS 0x93,
  C/R/G/B data 0x94–0x9B, PDATA 0x9C–0x9D (14-bit with APC), CFG4 0xAC (**must be 0x3D**), INTENAB 0xDD, TEST3 0xF2 (**must be 0xC4**).
* New driver `device/hisense/a6l/kernel/tmd3702/tmd3702.c` (polled IIO, no IRQ): checks ID, programs the stock DT timing
  values (ATIME 0x11 = 50 ms, AGAIN 16×, PRATE 0x32, WTIME 8, PERS 0x21, 30 pulses × 16 µs, PGAIN 1×) but **VCSEL drive
  10 mA** (stock DT says 9 = outside the documented table; module param `pdrive`, `prox=0` disables the emitter).
  Channels: `in_intensity_{clear,red,green,blue}_raw`, `in_proximity_raw`, `in_illuminance_input` (uncalibrated:
  C × 833 / (t_ms × gain), 833 = stock `als_dgf`; stock segment/IR coefficients not applied). Optional `vdd`/`vio` supplies.
* Stock DT: `als,position = "back"`, `ps,position = "back"` → this sensor faces the **e-ink side**; vdd = pm660 L13,
  vio = gpio64 fixed regulator (always-on). It already answered in V71 without us touching either.
* DT node (for later; the test binds it with `new_device` instead):
```
&blsp1_i2c? /* i2c@c176000, stock alias i2c2 */ {
    light-sensor@49 {
        compatible = "ams,tmd3702";
        reg = <0x49>;
        vdd-supply = <&vreg_l13a_1p8>;
        interrupts-extended = <&tlmm 113 IRQ_TYPE_EDGE_FALLING>;   /* unused by the polled driver */
    };
};
```

### Attended procedure (bundle `v74/als`)
```
adb push v74/als /tmp/als
D=/tmp/als sh /tmp/als/run.sh
```
Needs the c176000 I²C bus present (it was in V71 with the e-ink PMIC bundle). Expect `TMD3702 ID 0x10 REVID 0x..` in the klog,
`A6L_ALS_READ open… C=<n>` values, lower C and higher P while covered, much higher C under the torch, `A6L_ALS_READ_PASS`.
The script unbinds and unloads at the end. If the ID differs, the driver refuses (reload with `A6L_TMD_PARAMS=force=1` only
after reading the ID printed in the klog).

## Artifacts
All under `firmware/extracted/audio-20260923/` (full list with the copied V71 audio modules: `SHA256SUMS-all`):

| sha256 | file |
|---|---|
| `b91070219b2da4c8…` | `modules/snd-soc-tfa98xx.ko` |
| `0ecfff4cd31f2ff4…` | `modules/tfa98xx.cnt` |
| `d43161a7f80a14db…` | `modules/tmd3702.ko` |
| `65bd2c71422a2a58…` | `v74/als/SHA256SUMS` |
| `d43161a7f80a14db…` | `v74/als/modules/tmd3702.ko` |
| `90557dd6d5e86d43…` | `v74/als/run.sh` |
| `9f12f274925fb747…` | `v74/audio/SHA256SUMS` |
| `989c59127cfdd778…` | `v74/audio/bin/tinycap` |
| `03c78ad36979435d…` | `v74/audio/bin/tinymix` |
| `c2b13a1e2aeb8ffc…` | `v74/audio/bin/tinypcminfo` |
| `c964c0c2316b6bb3…` | `v74/audio/bin/tinyplay` |
| `fcf5301535c9c7f3…` | `v74/audio/mixer/common-off.txt` |
| `04491f6a257071a4…` | `v74/audio/mixer/earpiece.txt` |
| `de8ad047415aa638…` | `v74/audio/mixer/handset-mic.txt` |
| `38ab48228ae7fce3…` | `v74/audio/mixer/headset-mic.txt` |
| `f8796de7b5d9908d…` | `v74/audio/mixer/headset.txt` |
| `fba4f264b6553bea…` | `v74/audio/run.sh` |
| `e31e57e51559c756…` | `v74/audio/wav-level.py` |
| `4a46b86397a7d30e…` | `v74/audio/wav/sine1k-m30dBFS-left-2s.wav` |
| `3983a82278f1c128…` | `v74/audio/wav/sine1k-m30dBFS-right-2s.wav` |
| `800a571a218133cd…` | `v74/audio/wav/sine1k-m30dBFS-stereo-3s.wav` |

Full hashes: `firmware/extracted/audio-20260923/SHA256SUMS-all`. Key ones: `tmd3702.ko` d43161a7f80a14db4d99f2404a8bef7f0470eb971cbe141b0339c51b4f45a3c3, `snd-soc-tfa98xx.ko` b91070219b2da4c8ab5243387b12cd8c62a36561e2454c837eb0036dbb35d866, `v74/audio/run.sh` fba4f264b6553bea8139607ff74d27540a49c2293c5792a485faeac0e75bdb25.

Sources (repo): `device/hisense/a6l/kernel/tmd3702/tmd3702.c`, `device/hisense/a6l/kernel/tfa98xx-a6l/`, `device/hisense/a6l/audio/v74-test/` (run bodies, mixer files, `wav-level.py`), `tools/build-audio-sensors-v74.sh`. The bundles are the V71 run.sh header (image check `hisense,a6l-controls = v71`, SHA256SUMS) + the body. The audio bundle carries its own copy of the 35 V71 audio modules (same order.txt), so it works with or without the V71 `audio` area having been run.


Build: `tools/build-audio-sensors-v74.sh` (WSL; reproducible: tinyalsa from `external/tinyalsa` of the Lineage tree,
tfa98xx from msm8916-mainline/tfa98xx DIN_v6x @ d2cd129 + `apply-a6l-patches.sh`, tones generated in Python).
Laptop: `~/A6L-usb-20260915/v74/audio/` and `~/A6L-usb-20260915/v74/als/` (SHA256SUMS verified on the laptop).


## Offline validation done
* All three modules: vermagic `7.2.3-a6l-probe+ SMP preempt mod_unload aarch64` (= V67 phone kernel). tmd3702: 0 warnings.
  tfa98xx: 34 warnings (upstream enum-conversion/prototype noise), 0 errors. Two identical rebuilds gave identical hashes.
* tinyalsa tools: aarch64 static, stripped (AOSP `external/tinyalsa` of the Lineage tree, NDK r27c API 34).
* `run.sh` logic exercised with dash against a fake tinymix/tinyplay and fake `/proc/asound` (dump/earpiece/headset/mic/off,
  LEVEL 1–3 accepted, LEVEL 4 refused, a volume read-back of 0 dB → mute + abort rc=10). Real hardware behaviour untested.
* Laptop copies verified with `sha256sum -c` (`LAPTOP_AUDIO_OK`, `LAPTOP_ALS_OK`).
* Note: `device/hisense/a6l/kernel/tmd3702/Makefile` could not be written (protected path for the remote tools);
  the build script generates it (`obj-m += tmd3702.o`).

## Still unknown
* Real control names/enum texts (first `dump`), PCM numbering, whether the LPI MI2S back ends start without errors.
* Whether the earpiece/HPH analog defaults of the mainline msm8916 analog driver fit the pm660l (ear gain/boost).
* TMD3702 lux calibration; proximity threshold behaviour; whether the ID register really reads 0x10 on this part.
* TFA9894: never probed on mainline; TERT MI2S pin functions, SD line, and bit clock need the DT work above.
