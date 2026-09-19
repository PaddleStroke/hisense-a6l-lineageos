# V45 attended results — 18 September 2026

The combined test finished without a kernel crash or USB loss. Stock Android
returned with the original fingerprint and boot-completed=1. The coordinator
finished at 07:26:14.790196 UTC and restored the host services (owned pause null).
V45 recovery remains installed, SHA-256
`aae86f0da6f15a0c1f6e425e88806edb165289c459462b232f0b8773f5e4ac14`.
Its installation passed all twelve independently copied readbacks. Do not
reuse the one-shot V45 install/capture launchers; their output directories exist.

| Component | Result |
|---|---|
| Android graphics | Real SurfaceFlinger, AIDL graphics services, ANGLE/SwiftShader and client composition passed. Four quadrants visually confirmed. No composer commit failures, unchanged global mounts and private-service cleanup passed. |
| Front touchscreen | Driver bound at I2C 0-0038, correct 1080×2340 geometry; attended captures recorded 588 and 1175 frames without coordinate errors or dropped events. |
| Two-finger evidence | 1036 frames update two separate slots. Full initial tracking state was not captured; parser's maximum-one-contact result was misleading and does not establish a driver defect. |
| Power and volume | Press/release events for codes 116, 115 and 114 confirmed. |
| E-ink side key | User pressed it, but no event appeared; its configured IRQ counter remained zero. Mapping/pinctrl needs checking against stock. |
| Brightness | Two low levels visually confirmed; original setting restored. |
| Vibration | Module bound and bounded pulse/cleanup commands succeeded, but user did not feel it. Physical output unconfirmed. Do not classify as working or increase amplitude without reviewing motor configuration. |
| Battery telemetry | 99%, 4,364,002 µV, -8,789 µA, 318 deci°C and 3,800,000 µAh design capacity reported. Charging and measurement accuracy are unvalidated. |

The first touch attempt was missed by the user and contained no events. It is
preserved as a failed observation, not treated as hardware failure.

## RAM-only repairs during the session

The kernel registered input devices, but the minimal Android RAM userspace
lacked `/dev/input` event nodes. `Create-V45InputNodes.py` verified diagnostic
identity, virtual mounts, registered major/minor numbers and device names before
creating the corresponding character nodes in `/dev` tmpfs. A haptic node was
added after that driver loaded. No input driver changes were needed to obtain
front touch and three working buttons.

The brightness helper initially stopped at its first `printf`: this command
was missing from the minimal root. No brightness change had occurred. The
verified toybox printf applet was exposed through a RAM symlink, then the
unchanged bounded test passed in a separate follow-up report. These fixes vanish
on reboot; incorporate them into the next RAM package or guarded staging tool.
Original pinned tools and failed reports remain preserved.

## Follow-up priorities

1. Integrate the RAM device-node/utility fixes. Make input recording signal
   readiness before asking for contacts; have the user lift and reapply fingers
   after it starts, or capture the initial EVIOCGMTSLOTS state. Do not claim
   missing multitouch from a stream that began mid-contact.
2. Compare the stock e-ink key GPIO/pinctrl/interrupt mapping with the candidate;
   distinguish the physical side key from an e-ink power/status signal.
3. Compare haptic waveform, mode, resonance and enable sequence with stock.
   The fixed low test pulse may be too weak, but that is only a hypothesis.
4. Connect working touch to Android input dispatch/UI; current graphics tests
   establish rendering services, not a fully booted Lineage launcher. Continue
   native GPU/display, power policy and shared radio/audio integration separately.

Evidence: `captures/capture-controls-user-v45`, including initial failures,
successful follow-ups, raw events, final kernel state and final coordinator
report. `analysis.json` distinguishes software checks from user observations;
`evidence-sha256.json` indexes the saved files. Summary helper:
`tools/Analyze-ControlsV45Capture.py`.
