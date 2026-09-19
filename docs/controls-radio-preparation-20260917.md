# Controls, Bluetooth, GNSS and audio preparation — 17 September 2026

All work this session was offline or read-only stock inspection. The spare remains
in stock Android with V38 recovery unchanged. Nothing was flashed, rebooted,
played, recorded, paired or vibrated. No location was requested. V44 remains staged
and physically untested, awaiting the user's return.

## Deliverables and evidence

`firmware/extracted/controls-radio-prep-20260917/` contains:

- Eleven haptic/Bluetooth modules with a complete dependency order and SHA256
  manifest. All 25 QEMU load/unload/cleanup checks passed with the exact validated
  V38 Linux 7.2.3 kernel. This is ABI validation, not physical driver validation.
- Three independent DT overlays and merged review candidates for buttons,
  haptics and LCD backlight. The property-diff audit passed: 15, 2 and 9 changes
  respectively, confined to the intended nodes. Charger and other hardware nodes
  are unchanged. These DTBs are **not flashable recovery images**: image packaging
  must apply the changes before the bootloader's DT modifications.
- A built static AArch64 `a6l_haptic_probe` and `backlight_probe.sh`. All nine
  emulator checks passed for bounds/refusal behavior. Physical actuation and the
  brightness restoration path still need an attended test.
- Ten original Bluetooth-partition files, extracted read-only after matching the
  entire partition to its backup SHA256. Six CR firmware/NVM files have passed
  TLV framing and NVM-tag bounds checks and have case-correct copies under
  `firmware/qca/`. This does not prove signature or controller compatibility.
- Eight original audio mixer XML variants and expanded microphone, earpiece and
  headset routes; five GNSS configuration files; hashes of all 23 saved assets.
- `dependency-audit.json`, including stock GNSS ELF dependencies and explicit
  Bluetooth/audio prerequisites. No unsupported power rail was guessed.

The rejected first buttons DT and first probe-test logs are retained with explicit
`rejected`/`r1` names. The DT audit caught an automatic `fragment@0` naming collision
and it was corrected before any device use. The first shell test exposed a missing
`grep` symlink in the RAM environment; the final script calls toybox explicitly.
Only the final passing reports designate candidates for further preparation.

## Buttons

`device/hisense/a6l/kernel/a6l-buttons.dtso` enables:

| Button | Evidence / wiring | Linux event |
|---|---|---|
| Power | PM660 PON KPDPWR | 116 |
| Volume down | PM660 PON RESIN | 114 |
| Volume up | PM660L GPIO7, active low | 115 |
| Rear-screen button | PM660 GPIO11, active low | 616, retained from stock |

PMIC GPIO numbering is one-based in both bindings. Stock debounce values are
preserved. Live stock input capabilities confirm power/volume devices and the
vendor side-key input. The candidate initially relies on existing PMIC pin
configuration; full suspend/pinctrl validation remains. The power-key driver
configures debounce/pull-up and must be tested with the established long-Power
escape route. Key 616's Android action needs a keylayout/framework decision;
receiving the event is separate from switching displays.

## LCD brightness

`a6l-backlight.dtso` uses the upstream PM660L WLED4 driver, already built into V38.
The A6L stock DT specifies strings 0 and 1, 20,000 uA per string, 970 mA boost
limit, 800 kHz switching and 29,600 mV overvoltage threshold. These values are
translated into the upstream property names. Automatic string detection is not
enabled. Native brightness range is retained at 0–4095; initial software value is
256. This property is not proof of physical brightness at driver takeover.

The four-second test verifies kernel/RAM-diagnostic identity, exact PM660L
compatible and brightness range, then requests 64 and 256 for two seconds each.
It saves/restores the previous value and handles ordinary termination signals.
A kernel crash, forced poweroff or SIGKILL can bypass shell cleanup. Probe itself
configures PMIC registers, so this is not a passive test. Do not launch it while
the user is away. Screen suspend/resume and Android's Lights HAL remain separate.

Stock shell does not expose usable LED/backlight metadata at the queried class
paths; the live query is saved as unavailable, not taken as evidence of absence.

## Vibration

`a6l-haptics.dtso` enables PM660 SID1 c000 with stock LRA, square wave, 6667 us
period, direct play and brake pattern 3/3/0/0. The existing upstream driver uses
400 mA versus the stock 800 mA limit and a different resonance algorithm. Those
differences require a short physical test.

The pinned driver converts `strong_magnitude >> 8` as a percentage. Consequently
a conventional half-strength FF request would saturate at 3596 mV, above the
stock requested 3200 mV. The prepared utility accepts no arbitrary strength or
duration: 8192 strength means a 1229 mV request, rounded to 1276 mV, for 100 ms.
It checks the exact kernel and `spmi_haptics` input name, uses the kernel effect
timer, explicitly stops/removes the effect and closes the device. A quiet or
imperceptible pulse is not proof of a missing actuator. Production HAL support
needs corrected scaling/voltage limits and resonance validation first.

