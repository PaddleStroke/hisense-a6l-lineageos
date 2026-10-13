# Audio PCM-open diagnosis, TFA9894 speaker DT, TMD3702 proximity fix — 23 Sep 2026 evening (agent `audio2`, offline only)

Nothing here was run on the phone. Builds are against the V67 phone kernel (`7.2.3-a6l-probe+`) that V71 runs.
Previous report: `docs/audio-sensors-prep-20260923.md`.

## 1. "Device does not exist" on pcmC0D0p/c

### What the evidence actually says
* **"Device does not exist." is not an errno.** It is `tinypcminfo`'s own `printf` whenever `pcm_params_get()` returns NULL
  (`external/tinyalsa/tinypcminfo.c:140`). ENODEV would read "No such device" (bionic `bionic_errdefs.h:57`).
  `pcm_hw_open: cannot open device '/dev/snd/pcmC0D0p'` only says `open()` failed. tinyalsa's `oops()` then printed
  "cannot open device 0 for card 0" **with no strerror**, so the errno was lost (the `fprintf` in `pcm_hw_open` can reset it).
  **The real errno has never been seen.**
* Nodes are right: `/sys/class/sound/*/dev` = `/dev/snd` (r162), so `snd_open()` finds the minor.
* In r161/r162 the routing was **off**, because r160's `off()` had cleared `LPI_MI2S_RX_0 Audio Mixer MultiMedia1`.
  In DPCM, opening a front end with no enabled back end is **expected to fail**:
  `soc-pcm.c:dpcm_fe_dai_open()` → `"ASoC: no backend DAIs enabled for MultiMedia1"` → `-EINVAL`.
  That message is **`dev_err_once`**: it prints once per boot, the first time the dump's `tinypcminfo` ran without routing.
  After that the failure is silent. So "no kernel log" is normal for r161/r162.
* The **one unexplained failure is r160** (headset): the route was ON (`LPI_MI2S_RX_0 Audio Mixer MultiMedia1: On`) and all
  three `tinyplay` calls still failed with nothing in dmesg.
* In the 7.2 open path (`snd_pcm_open` → `snd_pcm_open_substream` → `dpcm_fe_dai_open` → `dpcm_be_dai_startup` →
  `__soc_pcm_open` → `q6asm_dai_open`), only these paths fail **without a log**:
  1. **No BE is found although the switch is on.** The FE→BE DAPM walk (MultiMedia1 Playback → MM_DL1 →
     "LPI_MI2S_RX_0 Audio Mixer" → AIF "LPI_MI2S_RX_0" → "LPI RX0 MI2S Playback") must reach the BE DAI widget. The
     `dev_err_once` has already been used, so this fails silently with EINVAL. Suspect: the `Failed to add route` lines.
     Every audio script so far filtered them out with `grep -v`, so it is unknown which routes are missing.
  2. `snd_pcm_hw_constraints_complete()` fails after the FE open. It logs through `pcm_dbg` only, and `CONFIG_SND_DEBUG` is not set.
  3. An ASoC op returns `-EOPNOTSUPP`/`-ENOTSUPP`/`-EPROBE_DEFER`. `snd_soc_ret()` deliberately does not print these three.
  4. `-EBUSY`: a substream is still held open (for example a stuck `tinyplay` from an earlier attempt).

  Each of these would log if it were the cause, so none fits the "no log" observation:
  * `q6asm_dai_open` → `-ENODEV` (missing q6asm APR service): it logs "Could not allocate memory" with `dev_info`, and the
    APR child devices must exist for the card to register at all.
  * A BE startup error: logged as `ASoC: BE open failed` or `ASoC error (%d)`.
  * The DT back ends: LPI_MI2S_RX_0/TX_3 `qcom,sd-lines = <0 1>` only matter at `hw_params`/`prepare`, not at open.

### Diagnosis bundle `v74/audio2` (MODE=diag makes no sound)
* `pcmprobe` (new, static): opens the node exactly as tinyalsa does (O_RDWR|O_NONBLOCK) and prints the **real errno**. Then it
  runs HW_REFINE and HW_PARAMS (48 k/S16/2 ch) and HW_FREE. It never calls PREPARE/START.
