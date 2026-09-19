# GPIO 8 reservation fix — 2026-09-15

V6 screen evidence is preserved in `captures/pinctrl-gpio8-screen-20260915/`.
The actual pinctrl callback completes mapping, reset-handler setup, IRQ setup,
pinctrl registration and enabling. GPIO 0–7 control reads finish; the last
visible line is `[0.167046] ... A6L pinctrl gpio 8 ctl begin`, with no completion
marker for that read. Android USB reappears about 100 seconds after leaving
fastboot; no diagnostic USB appears. The user was asked to leave buttons alone;
the explicit no-button confirmation from the previous v5 trial should not be
misrepresented as a separate confirmation for v6.

V6 stock recovery restoration/readback passed at 16:46:33 UTC, and original
Android/services at 16:46:57 UTC. Twelve copied before/after regions passed
independent desktop hashes. All v6 physical coordinators are USED.

## Evidence supporting the change

The pinned SDM660 driver maps GPIO 8 to the north tile with control offset
8 * 0x1000. The captured prototype DT maps north at 0x03900000, yielding
0x03908000. This is within the stock A6L TLMM span 0x03000000–0x03bfffff.
Stock pinmux descriptions group GPIOs 8–11 as SPI3 (10–11 also have I2C3
states). Their presence alone does not establish access permissions.

The same pinned kernel's SDM660 Xiaomi Jasmine, Lavender, Platina, Vsmart
Zangyapro and BlackBerry board descriptions reserve GPIOs 8–11 with
`gpio-reserved-ranges = <8 4>`. Several SDM636 and SDM630 boards do likewise.
Exact A6L secure-firmware ownership is not established; the physical GPIO 8
read boundary plus this existing SoC-family pattern supports a bounded test.

The pinned GPIO core initializes its valid mask by applying reserved ranges
before iterating over GPIO descriptors. It calls get_direction only for valid
lines. Reserving this group therefore excludes the problematic initialization
reads; it does not attempt to unlock pins or modify secure firmware.

## V7 candidate

`device/hisense/a6l/kernel/sdm660-hisense-a6l-probe.dts` adds only the TLMM
reservation. The kernel, RAM diagnostic and command line remain byte-identical
to v6. Only the device tree was rebuilt. Comparing compiled v6/v7 trees proves
there is exactly one property change and no node or other property changes.
Packaging roundtrip, captured ABL header/decompression/DT/AVB and six write
fault tests passed. The captured merged tree retains the reservation.

Recovery SHA256:
`117d78d628feb0e9f0f41e6f81c77b78e6f4ca4081f0d35c1e951299aebf405a`.
Artifacts: `firmware/extracted/recovery-probe-gpio-reserved-20260915/`.
V7 tools use DiagnosticRecoveryProtocolV6 and fresh install/capture/restore
folders, retaining all earlier write guards and rejection of older images.
Staged hashes and laptop no-USB preflight passed.

The stock recovery reboot was requested at 16:50:23 UTC; user reported Android
return and exact baseline was verified at 16:52:32 UTC. V7 Install was launched
in an interactive terminal (Windows PID 36480). Its write/readback status must
be inspected before any phone action. V7 Capture/Restore remain unused.


## Physical V7 result and current state

V7 recovery installation and all twelve desktop readback checks passed at
16:53:21 UTC. The observer saw fastboot leave at 16:56:50 UTC, but no diagnostic
USB or return to Android before its session finished at 17:03:12 UTC. Laptop
services were restored. Stock recovery is NOT restored yet; V7 Restore is
unused and awaits the user returning the phone to original Android.

The user's short video is preserved in
`captures/gpio-reserved-video-20260915/source.mp4` with extracted frames and
`analysis.json`. Frame 22 at video time 1.133056 seconds is the last readable
frame; frame 23 at 1.167411 seconds is black. The newest readable kernel line
is `[5.091812] initcall regulator_init_complete+0x0/0x78 returned 0`.
The screen is circular: the older 4.8-second lines below it are not newer.
The user reports that the black screen's backlight remains on.

