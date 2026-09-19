# Recovery access checkpoint — 2026-09-15

Current V11 trial (2026-09-16): V10 proved physical RAM userspace through 130 seconds; USB/storage report unavailable regulators. V10 restored to stock, twelve desktop readbacks passed, stock Reboot cleanup verified. V11 corrects only PM660L L4 min from 2.950 to 2.944 V under the unchanged 2.950 V cap, plus the logging newline. All offline and staged tests passed. V11 Install launched (PID 32580); inspect its current capture before any other phone action. V11 Capture/Restore unused.


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

## Current state (supersedes historical pending steps below)

**V7 GPIO-reservation trial armed at 16:55 UTC.** V6 was restored to complete
stock recovery with twelve local readbacks verified. The GPIO 8–11 reservation
candidate then passed all offline checks and staged preflight. V7 full install
readback passed at 16:53:21.140467 UTC; poweroff/services at 16:53:26.329900 UTC.
Recovery now contains `117d78d628feb0e9f0f41e6f81c77b78e6f4ca4081f0d35c1e951299aebf405a`.
After user Power startup, exact original Android was verified. V7 observer
started at 16:55:10.432972 UTC; exact spare fastboot observed at 16:55:14.989028 UTC.
The user is being asked to record Recovery selection with no further buttons.
V7 Capture is USED/running; V7 Restore is UNUSED. No v7 boot result yet.
See [GPIO reservation evidence](gpio-reservation-20260915.md).

**V6 capture armed at 16:40 UTC:** original Android passed the coordinator's
baseline checks; observer started at 16:40:43.330848 UTC and exact spare fastboot
was observed at 16:40:47.889651 UTC. The user is being instructed to record
Recovery selection and leave all buttons alone afterward. V6 Capture is USED
and running; V6 Restore remains UNUSED. All twelve local installation files
passed `captures/capture-diagnostic-install-v6/desktop-verification.json`.
No physical v6 result has been observed yet.

**V6 pinctrl diagnostic installed at 16:39 UTC.** Full readback passed at
16:38:56.835676 UTC; poweroff, USB disappearance and service restoration passed
at 16:39:02.068523 UTC. The boot message was confirmed zero. Recovery contains
`ef81c365f79648b66257d2ce55f2553b829629157874c629d0bad7899abfb7e2`.
The user is being asked to start original Android using Power only. V6 Capture
and Restore are still UNUSED. Before/after install files are being copied to
`captures/capture-diagnostic-install-v6/`. Detailed source and verification:
[pinctrl diagnostic](pinctrl-trace-20260915.md).

**V5 trial complete; stock restored at 16:27 UTC.** User photograph shows the
last visible line `[0.156396] A6L probe begin 3100000.pinctrl driver sdm660-pinctrl`.
The user explicitly reports no further button press, and Android returned
automatically in 1–2 minutes. The USB timeline confirms 100.32 seconds between
leaving fastboot and the first Android USB identity. This establishes autonomous
return for this trial, not the reset mechanism. Android reports bootreason
`bootloader`; no diagnostic USB appeared. The outer probe marker alone does not
prove entry to the actual driver callback. Screen evidence and analysis:
`captures/probe-trace-screen-20260915/`.

V5 restore full readback passed at 16:27:02.518794 UTC; original Android and
services verified at 16:27:26.195995 UTC. All twelve copied files independently
passed in `captures/capture-diagnostic-restore-v5/desktop-verification.json`.
Recovery is original stock. All v5 physical coordinators are USED. New offline
instrumentation adds actual pinctrl callback stages and markers before/after
existing GPIO direction reads. It adds no hardware accesses or changed GPIO
values. Build/validation are in progress; no further phone action is active.

**V5 physical capture armed at 16:22 UTC:** exact original Android was verified
by the capture coordinator. The observer started at 16:21:54.800938 UTC and
recorded the spare in fastboot at 16:21:59.358534 UTC. The user is being asked
to record before selecting Recovery and leave the buttons alone afterward.
All twelve local install-region files passed independent verification in
`captures/diagnostic-install-laptop-v5-20260915/desktop-verification.json`.
V5 Capture is now USED/running; V5 Restore remains UNUSED. No boot result yet.

**V5 installed and verified at 16:20 UTC:** full recovery readback passed at
16:19:56 UTC, followed by acknowledged poweroff, USB disappearance and restored
laptop services at 16:20:01 UTC. Only recovery changed. It now contains trace
candidate `886baf8c66f85b199e3e0d8449731e5e98488d7c38641e7942f31ce54a216c38`.
The complete misc boot-message region was zero before and after this write.
The user has been asked to start original Android using Power only. V5 Capture
must wait for the exact Android baseline, then it will send the bootloader
restart and arm the USB observer. V5 Restore and Capture are still UNUSED.
Copy of all install readbacks to the desktop is in progress.