* It enables dynamic debug (`CONFIG_DYNAMIC_DEBUG=y`) for soc-pcm/soc-dapm/soc-core/q6asm-dai/q6routing/q6afe-dai/q6afe/q6asm/
  q6adm/apr. The `dev_dbg` copy of "no backend DAIs enabled" then prints **every** time, together with
  "found N audio playback paths", "no BE found for …", "try BE …" and "open FE …".
* It also dumps: `/proc/asound` cards/pcm/sub0 {info,status,hw_params}, the APR bus devices and their drivers, the asoc
  components and dais, `dpcm_state` of both FEs, and the DAPM widget files along the FE→BE chain (each file lists the widget's
  in/out paths and whether they are connected). It lists processes holding `/dev/snd/pcm*`, and prints dmesg since its marker,
  **unfiltered**, so the `Failed to add route` lines are included.
* It runs everything twice: once with routing off (expected EINVAL + dyndbg "no backend"), and once with the routes on,
  every codec gain at raw 0 and HPH/EAR switches ZERO.
* Result files: `/tmp/audio2-out/diag.txt`. Summary line: `A6L_DIAG_SUMMARY`.

### Most likely fix, depending on what diag shows
| diag shows (routing ON) | cause | fix |
|---|---|---|
| `open errno=22`, dyndbg "no backend DAIs enabled", DAPM file of "LPI_MI2S_RX_0 Audio Mixer" has no `out` path to "LPI_MI2S_RX_0" | a q6routing→AFE route is missing (probe order or widget name) | fix the order of the links or components in the DT (same class of bug as the 21 Sep MM_DL finding) |
| `open errno=22`, BE found, "open FE MultiMedia1" printed, no error | hw constraints (`pcm_dbg`) | q6asm-dai/FE constraints; test again with `pcmprobe` HW_REFINE output |
| `errno=95/524` | a silent ASoC op | the dyndbg lines identify the op |
| `errno=16` | substream busy | kill the holder (the "openers" list shows it) |
| `open OK` | first-open issue fixed by a clean route set-up | run `MODE=headset` |

No kernel patch or DT change for the internal path is proposed blind. The previous audio overlay is still right as far as can
be checked offline: dai-links, back ends, the IBIT clocks in `sm8250.c`, and the route/control names confirmed by the dump.

### Corrected tests (same bundle)
* `mixer2/*.txt` uses the **real names** from the 23 Sep dump (`Digital RX1 MIX1 INP1`, `Digital RX1 Digital Volume`,
  `Digital DEC1/CIC1 MUX` …). Offline, every name was checked against `v74/logs/tinymix-controls.txt` (all present; the
  `speaker*.txt` names are present too).
* **Raw volume:** dB = raw − 84. LEVEL 1/2/3 = raw 54/60/66 (−30/−24/−18 dB). The script refuses anything above 66, reads the
  value back, and if the read-back is above 66 it mutes (raw 0) and aborts. Tested against a fake tinymix with the real names:
  a write of 70 is refused, and a read-back of 124 is muted and aborted.
* **Name check** is now an exact `"<name>: "` prefix match on tinymix's output. The old `*"$name"*` check also matched
  "Invalid mixer control: <name>".
* If `tinyplay` or `tinycap` fails, the script runs `pcmprobe` on the same node, so the errno is always visible.
* Modes: `diag` (default), `dump`, `headset`, `mic`, `speaker` (V74 DT only; tone −40 dBFS), `off`. `earpiece` is skipped
  because the earpiece is broken on this unit.

## 2. Loudspeaker (TFA9894 on TERT_MI2S) — ready for the V74 recovery build
* **LPI pinctrl:** `device/hisense/a6l/kernel/a6l-lpi-ter-mi2s-v74.patch` adds the functions `ter_mi2s_clk` (gpio4),
  `ter_mi2s_ws` (gpio5) and `ter_mi2s_data` (gpio6/7) in alt-function slot 4. This is the stock "func4" (`lpi_pinctrl@15070000`
  `ter_mi2s_*_active`, 8 mA). The patch applies to the 7.2 tree. A module with the same name, built out-of-tree, is at
  `audio2/extra/pinctrl-sdm660-lpass-lpi.ko` (0 warnings, depends on pinctrl-lpass-lpi).
