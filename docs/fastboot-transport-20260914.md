# A6L fastboot transport follow-up — 2026-09-14

The repeated-query failure is reproducible using Ubuntu's standard fastboot
34.0.4, including within one client process. The original firmware remains
installed and locked. Standard-client reboot returned the phone to Android
after every completed native-client check, although the final experiment needed
one retry after an explicit `unknown command` response. The original fingerprint,
boot-complete=1, flash.locked=1 and green state verified afterward.

## Experiments and evidence

| Capture | Result |
| --- | --- |
| `captures/fastboot-native-20260914/` | Three identical product queries returned OKAY, FAIL, OKAY; reboot passed. The first passive recorder exited on EAGAIN and captured no traffic. |
| `captures/fastboot-native-v2-20260914/` | No phone query: USB attachment refused because WSL had stopped. The collector subsequently failed its host USB preflight. |
| `captures/fastboot-native-v3-20260914/` | With WSL held open and the recorder corrected, the same OKAY/FAIL/OKAY sequence was captured; reboot passed. |
| `captures/fastboot-paced-20260914/` | A one-second interval still produced OKAY then FAIL for identical product queries; remaining queries were skipped and reboot passed. |
| `captures/fastboot-padded-20260914/` | A source-built client sent 64-byte zero-padded commands; the same OKAY/FAIL/OKAY sequence occurred. Standard reboot passed. |
| `captures/fastboot-all-20260914/` | One standard `getvar all` succeeded with 134 bootloader output lines. The following reboot returned unknown command; one standard reboot retry succeeded. Stock return was verified. |

The v3 usbmon trace has 16 lines and no recorder errors. It shows exactly three
14-byte OUT requests containing `getvar:product`; their completion statuses are
0 and the reported lengths are 14. The corresponding IN responses are
`OKAYsdm660`, `FAILunknown command`, `OKAYsdm660`. The six-byte `reboot` request
receives `OKAY`. Ubuntu's client requests up to 256 response bytes. There are no
extra zero-length command writes in that trace. Linux usbmon observes requests
at the host-controller boundary, not electrical traffic at the phone connector.

This rules out reopening the client as the sole cause and does not support a
simple one-second settling delay as a fix. It does not prove whether corruption
occurs in forwarding, the device USB implementation or the bootloader parser.

`tools/Inspect-FastbootNative.py` locates only USB 18d1:d00d with serial 1e529013
using cached sysfs attributes, runs a fixed query list and always attempts the
standard-client reboot. It records only that device's usbmon lines. A race where
the monitor appeared readable but returned EAGAIN was corrected to continue
waiting. Transfer, process and capture limits remain bounded.

The standard AOSP Linux transport obtains descriptors through usbfs/sysfs and
does not select alternate setting 0 again. The earlier PyUSB logger explicitly
requested serial/configuration information over control transfers. This is a
candidate difference to investigate, not an established cause of its disconnect.
Its active raw-USB path has not been repeated.

WSL shutdown was directly observed when usbipd refused attachment with
"The selected WSL distribution is not running." A bounded foreground Linux
process now keeps the VM alive throughout an inspection. Passive tracing uses
the WSL kernel's usbmon module and debugfs; it adds no device commands. Do not
attribute the earlier black screen solely to WSL shutdown without further evidence.

## Fixed-length query experiment

`tools/Prepare-FastbootQueryPadding.py` prepares a host-only change against
`system/core` revision `1375cc767964bbac2070bc41ebc3c864589846b4`, refusing unrelated
edits to the affected files. `tools/build-linux-fastboot-query.sh` builds fastboot
and its existing mock-transport tests. With `A6L_QUERY_PAD64=1`, RawCommand accepts
only a fixed getvar allowlist plus reboot and extends each command to 64 bytes
with zero padding. Downloads, unlocks, erases and partition writes are rejected
before the transport is called. Without that opt-in flag, normal behavior remains.

The stock ABL parser caps command length at 64 and writes a NUL terminator before
prefix matching (PE RVA 0x2e268..0x2e274). This justifies a bounded query experiment;
it does not establish that padding fixes the transport. Mock tests check exact
64-byte content, normal query behavior, reboot and rejection of mutation commands.
The phone harness requires the build/test success marker before selecting the
experimental query binary and uses Ubuntu's standard client for the return reboot.