**16:19 UTC update:** original Android return from stock recovery was verified
at 16:19:11 UTC. The v5 installer was launched interactively (Windows PID 12620).
Its worker began at 16:19:39 UTC and confirmed the complete boot-message region
is zero, along with exact spare/GPT/stock recovery/vbmeta checks. The full 64 MiB
trace diagnostic transfer was acknowledged; independent readback was still in
progress at the last inspection. Do not restart or launch a second worker until
`capture-diagnostic-install-v5/edl/report.json` confirms readback and poweroff.
The v5 capture coordinator expects normal Android after installation/poweroff;
start with Power only, then use the capture coordinator to enter fastboot.

**16:17 UTC update:** the user authorized continuation and the read-only laptop
check passed. The spare was in original Android. One `adb reboot recovery`
command succeeded at 16:17:13 UTC, and USB `18d1:d001` with the exact spare
serial was observed afterward. The user is being asked to enter stock recovery
and choose its Reboot option. Capture: `capture-stock-recovery-clear-v1` on
the laptop. Android return and a zero boot-message check are still pending.
No v5 diagnostic image has been installed. Recovery remains verified stock.
The v5 candidate, staged scripts, and laptop no-USB preflight are ready.

**Stock restored at 14:43 UTC after v4 early-console trials.** The complete
original recovery write/readback passed at 14:42:54.585912 UTC; Android and
laptop services were verified at 14:43:18.371849 UTC. All twelve copied region
files passed desktop hash verification in `captures/diagnostic-restore-laptop-v4-20260915/`.
Recovery is `9688e3dfb349186fb60efc057a618aeaf696b030ad52974932ea83a077b31621`.
All v4 install/capture/restore and v4b/v4c capture sessions are USED.

The user's final readable video frame proves physical prototype kernel execution
and the early LCD console. Last visible progress is a successful
`1f40000.hwlock` probe at 0.153186 seconds; no panic is visible and no diagnostic
USB appeared. It does not distinguish a stopped kernel from lost display power.
The photograph and analysis are in `captures/early-console-screen-20260915/`.

The restore captured `bootonce-bootloader` in misc (SHA256
`8ac9baa0ce2f52dda6debef8f8ffb4fcd8e85d44bf05882dbcb552dd06b46881`), identical
before and after restoration. Unlike earlier zero-BCB captures, the latest
Android-to-bootloader trials therefore must not be described as leaving misc
unchanged from the earlier baseline. No manual misc write was performed.
Android returned normally. Both GPT regions, devinfo and vbmeta also remained
unchanged across restoration. See [early-console details](early-console-20260915.md).

Latest state: complete stock recovery restored and Android/services verified
at **13:41:59 UTC**, after the corrected diagnostic trial. The trial left fastboot
at 13:38:09 UTC, showed a logo then black, and returned to Android USB at
13:39:58 UTC without diagnostic output. The user pressed Power after black;
its timing is uncertain. Do not classify this as an autonomous reset. The
post-boot bootreason remained `reboot,edl`, pstore was absent in stock, and
`/proc/last_kmsg` was permission denied. These do not locate the kernel failure.
The full stock write/readback passed at 13:41:35 UTC, with all other checked
regions unchanged. All twelve copied restore-region files also passed independent
desktop hash verification. Local evidence: `captures/probe-serial-laptop-v3-20260915/`,
`captures/diagnostic-boot-laptop-v3-20260915/user-observation.json`, and
`captures/diagnostic-restore-laptop-v3-20260915/`. All v3 coordinators/capture
folders are now USED; do not rerun them. The next step is early-boot logging
and kernel/USB dependency investigation. `CONFIG_QCOM_WDT=m` is a configuration
fact to investigate, not evidence that a watchdog caused this trial's reset.

At 12:35:38 UTC the spare was verified back in original Android with the complete
original recovery restored. The stock-body/no-AVB-footer control booted its
familiar two-option UI after explicit Recovery selection, and normal system
startup succeeded. The observer saw recovery USB 18d1:d001 at 12:33:23 UTC;
Android return was verified at 12:34:15 UTC. This rules out the missing recovery
footer alone as the cause of the two diagnostic failures.

The subsequent restore completed its full write/readback at 12:35:15 UTC.
All twelve copied files were independently verified on the desktop; see
`captures/stock-control-restore-laptop-v1-20260915/desktop-verification.json`.
Recovery matches `9688e3dfb349186fb60efc057a618aeaf696b030ad52974932ea83a077b31621`.
Fresh vbmeta before and after matches the backup:
`e48632a03179e68b3c059905f613d321b3b37aa789ba72a02b322dffde4f7350`.
GPT regions, devinfo and zero misc are unchanged. Host probing services were
restored. No new-kernel execution has been observed.

