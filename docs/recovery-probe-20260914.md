# A6L recovery diagnostic package — 2026-09-14

**Superseded candidate:** the original package below fails the captured Hisense
ufdt implementation despite passing newer host overlay validators. The corrected
package and its successful captured-code regression checks are in
`firmware/extracted/recovery-probe-v2-20260915/`. Its kernel and ramdisk are
unchanged; DT symbols, overlay fixups and root metadata handling are corrected.
Use `Package-RecoveryProbe-v2.py` for this format. See the latest physical state
in [the recovery-access checkpoint](recovery-access-20260915.md); historical
install/restore scripts and capture directories must not be reused.

The minimal kernel and static diagnostic RAM filesystem are packaged as an
Android recovery image. All 11 offline checks passed. The candidate has since
passed a RAM-only transfer test. On 2026-09-15 at 11:25:48 UTC it was installed
in recovery and independently read back in full. The first physical attempt
reached fastboot without diagnostic USB logs. Normal Android return and a full
stock recovery restoration were verified afterward, by 11:31:38 UTC.
The original offline report's `ready_to_flash: false` is historical; the separate
`installation-checkpoint-20260915.json` records the later validated prerequisites.
The second trial explicitly selected Recovery mode and again reached fastboot
without diagnostic USB output. Stock restoration and Android return passed by
12:09:13 UTC. No custom-kernel execution is established; current recovery is stock.
Linux EDL read access was verified on 2026-09-15: the complete stock recovery
matches the saved restore image. At 11:01:09 UTC the exact stock image was
rewritten and independently read back in full. Android and stock recovery both
started successfully afterward. The diagnostic installation subsequently passed.
The stock two-option entry screen belongs to the recovery ramdisk itself, and
a forced Power restart returns to it. It cannot be relied on after replacing
recovery. Cold physical fastboot entry, hardware recovery entry, and stock return
were subsequently verified. Forced restart from the new kernel remains untested.
See the
[recovery-access checkpoint](recovery-access-20260915.md).
This tests kernel startup and USB logging; it does not contain the LineageOS
system image, display support or an e-ink driver.

## Recovery-specific device tree

The captured stock recovery uses Android boot header v1 with an embedded DTBO
table. Inspection of the exact captured ABL found its non-A/B recovery path
selects that table when the header provides it. Instructions around PE RVA
0x23008..0x232e0 inspect recovery state, header version/size, DTBO offset and
table validity. Its overlay implementation also recognizes `target-path`.

`device/hisense/a6l/kernel/sdm660-hisense-a6l-recovery.dts` includes the audited
prototype tree and adds the same root `qcom,msm-id` and `qcom,board-id` values
as the captured stock base trees. It removes the base `/chosen/bootargs` so the
Android image header supplies one command line. Every hardware node is unchanged.

`a6l-recovery-overlay.dts` reproduces the stock embedded overlay's root selection
metadata, then adds only `/chosen/hisense,a6l-recovery-probe = "v1"` using a path
target. This recovery image carries its own overlay; it does not need a changed
normal `dtbo` partition or dummy nodes emulating the old USB symbol names.

The pinned AOSP libufdt test program and libfdt's `fdtoverlay` both apply the
overlay successfully. Their outputs differ in serialization but have identical
parsed properties. `/memory` lookup resolves the existing `/memory@80000000`.
These are host tests; actual ABL selection, memory fixups and peripheral behavior
remain untested.

## Artifacts and verification

Private output directory: `firmware/extracted/recovery-probe-20260914/`.

| Artifact | Size | SHA-256 |
| --- | ---: | --- |
| `recovery-diagnostic-unsigned.img` | 67,108,864 bytes | `ff3c54525d12a7f5d479fdc36cfd6ab0e6e103abcb9e95d398bac9ca58a27a7b` |
| `restore-stock-recovery.img` | 67,108,864 bytes | `9688e3dfb349186fb60efc057a618aeaf696b030ad52974932ea83a077b31621` |

The candidate uses the previously emulator-tested header-adapted kernel and
unchanged static diagnostic ramdisk. A gzip kernel is followed immediately by
the new DTB. The image uses header v1, 4096-byte pages, a 495-byte embedded DTBO
table and the captured header address fields. ABL's measured fixed load offsets
still govern actual placement; changing header addresses alone would not fix it.

The body occupies 11,976,704 bytes. Zero padding makes the candidate exactly the
recovery partition size and deliberately contains no copied AVB footer. It is
unsigned; the captured OS version/patch metadata is retained as packaging data,
not a claim that this diagnostic runs Android 9 or has a valid stock signature.
Round-trip comparison normalizes the unused second-stage address field that the
packing tool zeroes when there is no second-stage payload.

`report.json` records these 11 checks:

1. Audited hardware properties unchanged.
2. Selection metadata matches captured stock metadata.
3. libfdt and libufdt merged trees agree semantically.
4. Overlay changes only the diagnostic marker.
5. Bootloader-style `/memory` lookup works.
6. Gzip kernel and immediately appended DTB parse correctly.
7. Runtime kernel region fits before the relative DTB destination.
8. Unpacked recovery payloads match their inputs.
9. Normalized unpack/repack body is byte-identical.
10. Candidate exactly fits the captured recovery partition.
11. Complete stock restore image matches the independently checked backup hash.

