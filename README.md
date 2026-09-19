# Hisense A6L LineageOS port

**Current status (18 September 2026):** see the maintained
[port checklist](docs/port-status.md). V45 passed real Android rendering,
front touch movement, Power/Volume events, brightness and battery telemetry.
The full Android interface, native GPU/display lifecycle and rear e-ink remain
unfinished. E-ink key events and physical vibration need investigation.
Stock Android is running; verified V45 recovery remains installed.

Findings are preserved in dated `docs/` reports, raw `captures/`, `research/`,
and hashed firmware/build/test artifacts. The detailed
[latest physical report](docs/combined-controls-v45-results-20260918.md)
separates software success from physical confirmation.

**Historical checkpoint log follows. Older “current” and “active” statements
below describe their date, not the current session.**

Latest checkpoint (2026-09-17): V41 passed real Android minigbm allocation of
full-screen RGBA/RGBX buffers on the phone, including metadata, importing into
a fresh process and verifying every pixel. ADB stayed alive and global mounts
were unchanged. It reused V38 recovery with RAM-only payloads. Graphics service
binaries are built; running the compositor and full Android UI remains ahead.
See `docs/android-allocator-v41-20260917.md` and the newest resume entry for
return-to-stock and host cleanup status.

Latest checkpoint (2026-09-17): V40 produced visible colour bars and a gradient
on the physical LCD through simpleDRM; atomic updates, shared graphics buffers
and cleanup passed. Stock Android and host cleanup were verified afterward.
This is a display/allocator bring-up path, not a running Lineage interface.
See `docs/android-display-v40-20260917.md` and the newest resume entry.

Current checkpoint (2026-09-17): V39 passed real Android Binder service
communication and read-only system/vendor ext4 mounts, with all five sampled
files matching the backup. It reused V38 through authenticated ADB without
flashing. See `docs/android-integration-v39-20260917.md` for evidence and limits.
V38 physically boots Android first/second-stage
init on Linux 7.2.3, loads SELinux policy in development permissive mode, and
provides an authenticated root ADB shell. USB remained connected while all ten
firmware read hashes matched the backup. This is a RAM recovery environment;
full Lineage framework, graphics, vendor compatibility and e-ink remain ahead.
See `docs/android-ram-v38-20260917.md` and the newest entry in
`docs/resume-next-session.md` for phone/cleanup state. Older checkpoints below
are historical.

Current V11 state (2026-09-16): corrected recovery installed with full and twelve desktop readbacks verified; original Android returned. Capture wrapper launched PID 25860, but laptop SSH timed out twice during status checks. Actual observer/fastboot state is not yet verified. User asked to keep laptop awake/check Wi-Fi and report phone screen. Do not relaunch any coordinator or request Recovery selection until remote state is inspected. V11 Restore remains unused.


Current V11 trial (2026-09-16): V10 proved physical RAM userspace through 130 seconds; USB/storage report unavailable regulators. V10 restored to stock, twelve desktop readbacks passed, stock Reboot cleanup verified. V11 corrects only PM660L L4 min from 2.950 to 2.944 V under the unchanged 2.950 V cap, plus the logging newline. All offline and staged tests passed. V11 Install launched (PID 32580); inspect its current capture before any other phone action. V11 Capture/Restore unused.


Active V10 trial (2026-09-16 05:39 UTC): diagnostic recovery installed and all twelve desktop readbacks verified. Original Android returned. V10 Capture USED/running (Windows PID 2060); fastboot confirmed 05:39:42 UTC. User asked to record and select Recovery once, then leave buttons alone. Await physical results; V10 Restore UNUSED. Stock recovery is not installed. Inspect capture-probe-serial-v10 before any subsequent phone operation.


Current state (2026-09-16 05:37 UTC): V10 diagnostic recovery installed; complete readback passed, spare powered off and laptop services restored. User asked to start normal Android with Power. V10 Install USED; Capture/Restore UNUSED. Stock recovery is NOT installed. Desktop readback transfer in progress.


Current state (2026-09-16T05:37:01.630333+00:00): user returned; laptop and original Android verified. V10 Install launched (Windows PID 12416). Inspect capture-diagnostic-install-v10 before any other phone operation. V10 Capture/Restore remain unused. Previous overnight state below is historical.