Offline captured-code emulation then reproduced a specific rejection in
`ufdt_apply_overlay` (0x259f0). The old base lacked `/__symbols__`, returning NULL
with `Bad main_symbols in ufdt_overlay_do_fixups`. Adding symbols alone exposed
the next requirement: the target-path overlay lacked `/__fixups__`, returning
NULL with `Bad overlay_fixups`. These are actual captured-code failures, not
inferences from a different bootloader source. Their correspondence to the
physical failure is plausible but not yet established by a corrected phone boot.

The correction exports DT symbols (`DTC_FLAGS=-@`), labels `/chosen`, and uses
an external phandle reference in the overlay so dtc generates its fixup table.
It also removes generic model/compatible root properties from the overlay:
captured old ufdt merges these into the base, unlike the newer host validators.
The exact old merger now accepts the corrected pair; its resulting tree changes
only root board-id selection metadata and the diagnostic marker. Hardware
properties and existing references are preserved; additional label phandles
are audited. The compressed kernel and RAM diagnostic remain byte-identical.

The new offline candidate is in `firmware/extracted/recovery-probe-v2-20260915/`,
SHA256 `9c368dba3dc9a3e70167114ea160f224750dc7a9c8d39b4bc72191dd51344ac3`.
Captured ABL header/overlay/selection/fixup/AVB checks passed, including old
failure controls. Guard fault-injection tests and laptop offline preflight passed.
New v3 coordinators and `DiagnosticRecoveryProtocolV2.py` pin the corrected image,
retain exact recovery-only geometry and add live vbmeta checks. At 13:35:34 UTC
installation and complete readback passed; poweroff and services were verified
by 13:35:39 UTC. At that point recovery contained the corrected diagnostic; it was subsequently
restored as recorded at the top.
All twelve copied region files also passed independent desktop hash verification.
The read-only observer armed at 13:36:31 UTC. The physical result and completed
v3 restore are recorded at the top; these capture directories are now used.
See the candidate installation checkpoint JSON for this trial.

## Historical sequence

The independent recovery read using the Linux laptop passed at 10:16:51 UTC.
The complete 64 MiB stock recovery and both GPT regions match the verified
backup. Android returned unlocked/orange and both host services were restored.
A guarded one-partition restore procedure and physical diagnostic entry/exit
validation remain necessary. No recovery image has been flashed or executed.

The original Android fingerprint and unlocked/orange baseline were confirmed
before starting this step. The approved vendor unlock remains saved; do not run
the earlier locked/green collectors or repeat the unlock sequence.

## Prepared laptop kit

`tools/Package-RecoveryLaptop.py` packages the existing inspected EDL sources,
the exact programmer used for the successful backup, reference GPT regions and
pinned Linux Python libraries. All 559 files were hash-verified on the laptop.
The kit lives under `/home/pierrelouis/A6L-usb-20260915/a6l-recovery-kit` and does
not install system packages. The only additional EDL source adjustment changes
the hardcoded libusb `.so` lookup to PyUSB's default lookup, resolving Ubuntu's
installed `.so.0` runtime without a development package.

Archive: `tools/a6l-recovery-kit.zip`, 6,179,750 bytes,
SHA-256 `85cee42432351881b7656f290cb666ad57f65c88213627fd3840858de18fc7d4`.
Libraries: pyusb 1.3.1, pyserial 3.5, pycryptodomex 3.23.0, capstone 5.0.6,
keystone-engine 0.9.2 and colorama 0.4.6; the pure-Python docopt module is copied
from the established local EDL environment and hash-pinned in the manifest.

`Read-LaptopRecovery.py` guards the exact USB port and Sahara identity before
uploading the known programmer. It permits only configuration, fixed reads,
storage information and a normal reset. Program, erase and patch commands are
rejected. Five guard tests pass locally and on the laptop. The fixed reads are
the primary GPT region, saved GPT tail, complete 64 MiB recovery, 4 KiB devinfo
and the first 4 KiB of misc. Only the first two and recovery have expected
pre-reset hashes; devinfo/misc are observations of the new state. A successful
read requires exact lengths, hashes where applicable and final raw-mode ACKs.

The coordinator pauses idle fwupd and ModemManager only after verifying that
fwupd is idle and no modem exists. It preserves their prior active state.
No USB NO_LPM quirk is applied for EDL's different USB identity.

## First physical attempt and correction

At 09:56:54 UTC the first coordinator verified the original Android fingerprint,
boot complete=1, flash.locked=0 and verifiedbootstate=orange, then paused the
services and requested `adb reboot edl`. The exact physical port appeared as
05c6:9008. At 09:57:05 UTC the worker rejected its EDL command-line syntax before
opening USB, running Sahara, uploading the programmer or issuing Firehose XML.
The source's commandless grammar did not accept loader, memory and VID/PID
options together. Its earlier `--help` test had not tested that argument combination.

The corrected worker uses the documented `getstorageinfo` argument grammar in
imported mode, which returns after connection instead of dispatching that nominal
command. `Inspect-RecoveryImports.py` now imports the actual client using these
exact arguments while USB enumeration is explicitly disabled. That check passes
and is required by the coordinator before requesting any phone reboot.

