# Direct Linux USB checkpoint — 2026-09-15

The Linux laptop is reachable over SSH. Pierre explicitly authorized persistent
login access after automatic approval review initially rejected adding a key
without that specific authorization. A dedicated Ed25519 public key was then
installed successfully. It is restricted to the desktop's current local source
address, with agent, TCP and X11 forwarding disabled. Existing authorized keys
were retained. The private key remains in the desktop's ignored `logs/` area,
readable by Pierre, SYSTEM and Administrators; the sandbox account's grant was
removed. The password was entered in a local terminal and was not recorded.

## Verified preparation

The laptop identifies itself as Ubuntu 22.04.1 LTS, x86-64, running kernel
`6.8.0-87-generic`. Ubuntu's installed ADB and fastboot package version is
`1:10.0.0+r36-9`; the clients identify themselves as `28.0.2-debian`.
`dpkg --audit` returns no unfinished-package report and SSH is active.

After Pierre accepted the laptop's USB debugging prompt, the exact spare serial
`1e529013` returned the captured fingerprint, boot complete=1, flash.locked=1
and verifiedbootstate=green. Its Android USB composition on this laptop is
`109b:9130`; `109b:911f` was also observed after returning to Android on the laptop. The guarded runner now
accepts either observed composition while retaining the exact serial and
firmware-state checks. Four preflight fault-injection checks pass with the
newly observed composition in the fixture.

Evidence is in `captures/laptop-host-20260915/`: initial `report.json`,
`report-authorized.json`, and `kit-verification.json`. The laptop workspace
contains the diagnostic kit, versioned follow-up helpers, the separately verified
Google fastboot client, and capture outputs.
The transferred archive was checked against SHA-256
`5d09ee1033d65154c4a9500cb8e8ecacb410c3cf85d5d1c43929aaa07d68e1a7`.

## Physical comparison

The first three direct USB comparisons failed; the fourth succeeded with a
temporary NO_LPM quirk. Each used fixed read-only bootloader queries
and standard reboot, without image download, unlock, erase, or partition write.
The phone returned to the captured Android fingerprint, completed boot, locked=1
and green after Pierre used its buttons. These properties are a runtime baseline,
not an independent readback of every partition or proof of factory provenance.

| Case / UTC | Client | fwupd | Result |
| --- | --- | --- | --- |
| native / 08:15:32 | Ubuntu 28.0.2 | Active | A product reply was captured, then an unsolicited `getvar:version` failed with USB -108. Our client subsequently waited for the missing device. |
| quiet / 08:24:10 | Ubuntu 28.0.2 | Temporarily masked and stopped | First two product OUT requests failed with USB -71; the device disconnected. |
| google / 08:37:11 | Google 37.0.1 | Temporarily masked and stopped | Enumeration completed; the first product OUT request failed with USB -108 and the device disconnected. |
| no-lpm / 08:51:48 | Google 37.0.1 | Temporarily masked and stopped | With `18d1:d00d:k`, all eight queries and the first reboot succeeded. Android return was automatic and verified. |

All tests used the spare on laptop physical USB path `3-2`, on an AMD xHCI
controller, at USB high speed (480 Mb/s). Direct Linux therefore did not by itself
solve the transport problem. Changing from the older Ubuntu client to the current
Google client did not solve it either.

The first unsolicited query matches the probe order in upstream fwupd 1.8.0
(`product`, then `version`), and the laptop journal records fwupd losing this
device at that moment. This supports attribution to fwupd, without proving the
exact Pop/System76 package implementation from an upstream source snapshot.
The second test shows fwupd is not the sole cause. All temporary service pauses
were restored and independently checked active after testing.

Evidence directories:

- `captures/fastboot-laptop-native-20260915/`
- `captures/fastboot-laptop-quiet-20260915/`
- `captures/fastboot-laptop-google-20260915/`, including complete USB setup and
  both service-restoration reports
- `captures/fastboot-laptop-no-lpm-20260915/`, including complete USB setup and
  quirk/service restoration
- `captures/laptop-host-20260915/verify-after-{native,quiet,google,no-lpm}.json`

The Google client archive came from the official Android download endpoint.
Its archive SHA-256 is
`d230f13842f60f782a8645f9c813f8f845bf36089ea7289f28c48f17979313f1`;
the executable reports `37.0.1-15733141` and has SHA-256
`a686e2c7e8dc9cf4cba0cb8a2eef05f7b2bd682c925abd032fe203215d80b618`.
The installed Ubuntu ADB client was retained.

## Shutdown prompt identified in the actual backup

Pierre reported that the screen displayed `Press any key to shutdown`, including
on a previous test. A volume key later shut it down. This adds information to the
earlier descriptions of a black screen; it does not establish which earlier test
first reached that message or explain the original USB failure.

The exact message is absent from the extracted ABL LinuxLoader but is present in
the captured XBL's decompressed **QcomBds** module. `tools/Inspect-XblShutdown.py`
checks the XBL partition against the authoritative backup and hash-pins the PE
before analyzing it. Its private output is
`firmware/extracted/stock-xbl-20260915/shutdown-report.json`.

- XBL SHA-256: `e964e8da8f21784893c92c8597b2237bfc8148afb3172fe38291e8577f53a11e`.
- QcomBds SHA-256: `d5cb83a194d55f93a8abacc42d058e4a1f990d5472bbe4786813e29a60d8e04a`.
- UTF-16 message RVA `0x11564`; reference at `0xb878..0xb880`.
- Function `0xb86c` prints the prompt, calls a key-input helper, and on success
  prints `Key detected, shutting down` before a runtime-service call with reset
  type 2 (UEFI shutdown).
