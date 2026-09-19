# EVIDENCE — PM660 haptics drive-path investigation (offline)

Date: 2026-09-18. Workspace read-only except this directory. No phone/laptop
access; no ADB/fastboot/EDL/flashing. All claims below are from saved artifacts
and primary online sources.

## 1. Artifacts examined (paths + SHA256)

| Artifact | SHA256 |
|---|---|
| `device/hisense/a6l/kernel/a6l-haptics.dtso` (our overlay) | `ed8cccddf2b75cf67b3cbf0d17b4bb1e6bb2f94d646d724f12c52e4b23a4a991` |
| `device/hisense/a6l/diagnostic/haptic_probe.c` (test helper) | `3d84416820b50fc30b739040b3c5788c8e9a962b39958b209eb38f43f2b43e7c` |
| `firmware/extracted/device-trees/stock-00.dts` (stock merged DT) | `769d56892badca9efca36a8db92369530947de63731883d8c8192e89e76324c6` |
| `firmware/extracted/haptics-brake-prep-20260918-r4/candidate.c` (R4 driver) | `9ac4a943be46172c1fe6205fb4af268d7af3544dba47e9b986e6384fc9a9ff52` |
| `firmware/extracted/haptics-brake-prep-20260918-r4/original.c` (pinned mainline) | `e83ffefb3610c9c5370f4227cfa68744a3c0df41c7e870145a92e7e88731dcd6` |
| R4 module `qcom-spmi-haptics.ko` (as loaded in V46) | `9e0c92f8af21e5ed269945de04887e46ebaadf554a5cac59e7f7902630acf3f8` |

V46 physical evidence: `captures/capture-controls-user-v46/` (`analysis.json`,
`stages/vibration/`). `analysis.json` records `commands_passed: true`,
`user_felt_vibration: false`, `physical_passed: false`.

## 2. The pulse path, end to end (from source)

1. **Helper** `haptic_probe.c`: sends one `FF_RUMBLE`, `strong_magnitude=8192`,
   `replay.length=100 ms`. Its own header notes `8192>>8 = 32 → 1229 mV
   requested → rounded 1276 mV` (stock cap 3200). Confirmed by recomputation:
   magnitude 32, vmax 1229, rounded 1276 mV.
2. **ff-memless → driver** `spmi_haptics_play_effect()` (candidate.c:743-775):
   `magnitude = strong_magnitude >> 8` (=32); `vmax = (3596-116)*32/100+116 =
   1229 mV`; schedules work.
3. **work → enable** `spmi_haptics_enable()` (candidate.c:610-…): set_auto_res
   false → module_enable(0x46) → play_control PLAY (0x70) → set_auto_res true.
4. **init** `spmi_haptics_init()` programs CFG1(0x4C), LRA_AUTO_RES(0x4F),
   SEL/play-mode(0x4E), VMAX(0x51), ILIM(0x52), SC_DEB(0x53), CFG2(0x4D),
   RATE(0x54/55), BRAKE(0x5C). **It never touches 0x56/0x58 (internal PWM).**
5. V46 dmesg (`stages/vibration/dmesg-after.txt`): module bound at
   `800f000.spmi:pmic@1:vibrator@c000`, input3 `spmi_haptics` created, probe
   returned 0. No haptic errors, **no short-circuit**, no auto-res error logged.
   Probe printed `A6L_HAPTIC_COMMANDS_PASS physical_confirmation_required=1`.

## 3. Stock vs. our driver/config — confirmed discrepancies

Stock node `qcom,haptic@c000` (stock-00.dts:2788-2810), driver
`qcom,qpnp-haptic`:
```
compatible = "qcom,qpnp-haptic";      pmic-revid = 0x7a (PM660)
qcom,vmax-mv = 0xc80  = 3200 mV        qcom,ilim-ma = 0x320 = 800 mA
qcom,wave-shape = "square"             qcom,play-mode = "direct"
qcom,wave-play-rate-us = 0x1a0b = 6667 qcom,int-pwm-freq-khz = 0x1f9 = 505 kHz
qcom,sc-deb-cycles = 8                 qcom,en-brake; brake-pattern = 0x3030000
qcom,lra-high-z = "opt0"               qcom,lra-auto-res-mode = "qwd"
qcom,lra-calibrate-at-eop = 0          qcom,correct-lra-drive-freq;
qcom,misc-clk-trim-error-reg = 0xf3
```
Our driver is the **pmi8998 mainline** `qcom-spmi-haptics` (Caleb Connolly),
run in **direct** mode via overlay `a6l-haptics.dtso`.