The user was asked to hold only Power about 20 seconds and return to Android.
The first coordinator's 120-second return window expired at 09:59:05 UTC while
the phone remained in EDL. It deliberately left both probing services runtime
masked/inactive. The current v2 coordinator can adopt those masks only after
verifying Android returned and attributing both masks to this completed,
zero-operation failed attempt; it restores their original state after the next
check. No system configuration file or persistent mask was installed.

Evidence: `captures/recovery-access-laptop-20260915/`, including `session.json`,
`worker.log` and `edl/report.json` (zero operations). Preserve that failed result.

## Corrections and successful version 4

Version 2 uploaded the identity-checked programmer, then rejected the library's
automatic one-sector GPT-header read before sending it. The allowlist now
permits exactly LBA 1, one 512-byte sector, on LUN 0 in addition to the five
fixed regions. Android and both services returned; evidence is retained under
`captures/recovery-access-laptop-v2-20260915/`.

Version 3 exposed the upstream client's counterintuitive constructor: an empty
`imported` argument selects imported mode, while a nonempty one selects CLI
dispatch. The worker stopped at the connection handoff and its guarded normal
reset was acknowledged. Android and services returned. Evidence is retained
under `captures/recovery-access-laptop-v3-20260915/`.

The import preflight now tests the actual argument parser, actual configure
method's automatic header read, and actual imported connection handoff with
inert USB objects. It fails if a CLI command is dispatched or the imported
connection is closed. All these checks passed before version 4 ran.

Version 4 completed every fixed read and a normal reset at 10:16:51 UTC.
The original Android fingerprint, boot complete=1, flash.locked=0 and orange
verified boot were confirmed by 10:17:15 UTC. fwupd and ModemManager were both
restored to loaded/active. No NO_LPM quirk was needed. The first configure
attempt received startup logs; its retry and all subsequent operations were
acknowledged. Only configure/read/getstorageinfo/power XML was sent.

Evidence: `captures/recovery-access-laptop-v4-20260915/`. All downloaded region
hashes were independently checked on the desktop; primary GPT CRCs also pass.

| Region | Bytes | SHA-256 |
| --- | ---: | --- |
| Primary GPT region | 1048576 | `0a170df9e55b8c1e2c7ad971f44c2b05ddfa78efc1b47588ebdbefadfdd396b0` |
| Saved tail | 22528 | `14a0aa4ecef49ffe6b477cf43a08da1d457624d6a687e8be2f4d7015b9359946` |
| Complete recovery | 67108864 | `9688e3dfb349186fb60efc057a618aeaf696b030ad52974932ea83a077b31621` |
| GPT devinfo | 4096 | `7d6a4855f19d498a092ff0ffb44a69e114cd915d4d49acb2640035cf70e854ed` |
| First 4 KiB of misc | 4096 | `ad7facb2586fc6e966c004d7d1d16b024f5805ff7cb47c7a85dabd8b48892ca7` |

Plain GPT devinfo is unchanged from before unlocking: bytes 13 and 14 remain
zero despite the persisted unlocked Android state. This supports the protected
storage interpretation; it is not a read of RPMB or the live critical flag.
All first 4096 misc bytes were zero before the subsequent recovery entry test.

## Recovery exit check

At 10:18:54 UTC a guarded `adb reboot recovery` requested the existing recovery.
The user confirmed the two-option screen, then reported that holding only Power
for about 20 seconds returns to the same screen instead of normal Android.
The user was asked to select **正常启动系统** to return to Android. At 10:44:16
UTC a read-only ADB check confirmed the original fingerprint, boot complete=1,
flash.locked=0, orange verified boot and normal boot mode. The laptop's fwupd
was loaded/active and its runtime USB quirk list empty.

The exact graphic was found in the hash-verified stock recovery ramdisk as
`res/images/recovery_init_vision.png`, SHA-256
`e96826dcc612fefa73676fb39264b216346f3513bc0b6d725ebffa824872ed5b`.
It contains both **进入 Recovery 模式** and **正常启动系统**. This is recovery's
own UI, not a bootloader menu that would survive replacement of recovery.
The no-display diagnostic has no automatic normal-boot fallback. Before
installation, validate a physical bootloader/EDL route independent of Android
and the contents of recovery. A reset alone is not a proven escape route.

`tools/Inspect-RecoveryExit.py` reproduces extraction and hash verification of
the selector and saves the exact ABL and ButtonsDxe code under
`firmware/extracted/recovery-exit-20260915/`. The captured ABL prioritizes its
fastboot flag even when RecoveryInit finds a recovery request. Its button flow
sets that flag for USB/charger power-on with scan code 1. ButtonsDxe's maps and
GPIO polling identify Volume Up alone as scan code 1, while Power plus Volume
Up produces a different code. This suggests testing a powered-off phone with
only Volume Up held while attaching USB, rather than assuming Power plus a
volume key enters fastboot. This procedure is inferred from the exact binary;
it has not yet been physically verified. Host fwupd and the scoped NO_LPM quirk
must be prepared before fastboot enumeration, using the established workaround.

