# V8 sync-state boundary and power-domain candidate

The V8 recording is preserved in `captures/init-milestones-video-20260915/`.
Video SHA256: cf15f59b0ab3b9d5a5b3b500f856cbe9c8d21bee72e9088bf679c57c73a96929.
The last clear frame is 39 (1.303467 seconds into the supplied clip). Frame 40
has the same text fading; frame 41 is black. The newest readable kernel line is
`[5.094415] calling of_platform_sync_state_init+0x0/0x38 @ 1`.
`regulator_init_complete` returned 0 at 5.089030. No later init milestone or
panic is visible. The circular console's older lines below are not newer.

The USB observer left fastboot at 18:41:28.409130 UTC, received no diagnostic
USB or bytes, and ended at 18:44:19.546110 UTC. Capture session cleanup restored
laptop services at 18:48:22.736777 UTC; Android was not back at that deadline.
Later the spare returned to Android and V8 Restore ran (Windows PID 32676).
Stock full readback passed at 18:58:13.927827 UTC and original Android/services
at 18:58:37.732162 UTC. Desktop readback verification is tracked separately.
V8 Install, Capture and Restore are now USED. Stock recovery is installed.

## Source evidence and limits

Pinned kernel e47d622cb6d2440a9eacdc8bb2df32c037bec7b8:
`of_platform_sync_state_init` calls `device_links_supplier_sync_state_resume`,
which releases deferred sync callbacks. The generic power-domain core registers
`genpd_sync_state` dynamically for provider drivers. `of_genpd_sync_state` clears
`stay_on` and invokes `genpd_power_off`; the simple-provider sync callback has a
similar path. Neither checks `pd_ignore_unused`. The earlier unused-domain
cleanup initcall does honor that option and visibly reported preserving domains.

This is a source-backed hypothesis for display loss at the observed boundary,
not proof of which callback ran or whether the kernel hung. Interconnect/clock
callbacks must not be assumed from a function name; the SDM660 driver definitions
examined did not directly define them. The generic power-domain registration is
the concrete lead. USB PHY/controller, eMMC and a PMIC temperature-alarm child
also defer in the video. Their underlying dependencies are still unestablished.
The child temperature-alarm messages must not be described as failure of the
entire PMIC arbiter.

## Candidate

`Prepare-A6LKeepBootDomains.py` adds two early-return guards in power-domain
sync callbacks. They apply only when the machine is `hisense,hlte730t` AND
`pd_ignore_unused` is set. This preserves firmware domain state and `stay_on`,
prints which provider is preserved, and avoids power-off attempts in these
sync callbacks. Other boards and operation without the option retain the
original code. Other runtime-PM code is unchanged, but retained `stay_on` can
also prevent subsequent domain shutdown: this is a temporary bring-up policy,
not a completed production power-management implementation.

The actual extracted original and modified functions passed 24 inert-domain
cases each: direct/onecell/simple/off/null/unmatched-provider routes with all
board/option combinations. Original-code controls exercise the power-off path.
These tests do not simulate hardware or locking concurrency. Diskless QEMU
reached the ordered init milestones, RAM PID1 READY and two ALIVE messages,
without panic or writes past the bounded framebuffer. The virtual hardware
is not a physical A6L test of the new power policy.

Kernel SHA256: 4cc5b6b159bdcef531a4e2916d2e4b7a40598e666f11b61625901d8f41a62ca8.
Recovery SHA256: d252fa62e3803c877844d6644200d4b91626b93424352291604c1ca50a02a5a1.
Artifacts: `firmware/extracted/recovery-probe-keep-boot-domains-20260915/`.
Device tree/overlay remain identical to V7; RAM disk and command line are
unchanged. Packaging roundtrip and captured ABL checks passed.

Fresh V9 coordinators use DiagnosticRecoveryProtocolV8. Six transfer/geometry/
payload fault checks passed. Manifest SHA256:
a1fd50c4a7bbd3e9ea5d434692c68a29b1d8fc6aeea29a931f0c3c8fa18e56bc.
No V9 physical coordinator has run. Staging/preflight and stock recovery Reboot
cleanup must finish before V9 Install, whose full-zero-BCB guard remains intact.


## V9 installed

V8 restoration's twelve local readbacks passed. V9 staged tools/image preflight
passed with USB access disabled. Stock recovery was requested at
19:03:31.182455 UTC; original Android returned and was verified at
19:04:55.686405 UTC.

V9 Install (Windows PID 7316) verified the entire zero BCB block and stock
recovery, then installed the d252... image. Full readback passed at
19:06:12.777170 UTC. Poweroff/disconnection and services were verified at
19:06:17.964273 UTC. V9 Install is USED; Capture and Restore remain UNUSED.
Stock recovery is not installed now. User was asked to hold only Power up to
20 seconds and release at the logo, then confirm original Android. Desktop
readback copy verification is recorded in the preparation checkpoint.


All twelve V9 copied readbacks passed desktop hashes, and original Android
returned after installation. V9 Capture (Windows PID 38652) began observing at
19:09:15.711203 UTC and saw fastboot at 19:09:20.268303 UTC. User was asked to
record video, select Recovery once and leave buttons alone. V9 Restore remains
unused. No V9 physical kernel result is established at this checkpoint.


## V9 physical result: init execution attempt reached

The photo in `captures/keep-boot-domains-screen-20260915/` shows the guarded
power-domain callbacks preserving domains, sync-state init returning, all
init milestones completing, and exec /init attempted at 5.394860 seconds.
Kernel messages continue to about 38 seconds. No visible panic; user observed
a stable lit screen for four minutes. Deferred probes include the USB PHY,
USB controller, MMC and the PMIC temperature-alarm child, not the whole PMIC.
The underlying supplier failure and successful userspace entry are not yet
established. Original PID1 prints to /dev/console rather than /dev/kmsg.

Capture ended at 19:17:17.373465 UTC with no diagnostic USB and services
restored; Android was absent then. User subsequently restarted twice and
original Android was verified. V9 Restore launched via interactive terminal
PID 37112; inspect its result before any subsequent physical operation.


V9 stock recovery restoration completed with full readback at
19:24:52.010792 UTC; Android and services verified 19:25:15.674836 UTC.
All twelve desktop readbacks passed. All V9 coordinators are USED.
Stock recovery Reboot cleanup clear-v6 requested at 19:29:20.372008 UTC;
user completed it and original Android verified at 19:31:06.828432 UTC.
No V10 image has been installed. Revised RAM PID1 source sends only new
status messages to /dev/kmsg, never replayed kernel messages; mounts debugfs
read-only and prints bounded deferred-device snapshots at startup and two
later intervals. The runtime printk_devkmsg setting is set to on to avoid
losing bounded status bursts. Kernel, device tree and hardware policy remain
V9. Build, QEMU and packaging tests pending.