* **DT:** there are two variants, because fdtoverlay prepends new nodes and cannot reorder existing ones. A plain new tert
  link would become link 0, q6routing would then probe before q6asm-dai, and the card would fail with −19.
  * `a6l-audio-speaker-onbase-v74.dtso`: for a base that **already contains the V71 sound links** (the V71 `base.dtb` does).
    It prepends a new MultiMedia1 FE and turns the existing `mm1-dai-link` into MultiMedia2 and `mm2-dai-link` into the tert
    BE. Checked offline: `fdtoverlay` on the V71 base gives links **MultiMedia1 | MultiMedia2 | Speaker Playback | Internal MI2S
    Playback | Internal MI2S Capture**.
  * `a6l-audio-speaker-v74.dtso`: a superset of `a6l-audio-internal.dtso` for a base **without** sound links. Use it instead of
    that file. It compiles, but its merged order was not checked with the recovery build's fdtoverlay.
  * Both variants contain: TFA node `audio-amplifier@34` on `blsp_i2c6` (`nxp,tfa98xx`, reset `tlmm 76`, irq `tlmm 77`,
    vdd `a6l_vreg_l13a`, `pinctrl-0` = the new LPI state, `#sound-dai-cells = <0>`), and `q6afedai dai@20`
    (TERTIARY_MI2S_RX) with `qcom,sd-lines = <1>`. The line is from the stock tert config, rx-lines 0x2 = SD1 = gpio7;
    the previous doc's `dai@18` was wrong, 18 is SECONDARY_MI2S_RX.
  * **Warning for the integrator:** with either overlay the whole card waits for the TFA codec. If `snd-soc-tfa98xx.ko` or the
    patched LPI module is not loaded, the headset path disappears too. Keep `a6l-audio-internal` as the fallback DTB.
    Load order: `pinctrl-sdm660-lpass-lpi` (patched) → … → `snd-soc-tfa98xx` → `snd-soc-sm8250`, plus `/lib/firmware/tfa98xx.cnt`.
* **Test:** `D=/tmp/audio2 MODE=speaker sh /tmp/audio2/run.sh`. It checks that the tfa98xx component is in the card, sets
  `TERT_MI2S_RX Audio Mixer MultiMedia1` and `TFA Profile=music` (if present), plays 2 s at **−40 dBFS**, then turns the
  route off. Risk: the smart amp runs with the stock container unchanged; do not run calibration.

## 3. TMD3702 proximity (driver v2, bundle `v74/als2`)
* **Stock driver disassembled** (`stock-symbolized.elf`: `tmd3702_offset_calibration`, `_ps_set_enable`, `_read_ps`,
  `_als_init_paras`, DT property names resolved from the string table). It differs from our v1 in four ways:
  * `CFG6 (0xAE) = ams,ps_apc = 0x7F`, i.e. **APC disabled**. v1 left the reset value 0x3F, so APC was on.
  * `PCFG1 = gain<<6 | ps_drive = 0x09` (v1: 0x04).
  * Stock **never writes CFG4 (0xAC) or TEST3 (0xF2)**. v1 wrote the datasheet values 0x3D/0xC4.
  * Stock reads PDATA as **one byte** (0x9C) and subtracts a software crosstalk value. The crosstalk is measured at every
    enable: PEN only, wait for PINT, read PDATA, accept it if below 255. The near threshold is crosstalk + 58, far is
    crosstalk + 30. Stock does not use the hardware POFFSET/CALIB.
* **No separate emitter supply:** the stock node has only `vdd = pm660_l13` and `vio = gpio64 fixed "sunwave_ldo"` (always on).
  The VCSEL anode (LEDA pin) is not switched by the SoC. The only GPIO is the interrupt, gpio113.
* **v2 driver** (`device/hisense/a6l/kernel/tmd3702/tmd3702.c`, 0 warnings):
  * programs exactly the stock values by default; `ds_init=1` brings back the v1 CFG4/TEST3 writes;
  * measures the crosstalk at probe;
  * `in_proximity_raw` returns the full PDATA (10-bit with APC off);
  * new attributes: `prox_crosstalk` (writing to it re-measures), `prox_near`, `regs` (dump of 26 registers), and `reg_write`
    (only with `debug_write=1`).