Current entry capture and the user's forced-restart result are saved under
`captures/recovery-exit-laptop-20260915/`; `verify-return.json` records the
successful later Android return separately from the entry and user observations.

## Physical button test window

The version 1 button coordinator became ready at 10:48:22 UTC after checking
the unlocked Android baseline, pausing idle fwupd and applying the established
bootloader-only NO_LPM quirk. It issued no Android reboot request and permits
only fixed bootloader state queries and a normal reboot. During the four-minute
window, USB remained connected to the same Android device: no disconnect or
bootloader entry was observed. Therefore the button procedure was not tested;
this is not evidence that the sequence fails on the phone.

At 10:52:23 UTC the coordinator verified normal unlocked Android and restored
fwupd plus the original empty quirk list. Capture:
`captures/button-entry-laptop-v1-20260915/`. The user has been asked for physical
readiness before re-arming. Keep the phone connected until then. The next
launcher selects `Run-LaptopButtonEntry-v2.py`, with a separate capture directory
to preserve version 1 evidence. A cold-start button test alone will not establish
that a hung replacement kernel can be forced off; that exit behavior also needs
to be checked before diagnostic installation.

## Stock recovery write test

`RecoveryWriteProtocol.py` restricts its immutable input to the verified 64 MiB
stock recovery and its XML to LUN 0, sector size 512, start 917504, count 131072.
It requires initial ACK/rawmode=true, exact endpoint byte counts without retries,
and final ACK/rawmode=false. It does not reset or claim readback success itself.
Five fault-injection tests pass on Windows and Ubuntu, covering forbidden
geometry, wrong payload, initial rejection, short writes and final ACK failures.

`Write-LaptopStockRecovery.py` retains the proven reader's exact Sahara identity,
kit hashes, USB port guard and fixed GPT/recovery reads. It verifies that recovery
already equals stock, permits one stock write, then independently reads all five
regions again and compares every hash before resetting. This is specifically a
stock-write validation mode; a later restore from a diagnostic or damaged image
needs a separate reviewed mode because its pre-write recovery hash would differ.

`Run-LaptopRecoveryRestore.py` coordinates that worker using the established
temporary EDL service masks and bounded process deadline. On uncertain write
outcomes it instructs the user to keep USB connected pending review. The code is
staged on the laptop with `-v1` worker/coordinator filenames. Its actual-library
import, XML guard and exact-payload preflight passed with USB explicitly disabled;
the remote report is `recovery-restore-preflight-v1.json`. Launcher switch:
`tools/Run-A6LLaptopTest.ps1 -RecoveryRestore`. It subsequently ran successfully
as recorded below; preserve its capture instead of launching this version again.

No diagnostic installation or physical diagnostic boot has yet occurred.

## Successful physical entry and restore validation

Pierre confirmed he missed the first READY prompt and had not performed that
procedure. With him beside the spare, version 2 was armed at 10:58:06 UTC. USB
disconnected at 10:58:11, and the powered-off phone appeared at 10:58:46 as
18d1:d00d on the exact port/serial with quirk bit 0x400. The user action was:
unplug, shut down from Android, then hold only Volume Up while reconnecting USB.

The fixed product/unlocked/secure and OEM device-info queries succeeded:
sdm660, unlocked=yes, secure=yes, ordinary=true and critical=true. Normal reboot
and original unlocked/orange Android were verified by 10:59:09 UTC. fwupd and
the original empty quirk list were restored. Evidence is
`captures/button-entry-laptop-v2-20260915/`. This proves entry from a powered-off
phone; it does not alone prove how a hung replacement kernel can be powered off.

The stock restore validation followed. At 11:01:09 UTC, after exact spare/GPT/
stock recovery checks, the worker completed one program of LUN 0, 512-byte
sectors, start 917504, count 131072. All 67,108,864 bytes were transferred.
The initial ACK/rawmode=true and final ACK/rawmode=false were received. All five
fixed regions were independently read again: recovery matches stock hash
`9688e3dfb349186fb60efc057a618aeaf696b030ad52974932ea83a077b31621`, and primary,
tail, devinfo and misc match their pre-write hashes. The desktop independently
verified every copied before/after file's length and hash as well.

Original Android returned and both services were restored at 11:01:33 UTC.
At 11:03:15 UTC, a guarded ADB request entered the rewritten stock recovery.
Pierre confirmed its entry menu, entered recovery, selected Reboot and reported
normal Android startup. A separate read-only check at 11:05:16 UTC verified the
original fingerprint, boot complete=1, flash.locked=0, orange and normal mode.
Evidence: `captures/recovery-restore-laptop-v1-20260915/`, including the independent
raw readbacks and subsequent entry/return reports. This validates rewriting the
existing stock image and its startup, not recovery from arbitrary corruption.

