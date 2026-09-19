# Combined diagnostic V45 — prepared, not installed

The next attended session can test graphics, front touch, buttons, brightness,
vibration and battery telemetry in one diagnostic boot. This remains a RAM
diagnostic, not the complete LineageOS desktop or production hardware support.
No installation, reboot, module activation or physical output was performed
during this preparation. The spare remains in stock Android with V38 recovery.

## Image and evidence

Candidate: `firmware/extracted/recovery-controls-v45-20260917/recovery-diagnostic-unsigned.img`

SHA-256: `aae86f0da6f15a0c1f6e425e88806edb165289c459462b232f0b8773f5e4ac14`

The V38 Linux 7.2.3 kernel, Android RAM userspace, command line and recovery
selection overlay are preserved. There are 57 scoped Device Tree property
changes combining the previously reviewed touch, button, WLED, haptic and fuel
gauge descriptions. The `/chosen/hisense,a6l-controls` marker is `v45`;
`ro.a6l.ramdiag` intentionally remains `v38` for the unchanged RAM userspace.
The compressed boot kernel section includes the changed DT, so its section
hash changes even though the executable kernel is preserved.

Packaging/roundtrip and captured bootloader validation passed, including board
selection and merged DT fixtures. The packaging report's `ready_to_flash:false`
is its initial packaging-only state; separate captured-ABL and staging reports
record the completed later checks. It is not evidence of a phone test.

Offline checks completed:

- Three module ABI/load/unload tests: 9/9 checks in diskless QEMU.
- Existing real SurfaceFlinger/ANGLE/SwiftShader graphics payload: 12/12 QEMU
  checks, including every scanout pixel and the adjacent memory guard.
- Wrong-device/hash/mount/privilege rejection: 7 cases.
- Touch parser: 10 cases including dropped input and retained slot coordinates.
- Recovery transition: 4 cases; writer protocol: 6 cases.
- Laptop staging: 30 pins, 9 V38 tools, 12 V45 tools, 5 controls payload files
  and 146 graphics payload files verified. Offline writer/restore preflight
  passed without opening USB.

Reports: `research/combined-controls-v45-20260917/laptop-stage-verification.json`
and `laptop-offline-inspection.txt`. Module payload/report:
`firmware/extracted/controls-v45-prep-20260917`.

## Tomorrow's attended sequence

1. Read fresh stock identity, boot-completed and host-helper status. Keep the
   everyday A6L disconnected. Verify staged hashes again.
2. Invoke `Launch-ControlsV45Install.py` once on the laptop. It performs the
   guarded recovery-only write, full readback and poweroff. Copy the complete
   installation capture to the desktop and run `Verify-ControlsV45Readbacks.py
   install` before asking for Power/normal Android. Do not rerun on ambiguity.
3. After stock Android returns, invoke `Launch-ControlsV45.py` once. Confirm
   fastboot/logging, then ask the user to select Recovery. No filming. The
   readiness helper uses the existing short authenticated ADB gate.
4. In the same diagnostic boot, invoke the following separate stages with
   `/usr/bin/python3 /home/pierrelouis/A6L-usb-20260915/Run-ControlsV45.py STAGE`:

| Order | Stage | What it establishes / user action |
|---|---|---|
| 1 | `baseline` | Input, interrupts, modules, backlight and kernel logs |
| 2 | `surface` | Real Android graphics services and visible four-quadrant pattern; user confirms appearance |
| 3 | `touch-load` | Front controller driver binds and reports expected geometry |
| 4 | `touch-events` | 15 seconds: corners, centre, swipe, two fingers |
| 5 | `keys` | 15 seconds: briefly press Power, Volume Up/Down and e-ink key |
| 6 | `brightness` | Two low levels over four seconds, then restores the previous value |
| 7 | `vibration` | One low-strength 100 ms pulse; user confirms it was felt |
| 8 | `battery` | Load fuel-gauge driver and capture voltage/current/temperature/capacity |

Each stage checks V45 identity, authenticated root ADB, virtual-only mounts and
payload hashes. Each has a separate one-shot report. Preserve failed reports;
inspect the actual failure and device state before deciding whether another
independent stage is appropriate. No automatic retry or reboot follows failure.
The capture coordinator keeps the host pause/inhibitor for up to 15 minutes
after readiness while awaiting stock Android; keep the attended session within
that window or explicitly finish and clean up.

5. Copy stage logs and collect the user's physical observations. Return to
   stock Android with Power and verify host-service cleanup. The V45 stock
   restore tool is prepared if restoration is needed. Update the current
   recovery hash in the session notes only after verified physical changes.

## Limits and remaining preparation

Combining the checked descriptions reduces boot cycles; it does not guarantee
all drivers will probe successfully together. WLED is built in and probes at
boot. Touch, haptics and fuel gauge remain unloaded until their selected stages.
Fuel-gauge initialization changes PMIC control state; this is not a purely
passive read. Charging remains disabled in this candidate. No charging,
suspend/resume, battery-life or thermal-policy validation is implied.

The controls helper restores brightness on ordinary exit/signals, but cannot
guarantee restoration after a kernel crash or forced poweroff. Vibration has a
kernel-timed 100 ms limit plus explicit stop/removal. It is below the stock
voltage request and uses the reviewed fixed strength, not arbitrary FF values.

Graphics uses the preserved boot display with software rendering. Touch event
success does not prove Android input dispatch or a usable launcher. A full
Android framework boot, HAL integration, native panel/GPU, suspend and SELinux
policy remain later work.

Wi-Fi/cellular/GNSS/audio/sensors already have archived firmware, module and
hardware inventories, but are not part of this activation bundle. Remaining
shared prerequisites include ADSP/APR services, modem/QRTR and RMTFS integration;
SDM660 IPA 2.6L data support is unresolved. Bluetooth's supply mapping is not
yet sufficiently established. Speaker protection and charging policy also
require review before activation. Do not add guessed DT entries just to expand
this batch.

The separate e-ink software ABI experiment passed: see
`eink-abi-preparation-20260917.md`. Physical panel transport, power sequencing
and separate SPI waveform/calibration preservation remain unresolved.