* **Test (two runs; Pierre holds one state for each whole ~40 s run, nothing to time):**
  `D=/tmp/als2 PHASE=open sh /tmp/als2/run.sh` first, then `PHASE=cover` (finger on the e-ink-side window).
  Each run switches live through the variants stock / ds / apc / v1 / drv4 / gain4 and prints `A6L_PROX … P=[5 samples]`, plus
  STATUS/PDATA/CFG registers. Pass = P(cover) ≫ P(open) in `stock` (expected). The `v1` variant should reproduce the flat ~90.
  If every variant is flat, the VCSEL is not emitting: an optical or hardware issue, or LEDA is fed by an unpowered rail.

## Artifacts (repo `firmware/extracted/audio2-20260923/`, laptop `~/A6L-usb-20260915/v74/{audio2,als2}`, sha256 verified on laptop)
| sha256 (first 16) | file |
|---|---|
| `3e73abb0e81625b9` | `modules/tmd3702.ko` (= `v74/als2/modules/tmd3702.ko`) |
| `392f9fc9919f7157` | `modules/pinctrl-sdm660-lpass-lpi.ko` (= `v74/audio2/extra/`) |
| `b2067a3b8712a97b` | `modules/pcmprobe` (= `v74/audio2/bin/pcmprobe`) |
| `9d31e7316d92c432` | `v74/audio2/run.sh` |
| `4f5d7e8d03949c31` | `v74/audio2/SHA256SUMS` |
| `61c274da12a34a1a` | `v74/als2/run.sh` |
| `da090fc05eda89dc` | `v74/als2/SHA256SUMS` |
| `90732dcce95309cb` | `dt/a6l-audio-speaker-onbase-v74.dtbo` |
| `02a12e34ddf6ac71` | `dt/a6l-audio-speaker-onbase-v74-merged.dtb` (V71 base + overlay, order checked) |
| `160bb1691bdf881e` | `dt/a6l-audio-speaker-v74.dtbo` |
| `b91070219b2da4c8` / `0ecfff4cd31f2ff4` | `v74/audio2/extra/snd-soc-tfa98xx.ko` / `tfa98xx.cnt` (unchanged from the afternoon) |

The full list is in `SHA256SUMS-all`.

Sources in the repo:
* `device/hisense/a6l/audio/v74-test/{audio2-run.body.sh, als2-run.body.sh, pcmprobe.c, mixer2/}`
* `device/hisense/a6l/kernel/{tmd3702/tmd3702.c, a6l-lpi-ter-mi2s-v74.patch, lpi-pinctrl-v74/, a6l-audio-speaker-v74.dtso, a6l-audio-speaker-onbase-v74.dtso}`
* `tools/build-audio2-als2-v74.sh`

## Attended procedure (V71 recovery, ADSP + card up as in the evening session; audio and ALS can share one boot)
```
# laptop, ~/A6L-usb-20260915:
adb -s HLTE730T-PROBE push v74/audio2 /tmp/audio2 ; adb -s HLTE730T-PROBE push v74/als2 /tmp/als2
# phone (ph.sh): always start with export PATH=/tmp/bin:$PATH
export PATH=/tmp/bin:$PATH; D=/tmp/audio2 MODE=diag sh /tmp/audio2/run.sh      # no sound; pull /tmp/audio2-out/diag.txt
export PATH=/tmp/bin:$PATH; D=/tmp/audio2 MODE=headset sh /tmp/audio2/run.sh   # only if diag shows "open OK" with routing ON
export PATH=/tmp/bin:$PATH; D=/tmp/audio2 MODE=mic sh /tmp/audio2/run.sh
export PATH=/tmp/bin:$PATH; D=/tmp/als2 PHASE=open sh /tmp/als2/run.sh         # sensor uncovered
export PATH=/tmp/bin:$PATH; D=/tmp/als2 PHASE=cover sh /tmp/als2/run.sh        # finger on the e-ink-side window
```
The bundle header checks `hisense,a6l-controls = v71` and SHA256SUMS. It needs the c176000 I²C bus (e-ink PMIC bundle) for als2.

## Untested / unknown
* Everything above is untested on hardware.
* The actual errno and cause of the r160 failure are unknown; diag is built to answer this.
* The TFA9894 has never been probed on mainline.
* TERT bit clock and SD line come from stock DT values only.
* The superset overlay's merged order was not checked with the recovery build's fdtoverlay.
* PLDRIVE code 9 is outside the datasheet table (it is what stock uses).