| # | Discrepancy | Status | Source |
|---|---|---|---|
| D1 | **Internal-PWM carrier (0x56 INT_PWM / 0x58 PWM_CAP) never programmed.** Stock sets 505 kHz; mainline has no such register. In direct/buffer LRA mode this carrier synthesises the drive. | **Confirmed (source)** | mainline defines lack 0x56/0x58 (grep); downstream `qpnp_hap_int_pwm_config()` writes 0x56 & 0x58 |
| D2 | **Current limit 400 mA vs stock 800 mA.** Mainline hardcodes `current_limit = HAP_ILIM_400_MA`; no DT read. | **Confirmed (source)** | candidate.c:970 `HAP_ILIM_400_MA`; stock `ilim-ma=800` |
| D3 | **Auto-res mode ZXD_EOP hardcoded vs stock "qwd"; no `correct-lra-drive-freq`/clk-trim correction.** Binding says PM660 supports only `zxd`/`qwd`; ZXD_EOP is a pmi8998-era mode. | **Confirmed (source)** | candidate.c:546 `HAP_AUTO_RES_ZXD_EOP`; binding "for pm660: zxd, qwd" |
| D4 | **VMAX overdrive left disabled.** Mainline clears `HAP_WF_OVD_BIT`; comment `// TODO: pm660 can enable overdrive here`. | **Confirmed (source)** | candidate.c:312 |
| D5 | **Driver validated in buffer mode on pmi8998; A6L forces direct mode on PM660.** | **Confirmed (source)** | LWN/patch series "initial support for LRA haptics in buffer mode", pmi8998 dts |
| D6 | **Requested drive only ~1276 mV vs stock 3200 mV** (test-helper magnitude 32, not a driver limit). | **Confirmed (arithmetic)** | haptic_probe.c self-test; recomputed |
| B0 | Brake-pattern u32-vs-byte bug (V45). | **Fixed in R4, verified insufficient** | controls-v46 physical result |

D1 is the most likely single reason a command can "succeed" yet move the LRA
imperceptibly: EN/PLAY/VMAX all latch, but the synthesised drive is built on an
unprogrammed PM660 carrier. D2/D3/D6 compound the weakness. None of D1–D5 raise
an error, consistent with the clean V46 dmesg.

## 4. Primary sources
- Mainline driver targets **pmi8998**, "initial support for LRA haptics in
  **buffer mode**": https://lwn.net/Articles/866503/ and the v3 series
  https://patchwork.ozlabs.org/project/devicetree-bindings/patch/20210816221931.1998187-2-caleb@connolly.tech/
- Downstream `qpnp-haptic` (SDM660/PM660 class device, whyred): programs
  `INT_PWM (0x56)` + `PWM_CAP (0x58)`, ILIM `(ilim/400-1)`, VMAX `(mv/116)<<1`,
  PLAY 0x70, EN 0x46; PM660 subtype allows VMAX overdrive & hw auto-res:
  https://github.com/PrimoDev23/kernel_xiaomi_whyred/blob/master/drivers/soc/qcom/qpnp-haptic.c
- Binding: PM660 auto-res modes limited to `zxd`,`qwd`; `int-pwm-freq-khz`,
  `correct-lra-drive-freq`, `misc-clk-trim-error-reg` defined:
  https://github.com/arter97/android_kernel_realme_sdm710/blob/master/Documentation/devicetree/bindings/leds/leds-qpnp-haptics.txt
- Stock-style PM660 config referenced in V46 report:
  https://android.googlesource.com/kernel/msm/+/android-msm-wahoo-4.4-oreo-m2/arch/arm/boot/dts/qcom/msm-pm660.dtsi

## 5. Isolated test performed here
`isolated-test/int_pwm_ilim_encoding_test.c` — replicates only the pure
freq→register and ilim→register encoding the candidate patch uses; verifies the
stock 505 kHz→sel 1 and 800 mA→sel 1 mappings and rejects unsupported values.
Result: `A6L_INT_PWM_ILIM_TEST_PASS cases=9`. This is a bounded userspace check,
**not** a hardware or register test.

## 6. Explicitly NOT done (boundaries)
- No full Android/kernel build; the candidate `.ko` was **not** rebuilt (would
  require the peripheral build host `/home/a6l/kernel/...`). Patch is offered for
  the main agent's existing R4/R5 build+QEMU flow.
- No phone or laptop access; nothing staged or flashed. The V46 module and all
  source trees were treated read-only.