## Bluetooth

Stock `/sys/class/tty/ttyHS0/device` resolves to `c1af000.uart`, matching mainline
BLSP2 UART1 on GPIO16–19. The WCN3990 transport module is built and ABI-tested.
Stock supplies establish PM660 L9 as core/XO, L6 as RF/PA and L19 as CH0/LDO.

**VDD_IO is unresolved.** Stock's `bt-chip-pwd` points to PM660L BOB pin1 at 3.6 V;
that cannot simply be relabelled as the upstream 1.8 V IO supply. Related SDM660
board files use differing rails, and one explicitly marks its choice TODO. No
Bluetooth activation overlay was generated with a speculative supply. Next is
tracing the A6L power implementation/board evidence, followed by a controller
version query and firmware loading. Preserve regulator-load permission checks
learned during eMMC bring-up.

Original `crbtfw11/20/21.tlv` and `crnv11/20/21.bin` are ready for the kernel's ROM
selection. Do not arbitrarily force one revision. The NVM files contain type-4
containers; the pinned loader supports those. UART flow control, sleep/wake,
pairing, Android HAL and Bluetooth audio each require later validation.

## Microphone, earpiece and wired headset

The common path is ADSP → APR/QDSP6 → internal digital codec at 152c0000 → PM660L
analog codec at SID3 f000. The earlier peripheral bundle already provides the
relevant modules with passing ABI checks. Stock MCLK is 9.6 MHz. Stock CP/PA
supply is PM660 S4; microphone-bias supply is PM660L L7. The upstream PM660L
analog compatible needs its PM8953 fallback, not the generic PM8916 defaults.

The saved route expansions identify RX1/earpiece, RX1+RX2/headphones, ADC1/main
microphone and ADC2/headset microphone in the common XML. Active XML selection
is not conclusively identified: stock shell denies audio-card metadata. Keep all
variants. These stock route/gain values are evidence, not an upstream `tinymix`
script. Build the ADSP/clock/DAI links, enumerate actual mixer controls, then use
muted/low-gain output and a brief requested microphone recording. Headset detect,
button thresholds, mic polarity/bias and unplug behavior need physical checks.

These internal outputs can be developed independently of the TFA9894 speaker
amplifier. Do not use the protection-bypassing TFA9894 RFC as a shortcut.

## GPS / GNSS

The stock `vendor.qti.gnss@2.0-service` uses old HIDL GNSS/vendor libraries;
`libloc_api_v02.so` directly depends on `libqmi_cci.so` and `libqmi_common_so.so`.
This is a modem location-service integration, not an external UART GPS chip.
Prepared configuration/dependency evidence supports a later QRTR/QMI LOC probe.
First bring up MSS/modem and its RMTFS/service dependencies; then discover LOC,
test short satellite/fix reception with the user and integrate a compatible
Android HAL. Standalone location testing need not wait for the IPA LTE IP data
path. No location data or assistance-data changes were requested this session.

## Next attended session

1. Finish already-staged V44 display composition test.
2. Package reviewed front touch/buttons/brightness changes into the next
   diagnostic, with normal image/bootloader checks. Haptics can remain a module
   loaded only when its pulse test is requested. Each overlay stays independently
   selectable if a regression needs isolation.
3. One diagnostic boot can cover touch gestures, side keys, two brightness levels
   and the short vibration test, with automated USB evidence and brief user
   confirmation. No long filming/waits are necessary once authenticated ADB is up.
4. Continue battery telemetry and shared ADSP/modem bring-up, then audio/GNSS;
   resolve Bluetooth IO power before controller activation. Keep the early eInk
   checkpoint from the main hardware roadmap.

## Sources

Authoritative local source is the pinned kernel tree and the phone's verified
stock DT/firmware. Relevant upstream references checked during this session:

- [Qualcomm Bluetooth transport implementation](https://kernel.googlesource.com/pub/scm/linux/kernel/git/bluetooth/bluetooth-next/+/ea9866793d1e925b4d320eaea409263b2a568f38/drivers/bluetooth/hci_qca.c)
- [Qualcomm WLED binding](https://android.googlesource.com/kernel/common/+/15600159bcc6abbeae6b33a849bef90dca28b78f/Documentation/devicetree/bindings/leds/backlight/qcom-wled.yaml)
- [libqmi location-service implementation](https://github.com/linux-mobile-broadband/libqmi/blob/main/src/qmicli/qmicli-loc.c)

These references establish available interfaces; none proves A6L hardware support.
