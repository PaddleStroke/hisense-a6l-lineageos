# V38 — Android startup in RAM

V37 proved physical storage reads: all ten hashes across two passes matched,
153,268,224 bytes were read, and USB remained alive. V38 adds Android userspace
to that working foundation before attempting any full system image.

The isolated Linux 7.2.3 build enables SELinux prerequisites. Its hardware source
patches and the exact working V37 device tree are retained. The RAM image includes
the built Android first-stage init, recovery second-stage init, monolithic
recovery SELinux policy, authenticated USB ADB, and an Android-managed diagnostic
service. The existing ACM logger and guarded direct read-only storage hash test
remain available. Automatic startup mounts only virtual filesystems.

This development image uses SELinux permissive mode. It does not establish an
enforcing production policy, vendor compatibility, Android graphics, or e-ink.
The full Lineage system image is not installed by this test.

Offline evidence:

- Kernel/module: `firmware/extracted/android-init-kernel-20260917` and
  `android-init-module-20260917`.
- Actual Android first/second-stage init, policy load, ADB service and diagnostic
  service passed diskless QEMU: `android-ram-v38-20260917-r2/qemu-report.json`.
- A6L framebuffer and load offset passed: `android-ram-probe-v38-20260917-r2`.
  The first attempt incorrectly treated SELinux audit rate limiting as diagnostic
  printk loss. Its evidence is preserved. The corrected check permits only
  `kauditd_printk_skb` suppression and still rejects diagnostic log suppression.
- Recovery package: `recovery-probe-android-ram-v38-20260917`.
- Candidate SHA-256:
  `54b0d7b2502b71d8493e4669f2081e46d9ec4ba7a23c275373d735134977cbbb`.
- RAM SHA-256:
  `b524aa6fcb5c408b35450830c975a4e4c87246d3ac449405fd3c7850c555aec5`.

Physical acceptance requires Android service/version markers, the ADB function
and running daemon, all ten expected read hashes, reader exit zero, and a later
heartbeat. Separately verify a host-authenticated ADB shell if the composite USB
enumerates. Serial acceptance normalizes CRLF and rejects a V37-only result.

At this checkpoint V38 is packaged, not yet installed. The spare remains in stock
Android with V37 recovery. Finish captured-bootloader and host workflow checks,
stage the pinned files, install once, and verify all twelve copied readbacks.

Installation launched once at 12:48:30.365496 UTC, PID 212561. Captured bootloader checks, protocol fault injection (6), transition guards (4), collector regression (25), all nine staged tool hashes and spare preflight passed. Await completion and independent desktop readback verification before requesting startup.

Installation completed at 12:49:01.299368 UTC. The laptop reported full readback, poweroff acknowledgement, EDL disappearance and restored host services. All twelve copied readbacks independently passed on the desktop (`captures/capture-diagnostic-install-user-v38/desktop-verification.json`), including preserved boot-control and security state. User asked to start stock Android with Power; capture remains unused. V38 physical Android RAM startup is still untested.

## Physical result — passed

The 12:51 UTC trial completed with 172,693 bytes of saved serial output, SHA-256
`b7b6e8683e20f3ee9ed9f12fb9f0f3d5d5c1eec4626de0d5a2a8cc59fcd9fe13`.
Android second-stage `/system/bin/init` is PID 1. SELinux policy loaded in the
intended permissive mode. The laptop opened an authenticated root ADB shell;
both ADB authentication properties are 1. Mount inspection confirms only RAM
and virtual filesystems. All ten hashes passed over 153,268,224 bytes in 4,722 ms,
with child exit zero and USB configured through the 40-second heartbeat.

Evidence is in `captures/capture-probe-serial-user-v38/analysis.json`,
`adb-read-only-verification.json`, `adb-dmesg.txt` and the serial directory.
The diagnostic remains a minimal Android recovery environment, not full Lineage.

ADB shell commands work despite warnings about missing generated linker config.
Ordinary `adb reboot` returned zero but its text reported missing
`/system/bin/reboot`; this did not initiate a reboot. A subsequent explicit
`/system/bin/toolbox setprop sys.powerctl reboot` was accepted and USB disappeared.
Stock Android had not reappeared at the next checks, so manual Power startup was
requested. Do not count remote restart as validated until its outcome is known.

Next integration work is an explicit fixed-partition fstab/root layout and the
Android 9 vendor policy/VNDK compatibility strategy, followed by Android core
services and display/hardware integration. Add the recovery reboot executable
and a suitable linker configuration when preparing the next RAM environment.

Final cleanup: exact stock Android fingerprint, boot completion, unlocked security state and USB port verified at 12:57:40.690115 UTC. Host services are restored with no owned pause. The coordinator had ended at 12:56:28 before Android returned; its original `android_return_verified=false` is preserved. Separate `post-return-verification.json` records the successful later check. V38 remains in recovery; the spare is running stock Android.