Current overnight state (2026-09-15): spare on STOCK ANDROID with STOCK
RECOVERY. V9 is fully restored; twelve desktop readbacks passed. Stock recovery
Reboot cleanup was completed and Android verified at 19:31:06 UTC.
V10 is built, QEMU/visible-screen/packaging/captured-ABL tested, transferred,
and laptop-preflight verified. Its installation launch was interrupted before
execution: no V10 capture, process or surviving terminal exists. All V10
coordinators are UNUSED. No password prompt or phone operation is pending.
The user has gone to bed. Stop physical work until they are beside the spare.
Resume from `docs/resume-next-session.md`; the next step is guarded V10 Install
and a camera-assisted recovery test. Do not ask for the unlock authorization
again. Dependency map and later milestones are prepared in the runbook.

Historical checkpoints follow; the state above supersedes them.

Active trial (2026-09-15 19:09 UTC): V9 power-domain preservation diagnostic
is installed with full readback and twelve desktop hashes verified. Original
Android returned after installation. V9 Capture is USED/running (Windows PID
38652); observer began at 19:09:15 UTC, and fastboot was confirmed at 19:09:20.
The user was asked to record video and select Recovery mode once, then leave
buttons alone. No V9 kernel result is established yet. V9 Restore is UNUSED;
stock recovery is NOT currently installed. Laptop probing services are paused
by Capture. Inspect capture-probe-serial-v9 before another device operation.


Latest state (2026-09-15 19:05 UTC): V8 ended with the last visible entry into
of_platform_sync_state_init at 5.094415 seconds. Stock recovery restoration,
original Android/services and twelve desktop readbacks passed. The new V9
candidate preserves boot power domains during synchronization only on A6L with
pd_ignore_unused; physical effect remains unproven. All offline and staged
checks passed. Stock recovery Reboot returned to Android at 19:04:55 UTC.
V9 Install has just been launched (Windows PID 7316); inspect its laptop capture
before any device action. V9 Capture/Restore are unused. See
`docs/keep-boot-domains-20260915.md` and its preparation checkpoint for details.
This supersedes the prior V8 and V7 states below.


Active V8 trial (2026-09-15 18:40 UTC): original Android was verified after the
Power restart. V8 Capture is now USED/running (Windows PID 50104). Observer
started at 18:40:19 UTC and fastboot was confirmed at 18:40:25 UTC. User was asked
to record video, select Recovery mode once and leave all buttons alone. V8
init-milestones recovery remains installed; V8 Restore is UNUSED. No V8 kernel
boot result is established yet. Laptop probing services are paused by Capture;
inspect its session and USB report before any further device operation.


Latest checkpoint (2026-09-15 18:03 UTC): V8 init-milestones diagnostic
recovery is installed. Exact image 5abb... passed full readback at 18:03:27 UTC;
zero BCB was verified before and after the write. The installer powered the
spare off and verified USB disconnection and restored services at 18:03:32 UTC.
The user was asked to start normal Android with Power and have a camera ready.
V8 Install is USED; Capture and Restore are UNUSED. All twelve desktop readback copies
passed independent hashes. Only run Capture after original Android is verified.
The image adds late-init logging to locate the V7 display loss after 5.091812
seconds. It retains the GPIO reservation. Stock recovery is NOT installed now.
This supersedes the historical trial states below.


Latest checkpoint (2026-09-15, after V7): GPIO reservation advanced the physical
kernel to late initialization. The last readable frame shows
regulator_init_complete returning 0 at 5.091812 seconds; the LCD then goes black
with backlight on. No diagnostic USB appeared. Capture cleanup restored laptop
services at 17:03:12 UTC. V7 diagnostic recovery remains installed; stock restore
is pending the user's Power restart to original Android. V7 Restore is unused.
A logging-only next kernel is being prepared locally. See
`docs/gpio-reservation-20260915.md` and the V7 installation checkpoint JSON.
This supersedes the historical trial states below.

Target: LineageOS 24.0. Status: first system-image compile probe built and
filesystem-verified; no boot-tested or flashable A6L ROM yet.

Active trial (16:55 UTC): the kernel's GPIO 8 read was isolated as the last
visible operation. A new device tree reserves GPIOs 8–11, matching several other
SDM660 phones. This candidate is installed in recovery with full readback passed;
the exact spare is in fastboot and logging is armed for a recorded Recovery
selection. V7 Restore remains unused. See the recovery-access checkpoint before
operating the spare. The reservation has not yet been proven physically.

