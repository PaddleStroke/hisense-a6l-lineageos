# V12: isolate USB from MMC initialization

V11's photos show the QUSB2 PHY and USB controller probes returning zero,
unlike V10's missing-regulator deferrals. The phone then stopped displaying
diagnostics around 4.71 seconds, showed an Android splash and ended in the
charging screen. No diagnostic USB enumeration or RAM userspace was observed.
There is no captured panic or established cause for the restart/shutdown.

The built System.map places efi_shutdown_init and efi_earlycon_unmap_fb after
register_update_efi_random_seed, followed by the previously successful late
initcalls. The last photographed return is therefore not evidence that this
function failed. MMC is also probing asynchronously in the photographs.

V12 changes exactly one flattened device-tree property relative to V11:
`/soc@0/mmc@c0c4000/status`, from `okay` to `disabled`. This avoids probing the
storage controller while retaining the corrected regulator constraints and
the USB providers. It is an isolation experiment, not a claimed MMC fix.
The RAM diagnostic does not need persistent storage to report its status.

Kernel, ramdisk, overlay, command line and load addresses are unchanged.
The compiled V11 DTB is preserved; the new DTS and DTB have separate names.
The only DTC warning was the existing duplicate I2C/SPI unit address.
Packaging verifies the exact one-property difference and byte-identical
other components, then a boot image unpack/repack roundtrip. The first
packaging attempt dropped a valid empty board argument; its artifacts are
preserved under `recovery-probe-usb-only-20260916-packaging-attempt1`.

All captured-ABL checks passed: header, old overlay implementation and
negative controls, DT fixups, decompression, three PMIC selectors, and the
unlocked AVB fixture. These are offline routines, not a hardware boot test.
The identical kernel/ramdisk reuse V11's RAM-userspace QEMU validation;
QEMU has not simulated the physical A6L MMC/USB dependency change.

The eight V12 tools were generated from hash-verified V11 tools using only
version, path and candidate-hash substitutions, plus rejection of the old
V11 payload. Six transport fault tests passed. Laptop staging verified all
eight file hashes and passed the preflight with USB enumeration disabled.
The existing recovery-only sector bounds and independent full-readback
requirements remain unchanged.

Candidate SHA256:
`95c7ccaea2803f31db413ab905e4e9d045f0149d0b98e6b8627715a15486477d`

V11 stock restore completed full readback at 07:44:35.739434 UTC;
Android/services verified at 07:44:59.436384. All twelve desktop copies
passed size/hash checks. Stock recovery's Reboot was completed and Android
verified again at 07:50:11.217905 (`capture-stock-recovery-clear-v8`).

Current physical state is recorded in
`firmware/extracted/recovery-probe-usb-only-20260916/preparation-checkpoint.json`.
V12 Install launched at 07:52:30 UTC in a visible laptop GNOME Terminal.
At the last inspection it awaited sudo; no EDL worker had started.

For future password prompts, open GNOME Terminal on the laptop over SSH,
using the observed display/session environment from
`systemctl --user show-environment`. The user enters only the password.
Windows consoles started by this agent were not visible, and the embedded
exec terminal did not expose its prompt in the user's app terminal.

V12 physical installation completed full readback at 07:55:25.420000 UTC,
then poweroff/USB disappearance and laptop services at 07:55:30.611995.
All twelve desktop copies passed verification; only recovery changed and
the entire BCB remained zero. The phone is awaiting normal Android startup
before the separate diagnostic Capture stage. Capture/Restore are unused.