## Next diagnostic entry route

The proposed route avoids `adb reboot recovery` for the new diagnostic because
that path left a persistent recovery request in the earlier stock test. Instead,
check that misc has no pending request, install/read back the diagnostic, power
off, and use hardware recovery entry. The diagnostic's PID 1 does not write to
storage. This route still needs physical entry validation before installation.

An offline audit of the exact already-used programmer located `power value="off"`:
its handler compares the literal `off`, acknowledges, waits, and calls the poweroff
routine. The routine selects PMIC shutdown then deasserts PS_HOLD. Evidence:
`firmware/extracted/recovery-exit-20260915/programmer-poweroff-report.json` and
adjacent disassembly. The separately guarded poweroff variant subsequently
passed at 11:11:04 UTC. All fixed reads matched stock, all first 4096 misc bytes
were zero, and `power value="off"` received ACK. EDL USB disappeared, the
physical port had no device after the observation interval, and both laptop
services were restored by 11:11:09 UTC. This test sent no program/erase/patch.
Evidence: `captures/recovery-poweroff-laptop-v1-20260915/`.

Pierre performed the physical sequence and reported a first bootloader-style
page followed by the usual recovery screen. The first page was not separately
identified. He selected normal system startup and reconnected USB. At 11:18:26
UTC, ADB verified the original fingerprint, boot complete=1, locked=0, orange,
normal mode and exact port/serial; both services were active and quirks empty.
Cold physical recovery entry and stock return are established. Forced normal
restart from the replacement kernel remains physically untested.

## First diagnostic installation preparation

`Write-LaptopDiagnosticRecovery.py` and `DiagnosticRecoveryProtocol.py` add two
fixed modes. Install requires stock recovery and all 4 KiB boot-message bytes
zero before writing the exact candidate hash. Restore always writes the exact
stock hash and accepts an existing diagnostic or damaged recovery, saving its
full pre-write bytes first. Both require the exact spare/Sahara, programmer, kit
and both GPT regions before one recovery-only write. No arbitrary image path or
partition geometry is accepted. Full post-write readback must match the selected
payload; the other four checked regions must remain unchanged. Install then
powers off; restore normally resets. Stopped checks/writes never automatically
reset or retry.

Six transfer/payload fault tests passed on Windows and Ubuntu, including real
cross-mode image-hash rejection. Actual Linux library/import and XML preflights
passed without USB access. Helpers are staged as version 1, with separate
install, restore and serial-capture directories. The collector opens only the
exact diagnostic VID/PID/serial on physical port 3-2, sends no data bytes, and
records enumeration transitions even if the diagnostic gadget never appears.
The current installation checkpoint is saved beside the image; the original
`report.json` retains its historical offline status. Installation has been
launched; its result must be checked before any button operation.

## First diagnostic installed and verified

At 11:25:48 UTC, version 1 completed exactly one 67,108,864-byte recovery write
of candidate `ff3c54525d12a7f5d479fdc36cfd6ab0e6e103abcb9e95d398bac9ca58a27a7b`.
The worker verified the exact Sahara identity, all kit/payload hashes, both GPT
regions, original stock recovery and zero first 4 KiB misc bytes beforehand.
Initial ACK/rawmode=true, exact transfer byte counts, and final ACK/rawmode=false
all passed. A separate full recovery read matched the candidate. Primary, tail,
devinfo and misc matched their pre-write hashes. The fixed poweroff command
received ACK; EDL disappeared and both services were restored by 11:25:53 UTC.
Evidence: `captures/diagnostic-install-laptop-v1-20260915/`.

This is the first custom image written to the phone. Normal boot/system and
bootloader/calibration partitions were not written. The image is a minimal
kernel/RAM diagnostic, without the LineageOS system image or display drivers.
At 11:26:36 UTC the serial collector was armed and Pierre was asked to use the
tested physical recovery-entry sequence, then reconnect USB. Hardware boot and
forced return still require results; installation success alone proves neither.

## First trial and complete stock restoration

After the physical sequence, the collector observed fastboot 18d1:d00d with
the exact spare serial at 11:27:31 UTC. Pierre confirmed a START/volume-select
fastboot page. No diagnostic gadget appeared and no serial bytes were received
during the bounded capture. This does not distinguish a different button
selection from image-loading failure or a later failed boot. No fastboot USB
command was sent during this trial. Pierre pressed Power with START selected;
the collector verified original Android and restored host services at 11:30:02.