This physically establishes progress past the GPIO 8 boundary and through
late initcalls. No visible panic or PID1 message establishes the cause of the
subsequent black screen. USB PHY/controller and PMIC arbiter probes defer
with -517 in preceding frames; their exact missing dependencies remain to
be determined. The clock and power-domain unused cleanup functions explicitly
log that they are preserving their state. The regulator initcall succeeds;
in the pinned source it schedules delayed completion for 30 seconds later,
so the final line alone is not evidence that it cut display power.

A logging-only init/main.c change is being built offline, with exact-board
and initcall_debug guards. It marks initramfs completion, opening /dev/console,
freeing init memory, read-only memory setup and executing /init. It preserves
the existing operations and their order, with no added hardware accesses.
No new recovery has been installed for this diagnostic.


## Prepared next diagnostic (not installed)

`tools/Instrument-A6LInitMilestones.py` preserves every pinned init/main.c
operation and proves exact reversal after removing the added logging. The
kernel build succeeded. Diskless QEMU passed the ordered milestones through
console opening, init memory cleanup and `/init`, with PID1 READY and two
ALIVE messages, no panic, and no framebuffer writes beyond the bounded area.
This does not emulate the A6L's physical display or USB dependencies.

The packaged candidate retains V7's device tree and overlay byte-for-byte,
as well as the existing RAM disk and command line. Packaging roundtrip and
captured ABL header, decompression, DT selection/overlay/fixups and AVB checks
passed. Artifacts: `firmware/extracted/recovery-probe-init-milestones-20260915/`.
Kernel SHA256: `adb37cd28319c6076fc8fe143a215e31c7d15b0d9a07fc3e4c2cf7fa173772de`.
Recovery SHA256: `5abb7746867573e4afd1243713eb05e47d40b3d0d261ddee08c57b95b789486f`.
No new physical coordinator has been prepared or run. Recovery on the phone
is still V7 (117d...), and V7 Restore remains unused. The latest USB check
still found no device at 3-2 and no ADB connection. Original Android return
must be established before stock restoration and another physical trial.


## Restoration completed and next trial prepared

The user returned the spare to Android. V7 Restore (Windows PID 28292) wrote
and independently read back stock recovery at 17:54:42.917315 UTC. Original
Android and laptop services were verified at 17:55:06.611125 UTC. All twelve
copied before/after regions passed desktop hashes; only recovery changed.
Evidence: `captures/capture-diagnostic-restore-v7/desktop-verification.json`.
V7 Restore is now USED. Stock recovery is installed.

V8 wrappers use DiagnosticRecoveryProtocolV7, pinning the new 5abb... image,
and retain all geometry, identity, transfer, readback and zero-BCB guards.
Six fault-injection checks passed. The image and tools were staged in fresh
laptop paths; no V8 Install/Capture/Restore coordinator has run. Tool manifest
SHA256: 5a823087ec2d05e357fc7d5aebd1e2ddc1c66fb7915fba8438601ae1e53ee0d2.

The verified stock recovery was requested through ADB at 17:58:58.744633 UTC,
recorded in laptop `capture-stock-recovery-clear-v4/report.json`. User was
asked to select Enter Recovery, then Reboot, and confirm original Android.
The next install still requires the entire BCB block to be zero; this guard
must not be relaxed.


## V8 installation completed; normal startup pending

V8 Install (Windows PID 37180) verified zero BCB and complete stock recovery,
then installed 5abb7746867573e4afd1243713eb05e47d40b3d0d261ddee08c57b95b789486f.
Full readback passed at 18:03:27.175943 UTC. Poweroff/disconnection and laptop
service restoration passed at 18:03:32.398821 UTC. Twelve desktop copies were
independently verified; non-recovery regions and the zero BCB are unchanged.
Evidence: `captures/capture-diagnostic-install-v8/desktop-verification.json`.

The user's initial Power startup report is black screen. A fresh laptop check
sees no USB at 3-2 and no ADB device. User was asked to keep USB connected and
hold ONLY Power for up to 20 seconds, releasing at the logo. Passive sysfs
observation is recorded in laptop `capture-v8-normal-start-observer`.
V8 Capture and Restore are still UNUSED. No Recovery selection has been
observed for V8; do not interpret this normal-start black screen as a result
of running the new diagnostic kernel.