- A call at `0xc15c` follows the default boot-application launch path; another
  caller reaches the same routine through `0xb8cc`. Thus the prompt is a boot
  manager fallback, not a unique error code that identifies the failing USB step.

The observed return to Android after a short volume keypress and then Power was
verified over ADB. No forced long Power hold was necessary for this prompt.

## Full USB setup and the next discriminating test

The Google setup capture retained 133 records, with no recorder errors. It excludes
the laptop's other device on bus 3. New phone addresses were accepted only after
cached sysfs identified the exact serial on physical port `3-2`.

At bootloader address 12, device/configuration/string/BOS reads and SET_CONFIGURATION
completed successfully. The configuration describes one fastboot interface
`ff/42/03` with bulk endpoints `0x81` and `0x01`, maximum packet size 512.
The device reports USB 2.10 and its USB 2.0 BOS extension advertises LPM support
(`bmAttributes=0x00000006`). There are no unexpected fastboot bulk commands in
this capture. The first query was submitted about 489 ms after configuration,
and completed with -108 about 0.7 ms later, without a response.

This does not prove that sending the query itself caused shutdown: host USB
request traces do not capture every physical link-management event. The advertised
LPM capability motivates a narrowly scoped comparison using Linux's documented
`USB_QUIRK_NO_LPM`, selected by `18d1:d00d:k`. The flag is checked as bit 10 in
the newly enumerated phone's cached sysfs before any query. The wrapper preserves
all existing runtime quirk entries, adds only this bootloader identity, restores
the previous value, and refuses to overwrite a concurrent external change.
No modprobe configuration, initramfs, or persistent system setting is changed.

The no-LPM comparison succeeded on 2026-09-15 at 08:51:48 UTC. Cached sysfs
confirmed `quirks=0x400` on the exact bootloader before the queries. All three
consecutive product queries returned `sdm660`, then `unlocked`, `secure`, maximum
download size and both partition sizes succeeded. The first standard reboot
returned OKAY and Android was verified automatically by 08:52:13 UTC. The phone
remains locked; boot/recovery are 64 MiB and maximum download is 512 MiB.

The targeted query trace contains nine request/reply exchanges (eight getvars
and reboot), all with successful USB completions and OKAY replies. There is no
alternating unknown-command error in this run. The full setup trace contains
920 records and no recorder errors. At 08:52:14 UTC both the original empty
runtime quirk setting and the previously active fwupd service were restored.
A separate ADB baseline check also passed after cleanup.

This is a working query/reboot workaround under the tested conditions: direct
laptop USB, the official Google client, fwupd paused, and NO_LPM applied before
enumeration. It strongly implicates link power management in the laptop failure,
but does not identify the exact failing instruction in the phone's USB firmware,
prove that fwupd can remain active, or validate long transfers or flashing.
The earlier WSL alternating-response mechanism is not independently explained.

## Follow-up: RAM transfer and vendor-client preflight

The same host configuration subsequently passed a 1 MiB RAM transfer and the
complete 64 MiB recovery diagnostic transfer (1.803 seconds). USB byte counts and
acknowledgments were verified; no device-side RAM readback is available. Subsequent
queries and reboot passed and the original locked/green Android returned. The
image was not executed or installed. Host settings were restored.

A source-built client with a guarded vendor-command branch then passed a physical
read-only preflight, again returning automatically to verified stock Android and
restoring the host. The vendor command remained disabled and was not sent.
See [the unlock checkpoint](unlock-checkpoint-20260915.md) for both captures, the
exact ABL/XBL audit and the persistent operation. Pierre subsequently explicitly
approved all its security effects and possible data wipe. Both commands succeeded;
the reboot reached a decryption-error page requesting a reset. Host settings were
restored. A separate post-reset check then verified the original Android with
boot complete=1, flash.locked=0 and verifiedbootstate=orange. The successful
check and current baseline are recorded in that checkpoint.
Standard lock/unlock is blocked in this captured firmware; vendor unlock changes
both flags and clears the custom key when persisted, with a possible userdata reset.

Before writing the candidate, complete the executable recovery restoration and
independent readback procedure. The stock recovery image is now also hash-verified
on the laptop. A successful RAM transfer does not validate flashing or restoration.

Do not infer that the 58-partition eMMC backup can restore secure state in RPMB.
That coverage limitation matters for the vendor unlock path; the actual storage
branch selected on this spare remains unobserved, and full reversal is not proven.

Do not use an Internet recipe claiming `18d1:d00d:ik` skips BOS on Linux 6.8:
the upstream code defines `i` as DEVICE_QUALIFIER and `k` as NO_LPM. BOS queries
in our trace already complete successfully. Ordinary two-second autosuspend and
hardware link power management are also different mechanisms.

Primary references:

- [Linux 6.8 runtime quirk definitions](https://github.com/torvalds/linux/blob/v6.8/drivers/usb/core/quirks.c)
- [Linux 6.8 USB LPM capability handling](https://github.com/torvalds/linux/blob/v6.8/drivers/usb/core/hub.c)
- [Linux 6.8 quirk bits](https://github.com/torvalds/linux/blob/v6.8/include/linux/usb/quirks.h)
- [fwupd 1.8.0 fastboot probe](https://github.com/fwupd/fwupd/blob/1.8.0/plugins/fastboot/fu-fastboot-device.c)
- [UEFI runtime service definitions](https://github.com/tianocore/edk2/blob/edk2-stable202408/MdePkg/Include/Uefi/UefiSpec.h)