The prepared restore-from-diagnostic mode then ran. Its pre-write recovery hash
matched the diagnostic exactly; misc remained entirely zero. At 11:31:14 UTC,
one stock recovery write and all independent readbacks passed. Recovery matches
`9688e3dfb349186fb60efc057a618aeaf696b030ad52974932ea83a077b31621`; primary,
tail, devinfo and misc remained unchanged. Normal reset received ACK; original
Android and both services were verified by 11:31:38 UTC. The desktop independently
verified the length/hash of all ten copied before/after region files.

Captures: `captures/probe-serial-laptop-v1-20260915/` and
`captures/diagnostic-restore-laptop-v1-20260915/`. The current phone has stock
recovery again. The replacement kernel has not been shown to execute. The
successful return tested fastboot START, not a forced restart of that kernel.

## Offline checks following the failed trial

`Test-CapturedAblHeader.py` executes the exact captured checker at RVA 0x13f78
in Unicorn, with debug output disabled. Stock and diagnostic headers return
success with their correct computed sizes. Four malformed-header controls
(magic, kernel size, page size, v1 header size) are rejected.

`Test-CapturedAblAvb.py` executes the exact captured libavb entry at RVA 0x8948.
It supplies verified backup partition bytes via host callbacks, an unlocked
state, zero stored rollback indices, a deliberately untrusted public-key result,
and placeholder GUIDs for command-line construction. SHA-256 firmware services
use hashlib; allocation, copy/set, logging and SafeStack use host fixtures.
Stock and diagnostic recovery are both loaded in full, with exact hashes, and
return public-key-rejected result 5. That value is permitted by the measured
unlocked continue mask 0x39 at RVA 0x62d8. A corrupted vbmeta header instead
returns invalid-metadata result 6 with no slot data. No storage mutation callback
is permitted. Reports and disassembly: `firmware/extracted/diagnostic-boot-v1-20260915/`.

The stock image has an algorithm-NONE recovery AVB footer; the diagnostic has
none. However, the exact verifier uses top-level vbmeta flags=2 in the backup,
loading the requested recovery without depending on its footer. The footer
hypothesis is therefore not supported under the tested fixture, and no image
change was made. This is not a fresh read of live vbmeta or a complete UEFI
emulation. DTB selection/fixups, kernel decompression/entry and the actual
selection made during the button sequence remain candidates for investigation.


## Exact DT selection and gzip checks before trial 2

`Test-CapturedAblDtb.py` executes GetSocDtb (0x16618), GetBoardDtb
(0x17818), their matching code and captured libfdt. The spare reports SoC 317,
revision 1.0, platform version 1179648 (18.0), QRD. The fixture uses QRD type 11,
subtype/foundry zero, and enumerates the three PMIC combinations supported by
the captured stock overlay; live PMIC revisions are not measured. Both images'
SoC and overlay selectors pass for all three fixtures (12 positive cases).
Zero/misaligned DTB offsets and a wrong board type fail. A separate deliberately
wrong PMIC fixture still selects the overlay: the captured selector accepts the
primary board match, so PMIC rejection must not be assumed. This behavior is
recorded, rather than described as a negative control passing.

`Test-CapturedAblDecompress.py` executes the captured decompressor (0x146d0)
and its zlib implementation, with host allocation/memory/logging fixtures.
Both images decompress byte-for-byte identically to host zlib. The diagnostic
produces the previously tested kernel hash bce9e6d3... at 28,510,720 bytes,
and returns DTB offset 11,778,619; stock returns 11,684,311. Truncated deflate
and insufficient output capacity are rejected. An initial instruction-count
bound interrupted both positive cases; replacing that bound with the existing
wall-clock deadline allowed both to return in under two seconds each. This
was a harness limit, not evidence of a device boot failure.

Reports: `firmware/extracted/diagnostic-boot-v1-20260915/dtb-emulation.json`
and `decompress-emulation.json`. Overlay application, live DT fixups, the actual
UEFI memory layout and kernel execution still need physical evidence.
The captured entry at 0x1a68 falls back to fastboot if BootLinux returns.

Trial 2 uses the identical diagnostic and rollback payloads, protocol and
partition geometry. Versioned install/restore/capture workers preserve trial 1
captures. The planned entry is cold Volume Up + USB, then explicit selection
of Recovery mode in the fastboot menu. This removes the ambiguity of simply
observing fastboot after a button combination. User confirmed readiness.


At 12:02:12 UTC trial 2 installed the same fixed 64 MiB diagnostic and passed
all independent readbacks, with the other four regions unchanged and misc zero.
Poweroff ACK and USB disappearance passed; host services restored by 12:02:17.
All ten copied before/after files were independently hash-checked on the desktop.
Capture: `captures/diagnostic-install-laptop-v2-20260915/`.
The read-only four-minute serial collector was armed at 12:03 UTC. The user
was instructed to use cold Volume Up + USB and explicitly select Recovery mode.
The current phone therefore has diagnostic recovery installed pending this test.


## Trial 2 result and stock restoration