Prior state (16:27 UTC): stock recovery is restored and original Android and
laptop services are verified. The new recording stops at the pin-controller
probe entry. The user confirms Android restarted automatically without further
buttons; USB returned about 100 seconds after leaving fastboot. No diagnostic
USB appeared. Finer pin-controller logging is being built offline to identify
the exact stage or GPIO read. This supersedes the earlier states below.

Current state (2026-09-15 14:43 UTC): original stock recovery is restored and
fully read back; original Android and laptop services are verified. A video
frame confirms the new kernel executes on the physical A6L and writes readable
LCD text through the early framebuffer console. The last visible line is at
0.153186 seconds, after the hardware-lock probe succeeds. No diagnostic USB or
PID1 execution is established yet. The next diagnostic adds probe-entry logging to locate the next operation
before the screen goes black; its emulator, packaging and captured-bootloader
checks pass. Physical installation is pending stock recovery boot-message
cleanup and user readiness.
See [the early-console work](docs/early-console-20260915.md) and the
current recovery-access checkpoint before operating the phone.

Latest physical milestone (2026-09-15 13:41 UTC): the corrected diagnostic
recovery passed installation/readback and was explicitly selected. The screen
showed a boot logo then went black; no diagnostic USB appeared. Original Android
returned after a user Power press at an uncertain time. The complete stock
recovery was subsequently restored and read back; Android and laptop service
restoration passed. That earlier trial did not establish new-kernel execution.

The original device-tree symbols/fixups incompatibility is reproduced and fixed
in captured bootloader code. The corrected image passes header, AVB, board
selection, overlay and DT-fixup checks offline. The earlier stock-body/no-footer
control also booted physically, ruling out the missing footer alone as the cause.
Fresh live vbmeta matches the backup. Further early-boot diagnostics are needed.
See the [recovery-access checkpoint](docs/recovery-access-20260915.md).

Kernel milestone: a new minimal A6L device tree and Linux 7.2.3 prototype now
compile. That kernel boots our static diagnostic RAM filesystem in an ARM
emulator. Physical early kernel execution and boot-framebuffer text are now
confirmed; full boot, USB and normal display drivers remain unverified. See
[the kernel prototype checkpoint](docs/kernel-prototype-20260914.md). The Windows
USB connection blocker is resolved: WSL fastboot reached the spare and rebooted
it to stock Android. The bootloader was subsequently unlocked as recorded below.
An [offline bootloader inspection](docs/bootloader-inspection-20260914.md) found no
standard fastboot boot handler and identified the fixed kernel load offset.
A header-adapted kernel passed a focused emulator test at that offset.
The [recovery diagnostic candidate](docs/recovery-probe-20260914.md) is now
packaged with its own embedded overlay and has passed 11 offline checks.
An exact stock recovery restore image and one-partition restore description
are prepared. The candidate was initially held pending access/unlock validation;
physical stock recovery entry and return to the same locked/green Android state
were verified before the subsequent unlock and installation recorded above.
The subsequent USB packet query disconnected before any reply; a Power-button
restart returned the spare to the same verified Android state. That transport
failure was later resolved on the Linux laptop as described below.
The [standard-client transport follow-up](docs/fastboot-transport-20260914.md)
reproduced alternating OKAY/FAIL responses, including with delays and command
padding. A single `getvar all` succeeded and all 58 backed-up partition sizes
match its output. The spare returned to verified stock Android. A guarded
[direct Linux laptop comparison](docs/linux-laptop-20260915.md) also reproduced
USB disconnection with two standard client versions and with fwupd paused.
The screen's shutdown prompt was identified in the captured Qualcomm boot
manager. Disabling USB link power management for its fastboot identity then
allowed all eight queries and an automatic reboot to verified Android to succeed.
That scoped host workaround also passed 1 MiB and full 64 MiB diagnostic transfers
to RAM, followed by an automatic reboot to verified stock Android. The 64 MiB
transfer took 1.803 seconds; the image was not executed or installed.
A guarded source-built vendor client passed transport tests and a physical
read-only preflight. The [unlock checkpoint](docs/unlock-checkpoint-20260915.md)
records the exact firmware audit and procedure. The explicitly approved vendor
unlock and custom-key persistence commands succeeded on 2026-09-15. The reboot
reached a decryption-error page requesting a factory reset. After that reset,
the original Android installation returned with boot complete=1, flash.locked=0
and verifiedbootstate=orange. Unlock persistence is verified. Normal relocking
and protected-state restoration cannot be promised.
The stock recovery write/readback test subsequently passed. The first diagnostic
installation then passed as recorded at the top of this file.
The [laptop recovery-access checkpoint](docs/recovery-access-20260915.md) records
the successful Linux EDL read: the complete 64 MiB recovery and both GPT regions
match the verified backup. The phone returned to unlocked/orange Android and
both laptop probing services were restored. A forced Power restart from the
stock recovery selection screen returns to that screen. Its graphic is part of
the recovery ramdisk, so it cannot serve as an escape route after replacing
recovery. Normal Android return was subsequently verified. On the second button
attempt, a powered-off spare entered fastboot with only Volume Up held while
USB was connected. Both persisted unlock flags read true and Android returned.
The guarded stock-recovery write test then passed: one 64 MiB write, full matching
readback, unchanged other checked regions, and successful stock recovery plus
Android startup. Host settings were restored. Hardware recovery entry after
clean poweroff was then verified, followed by diagnostic installation.

