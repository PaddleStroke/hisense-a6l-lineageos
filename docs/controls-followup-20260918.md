# Control follow-up after V45

## Vibration: confirmed data-format discrepancy

The exact V45 DT property `qcom,brake-pattern` is 16 bytes encoding four
big-endian 32-bit cells `[3, 3, 0, 0]`. The pinned driver calls
`of_property_read_u8_array(..., 4)`, which reads `[0, 0, 0, 3]` from those bytes.
This is demonstrably not the intended pattern. It is **not yet established as
the reason the user did not feel the 100 ms test pulse**. The low requested
voltage, PM660-specific resonance and startup sequence remain hypotheses.

`tools/Prepare-HapticBrakeFix.py` builds an isolated external module without
editing the validated kernel source/output. The candidate reads four u32 cells
when present and checks each is 0–3; the four-byte legacy form remains accepted.
No drive strength, voltage, duration, current limit or resonance algorithm changes.

Artifacts: `firmware/extracted/haptics-brake-prep-20260918/` contains original and
candidate sources, patch, actual V45 property audit, build log, manifest and QEMU
log/report. Module SHA256:
`a56fd661a534d46148bf0e097fe6967de7f2e0e2bf0bd9eb89de16485d80afb2`.
All five diskless-QEMU ABI/load/unload checks passed. This does not exercise a
physical haptic register or prove the repair restores perceptible vibration.
Nothing has been staged or installed on the phone from this candidate.

Source provenance: the pinned local `drivers/input/misc/qcom-spmi-haptics.c`
SHA256 is `e83ffefb3610c9c5370f4227cfa68744a3c0df41c7e870145a92e7e88731dcd6`.
Its local binding example also uses u32 cells. Do not assume differently named
newer Qualcomm haptic drivers target this PM660 block. Online primary comparison
was the [Qualcomm PM660 stock-style description](https://android.googlesource.com/kernel/msm/+/android-msm-wahoo-4.4-oreo-m2/arch/arm/boot/dts/qcom/msm-pm660.dtsi),
which supplies a byte-array brake pattern and QWD resonance configuration.
The attempted current Torvalds file fetch did not resolve; it is not used as
evidence for what current upstream supports.

## E-ink key: stock events confirmed

Both preserved stock merged DTs agree on PM660 GPIO11, active low, code 616.
The stock pin description additionally programs GPIO11 as normal input,
pull selection 0 and power-source selection 0. The V45 gpio-keys node requests
the same GPIO but relies on inherited pin setup rather than an explicit pinctrl
state. Thus missing pin configuration is a candidate explanation, not proven.

Fresh stock capabilities confirm `/dev/input/event9` named `gpio-keys` supports
Volume Up, F18 and LEFT_UP. The first capture window was missed by the user.
The coordinated second recording captured **13 complete KEY_LEFT_UP press/release
pairs**, Linux code 616, with no F18 events in this recording. This confirms the
physical side button works and the candidate's intended event code is correct.
It does not yet identify the cause of V45's zero IRQ count. Next compare the
explicit stock PMIC input/pull/source setup with the inherited configuration
used in V45. The port's pin configuration remains a hypothesis, not a proven fix.

Evidence: `research/controls-followup-20260918/stock-eink-key-events-r2.txt` and
the hash-indexed `stock-eink-key-result.json`. The second capture used an ADB
pseudo-terminal so output flushes promptly. No recording remains running, and
no phone configuration or firmware was changed.

## Interactive Android next

V46 preparation now applies explicit stock-equivalent PM660 GPIO11 pinctrl.
The independent GPIO and haptic reviews are archived alongside the stock key
capture. See `controls-v46-20260918.md` for the combined short test and outcomes
to distinguish. V46 packaging and captured-ABL checks passed; this does not
establish physical success. The haptic candidate is being hardened to reject
malformed present properties before the attended pulse.

The immediate larger milestone is to connect the demonstrated touch stream to
Android input dispatch and a visible UI, building on the validated SurfaceFlinger
pipeline. The current graphics client uses native colour layers and a private
service namespace, not WindowManager/SystemUI/launcher. Those framework services,
HAL manifests, writable directories and input routing require a new bounded RAM
integration stage. LCD native initialization, GPU acceleration and suspend/wake
remain independent tasks in `port-status.md`.

Stock routing: fresh dumpsys input confirms enabled event9 uses Generic.kl,
despite a separate gpio-keys.kl file existing on disk. The empty first capture is
preserved as a missed window, not hardware-failure evidence.