Build entry points are `tools/build-linux-recovery-dtb.sh` and
`tools/build-host-overlay-validator.sh`. Packaging is `tools/Package-RecoveryProbe.py`;
restore preparation is `tools/Prepare-RecoveryRestore.py`. These do not access
the phone. Both DTS files passed kernel checkpatch with no errors or warnings.
Shell syntax and Python compilation checks passed for the relevant helpers.

## Physical check and recovery plan

First validate entry into the existing recovery and return to Android while the
spare is within reach. `tools/Inspect-StockRecovery.py enter <capture-directory>`
guards serial, exact captured fingerprint, boot-complete and locked/green state
before requesting `adb reboot recovery`. Its observations are bounded. This
does not install an image. The matching `verify-return` action records the same
guards after Android returns.

The verified stock recovery ramdisk has `ro.debuggable=0`, `ro.secure=1` and
`ro.adb.secure=1`; its adbd service is disabled unless its property triggers start
it. Absence of ADB in recovery is therefore expected, not proof of a failed boot.
The saved no-SD-card menu graphic labels **重新启动** as Reboot and
**擦除用户数据** as Erase user data. These are reference graphics extracted from
the backup, not an observation of the phone screen. Confirm the actual UI with
the user and select only Reboot. If the menu differs, inspect its labels first.

The physical check subsequently reached a preceding selection screen. The user
transcribed **进入 Recovery 模式** (Enter Recovery mode) and **正常启动系统**
(Boot the system normally). After entering recovery, the user confirmed that
**重新启动** (Reboot) was available. ADB enumerated the spare as unauthorized
during this check, so recovery shell access was not established. The user was
instructed to select Reboot. The subsequent ADB check passed: exact original
fingerprint, `sys.boot_completed=1`, `ro.boot.flash.locked=1`, and verified boot
state `green`. Evidence is `captures/stock-recovery-20260914/verify-return.json`.
Physical entry and return are now confirmed; restoration after replacing the
partition remains untested.

Once stock recovery entry/return is established, resolve reliable fastboot
queries and the exact Hisense unlock behavior. An unlock may erase userdata;
the current backup intentionally excludes it. A concrete installation procedure
must address signature/AVB policy and how to enter and exit the diagnostic when
there is no display or working USB. The diagnostic has no automatic normal-boot
fallback, and a reboot alone must not be assumed to clear a recovery boot request.

`restore-recovery-only.xml` describes exactly one full partition on eMMC physical
partition 0: sector size 512, start sector 917504, count 131072. Before any use,
recheck this spare's identity and GPT against the backup and use only the verified
A6L eMMC programmer. After a restore, independently read back all 64 MiB, compare
the stock hash, then verify Android and stock recovery both return. The XML is
a prepared description, not an executed or proven restoration procedure.

The firmware backup excludes userdata, eMMC hardware boot areas, RPMB, fuses and
the separate e-ink SPI region. It preserves the working captured state, whose
boot image contains Magisk metadata and whose vbmeta has flags=2; it is not an
authenticated factory release. The later approved vendor unlock and custom-key
persistence operation is recorded in the linked unlock checkpoint. No diagnostic
or other image has been flashed.

`tools/Collect-ProbeSerial.py` is prepared for the diagnostic's exact USB identity
(VID 1d6b, PID 0104, serial HLTE730T-PROBE). It performs a bounded serial read,
saves the console and detects ready/heartbeat/panic markers. It has not yet been
tested against a physical A6L diagnostic boot.

On 2026-09-15 the full 64 MiB candidate was successfully transferred into volatile
RAM using the direct laptop USB workaround. The trace verifies byte counts and
acknowledgments; it does not provide a device-side hash. The phone then returned
to the same locked/green Android state. No candidate execution or partition flash
has occurred. An exact stock recovery copy is also hash-verified on the laptop.
The [unlock checkpoint](unlock-checkpoint-20260915.md) records the next prepared
operation, its specific security effects and the approval boundary. Restoration
after replacement and diagnostic entry/exit remain untested.

## Source provenance

The authoritative device evidence is the hash-verified captured ABL/recovery.
The Qualcomm bootloader source retained in `references/qcom-abl-reference/` is
an explanatory reference, not Hisense's exact source:
[SHIFTPHONES edk2 at 7d391d8b1059ed48259728cab004ca6f16ee3de2](https://github.com/SHIFTPHONES/android_bootable_bootloader_edk2/tree/7d391d8b1059ed48259728cab004ca6f16ee3de2/QcomModulePkg/Library/BootLib).

The host overlay validator uses the locally pinned Android source revisions:
libufdt `60546233f51d5eaceb9ca8837d1192539ccea1a1` and
libfdt `c1197f56b8f468effbbf5e79feae353c4280e2ea`.