Development has started in `device/hisense/a6l/`: measured hardware facts and a
system-only LineageOS 24 compile probe. The first system-image build completed
successfully in 3h41m24s; offline filesystem and packaging checks passed. The
separate first-stage init component built in 2m42s and passed ELF inspection.
Neither result has been boot-tested on the phone. See
[the build checkpoint](docs/build-checkpoint.md),
[kernel/boot investigation](docs/kernel-investigation-20260914.md), and
[e-ink analysis](docs/eink-port.md).

Firmware backup completed and verified on 2026-09-14: all 58 GPT partitions
other than userdata, including boot/recovery and calibration partitions.
Local files and verification manifest: `firmware/raw-backup-20260914/`.
This excludes eMMC hardware boot areas/RPMB/fuses and is not a tested restore.
It also does not cover the separate e-ink SPI flash region. The stock driver's
read interface was analyzed, but normal ADB permissions denied the backup
attempt before any read; the temporary reader was removed.
The approved vendor unlock and custom-key persistence commands have now been
performed. The exact existing stock recovery was rewritten and verified once.
The fixed diagnostic recovery was also written, verified, tested and replaced
with the verified stock recovery. Normal boot, system, bootloader and calibration
partitions have not been flashed.
Offline boot inspection subsequently found Magisk metadata and AVB flags=2 in
the captured firmware. The backup reproduces the phone's captured state; it is
not authenticated as an untouched factory release. See
[boot compatibility and provenance](docs/boot-compatibility.md).

The official manifest was cloned into `lineage-manifest/`, with local branch
`a6l-bringup` based on upstream `lineage-24.0` commit
`ad6b6d6bfc16cb0cd3c39db18685a72a6a43985a` (2026-09-14 inspection).
Its AOSP base is `android-17.0.0_r1`. This is a manifest checkout, not a full
Android source sync. The complete source is now synced separately in WSL at
`/home/a6l/android/a6l-lineage24`; its pinned manifest is also saved as
`lineage-manifest/a6l-source-revisions.xml`.
No GitHub fork has yet been created: the connected API lacks
a fork operation and the in-app browser requires GitHub sign-in.

## Start here

1. Connect only the spare A6L, on stock firmware, with USB debugging enabled.
2. Run `tools/platform-tools/adb.exe devices -l` and approve the phone's RSA prompt.
3. In PowerShell 7, run `./tools/Collect-A6L.ps1 -Serial '<serial from adb>'`.
4. Review the resulting local `captures/` directory. This is diagnostic evidence,
   **not a firmware backup**. Permission-denied results are useful; do not root
   or alter permissions to make this initial collector work.

Read [the investigation and recovery plan](docs/bringup.md). No flashing or
unlocking is needed for the first capture. Keep captures and firmware out of
public repositories; they can contain device identifiers, calibration, and
proprietary files.

## Local references

- `references/Hisense-A6L/`: tombaczynski's rooting and downgrade reports.
- `references/Hisense_A6L_Eink_Display/`: WanderingArrow's experimental e-ink work.

These are separate upstream repositories retained for review, not scripts to run
on the phone. Their claims need independent validation.