The user explicitly selected Recovery mode. Fastboot disappeared at
12:04:59.234643 UTC and reappeared at 12:05:02.779203. The user confirmed
"Recovery selected; fastboot menu returns". There was no diagnostic gadget or
serial data. START returned to normal Android; the collector verified its
baseline and restored services at 12:07:44.220495. This removes ambiguity about
the user's selection but does not reveal which loading/early boot stage failed.

Stock restoration finished at 12:08:49.946253 with exact complete readback,
all four surrounding regions unchanged and zero misc before/after. Android and
host services were verified by 12:09:13.749999. All ten copied raw files were
independently verified on the desktop. Evidence: `captures/probe-serial-laptop-v2-20260915/`
and `captures/diagnostic-restore-laptop-v2-20260915/`.

`Test-CapturedAblDtbFixup.py` now executes UpdateDeviceTree (0x187a0) and its
libfdt edits. Stock and diagnostic trees return success with synthetic RAM-bank
and splash fixtures, optional protocols unavailable, and a test command line.
It verifies the resulting memory, bootargs and initrd properties. The candidate's
missing splash node is nonfatal in this exact code (0x19074 -> 0x191d4).
The initial harness required an additional allocation/BootDeviceBaseAddr-variable fixture for
stock; both now pass. This still does not emulate all BootLinux or live UEFI data.

For a stock-kernel control, the original embedded IKCONFIG was extracted. Stock
has neither DEVTMPFS nor CONFIG_USB_CONFIGFS_ACM/SERIAL. Our mainline diagnostic
PID1 therefore cannot be reused unchanged with stock Linux: it requires both.
Do not build that combination and mistake its expected logging failure for a
loader failure. Instead, `firmware/extracted/stock-control-20260915/` prepares
an exact stock recovery body (kernel, ramdisk, embedded overlay, original UI),
with only its AVB tail/footer zeroed. Its control hash is
`e5ad6cad1bc15b2e8f9943088649b82a52691fc5de4fd6d53638e36791f18629`.
This is an offline control, not a fix or a live test yet. The captured vbmeta
fixture predicts that its footer is unnecessary; a physical result can test
that prediction independently of a new kernel or incompatible RAM diagnostic.


Stock control scripts and payload are staged on the laptop. Header and libavb
fixtures accept the control. Host preflight checks exact stock-body equality,
zero tail, both payload hashes, fixed program geometry/imports, and rejects
cross-mode/old diagnostic payloads. `Run-A6LStockControl.ps1 -DiagnosticInstall`
was opened interactively (Windows PID 24144), but at 12:23 UTC its session was
still waiting for sudo password; no EDL worker had started and the phone remained
in the verified Android baseline. A user-input request for that password step
is pending. Do not launch a duplicate: inspect `capture-stock-control-install-v1/session.json`
and its `edl/report.json` first. The same window can proceed when the user enters
the password. The control's physical boot and restoration remain outstanding.

Use `Run-A6LStockControl.ps1 -ProbeCapture` only AFTER verified install/poweroff.
This uses a sysfs-only observer, `Collect-StockControl-v1.py`; no ACM gadget is
expected, and recovery UI execution must be supplied by the user's observation.
Then cold Volume Up + USB -> explicitly select Recovery mode. If the familiar
two-option recovery screen appears, select normal system startup and reconnect.
The observer exits when the exact spare's Android USB identity returns, and its
coordinator verifies Android and restores services. If fastboot returns instead,
START is the tested normal exit. Avoid starting capture before user readiness.

The prepared stock-control RESTORE coordinator now uses a separate restore-only
worker (`Write-LaptopStockControlRestore-v1.py`) which additionally reads exactly
65,536 bytes of live vbmeta at offset 291,811,328 before and after restoring
recovery. The unchanged recovery program geometry is still the only allowed
write. This will compare fresh vbmeta with the backup rather than assuming they
are identical. Its preflight passed. `stock-control-restore-tools-v1.json` is the
current hash manifest for the restore coordinator/worker/preflight; it supersedes
the restore coordinator entry in the earlier stock-control-tools-v1 manifest.
No stock-control install or boot success has yet been recorded.


## Stock control installed; physical result pending

At 12:29:43.884529 UTC the stock-body/no-footer control was installed with one
fixed 64 MiB write and full independent readback. Both GPT regions, devinfo and
misc remained unchanged; misc was zero. Poweroff ACK and USB disappearance
passed, with services restored by 12:29:49.107849. All ten copied before/after
files were verified independently on the desktop under
`captures/stock-control-install-laptop-v1-20260915/`.

The sysfs-only observer started at 12:31:52.525341 UTC. The user was asked to
use cold Volume Up + USB and explicitly select Recovery mode, leaving the
resulting screen displayed for observation. Recovery currently contains this
stock-body control, not the original complete stock partition. No new-kernel
image is installed. Restore only after observing the result and returning to
Android; the restore worker also captures fresh vbmeta as documented above.