All seven DriverTest cases passed. The physical experiment nevertheless failed
to fix the alternating replies. Its passive trace has 16 lines with no recorder
errors, and confirms 64-byte OUT transfers with visible zero padding. Text
usbmon records only the first 32 payload bytes; the mock tests separately verify
the full command contents. Padding is not a solution or a prerequisite for the
next laptop test. The host-only change remains local in WSL's system/core;
no Android image was rebuilt or installed as part of that experiment.

## Live bootloader inventory

The successful `getvar all` reports `unlocked:no`, `secure:yes`, product `sdm660`,
variant `SDM EMMC`, and maximum download size 536870912 bytes (512 MiB).
Boot and recovery are each 0x4000000 bytes (64 MiB), system is 0x180000000
bytes (6 GiB), dtbo is 0x800000 bytes, and vbmeta is 0x10000 bytes.
All 58 partition sizes in the verified backup manifest agree with this live
report; userdata is the 59th reported partition and is excluded from the backup.
Evidence is `captures/fastboot-all-20260914/backup-size-comparison.json`.
This agreement does not test restoration or establish reliable image transfers.

The first reboot after this inventory also returned `FAILunknown command`.
The retry and verified return are saved separately as `reboot-retry.json` and
`verify-return.json`, preserving the original failed attempt in `report.json`.
The collector now permits one retry of reboot only after this explicit remote
rejection. It does not retry ambiguous timeouts or add any write operation.

## Follow-up: direct USB on the Linux laptop

The [2026-09-15 laptop comparison](linux-laptop-20260915.md) removed Windows
usbipd and WSL from the USB path. Three ordinary-client trials disconnected;
the fourth, using Google fastboot 37.0.1 with fwupd paused and a bootloader-only
NO_LPM quirk, passed all eight queries and rebooted automatically to verified
Android. This provides a tested host workaround for queries and reboot. It does
not validate long image transfers or establish the mechanism of the earlier WSL
alternating replies.

The final desktop session ended in verified stock Android. Only the spare's
USB sharing registration was removed successfully, and the bounded foreground
WSL keeper was stopped. No kernel, recovery or system image was sent to the phone.

## Unlock mechanism: offline evidence only

The captured ABL's `Hisense` handler compares the suffix ` unlock` including its
terminator, then calls RVA 0x23ee8. That function sets bytes +0x0d and +0x0e in
the runtime device-info structure at RVA 0x60838 and performs no storage write.
This has not been executed on the phone.

The erase handler recognizes the name `avb_custom_key` as a special case and
calls RVA 0x245d0. That clears the public-key length and key buffer in the same
runtime structure, then passes the complete 0x998-byte structure to the
device-info protocol wrapper at RVA 0x1a1e8 with operation 1. The pinned Qualcomm-
derived reference implementation names the equivalent operation
`ReadWriteDeviceInfo(WRITE_CONFIG, ...)` inside `EraseUserKey`.

Together these instructions explain why the community procedure couples the
runtime unlock command with key erasure: the latter can persist the altered
device-info block. This is not a dry-run command or an ordinary standalone GPT
partition erase. Exact secure-storage behavior, wipe handling and recovery of
unlock state still need review before an installation procedure is approved.
No step of that unlock procedure has been performed during this project.

The later [2026-09-15 unlock checkpoint](unlock-checkpoint-20260915.md) completes
the exact XBL storage-provider audit and documents the blocked standard lock/unlock
handler. The actual protected-versus-plain storage branch remains unobserved.
It also records successful 1 MiB/64 MiB RAM transfers and a physical read-only
preflight of the guarded vendor client. Pierre later explicitly approved both
unlock flags, custom-key erasure and a possible userdata reset. The commands
succeeded and the reboot reached a decryption-error page requesting a reset.
Afterward the original Android returned with boot complete=1, flash.locked=0 and
verifiedbootstate=orange. The newer checkpoint records the separate verification.

Reference source is explanatory, not Hisense's exact source:
[DeviceInfo.c at the pinned SHIFTPHONES revision](https://github.com/SHIFTPHONES/android_bootable_bootloader_edk2/blob/7d391d8b1059ed48259728cab004ca6f16ee3de2/QcomModulePkg/Library/BootLib/DeviceInfo.c).
