# Stock bootloader inspection and kernel load-offset test

## Outcome

The Windows USB blocker is resolved. The spare was inspected through WSL
fastboot and returned to stock Android. No unlock, image download, erase or
partition flash was performed. The installed usbipd-win driver remains available,
but the spare's temporary sharing registration was removed.

Offline inspection of the captured ABL found no standard fastboot `boot` handler
in its registered command table. It also established a fixed 512 KiB kernel load
offset. A separate prototype Image with matching ARM64 header metadata booted
the diagnostic RAM filesystem in QEMU at the measured offset. These findings
narrow the next test route; they are not an A6L hardware boot result.

## USB and stock return

- Installed official usbipd-win 5.3.0, with installer digest and Authenticode
  checked. Installer exit 0. Automatic Windows reboot was suppressed.
- Restricted its firewall rule to the current WSL IPv4 address, and shared only
  USB\VID_18D1&PID_D00D\1E529013. No other USB device was bound.
- WSL fastboot 34.0.4 detected serial 1e529013 and returned product sdm660.
  Thirteen other getvar queries failed remotely. Process exit 0 misleadingly
  accompanied those failures; the collector now records protocol failure too.
- Fastboot reboot returned OKAY. ADB then confirmed boot complete, the original
  L1632.6.01.04 fingerprint, flash.locked=1 and green verified boot state.
- Removed the exact persisted USB binding by GUID and verified removal. The
  firewall restriction was verified during cleanup. If WSL's address changes,
  the rule must be updated before a future attachment.

Evidence: captures/fastboot-wsl-20260914/{report.json,stock-return.json},
logs/usbipd-setup-result.json and logs/usbipd-cleanup-result.json.

The administrator helpers require Windows UAC. Windows PowerShell 5.1 initially
blocked local scripts; launching the inspected helper with process-only
RemoteSigned resolved that. No persistent execution policy, driver signature
requirement or trusted certificate store was changed. An initial firewall
attempt rejected IPv6 loopback; the successful configuration permits only WSL's
IPv4 address. tools/Install-A6LUsbForwarding.ps1 supports resuming after installation.

## Verified offline ABL extraction

tools/Inspect-StockAbl.py extracts only the authoritative backup's `abl` partition,
at offset 251658240, length 1048576, and verifies its manifest SHA-256:

553e8e70315d15e4c5bcb459e456eb37f643f4a00ad25c90a257cc1ca751dd53.

The outer ELF contains a firmware volume at offset 0x3000. UEFI Firmware Parser
1.16 decompressed its guided section and extracted one application named
LinuxLoader. The inner PE is AArch64, 434176 bytes, with SHA-256:

6bbe53f345729c77c9a0ba74aa7f7aadb6cd68b0ac1511f6f160bc69de6ab523.

Private artifacts are under firmware/extracted/stock-abl-20260914/. The separate
WSL environment /home/a6l/tools/abl-analysis-venv holds the offline parsers.
tools/Inspect-AblPayload.py records PE sections, strings and the command table;
tools/Disassemble-Abl.py uses Capstone to inspect code as data. Firmware was
never executed on the host. Backup hash verification does not authenticate this
as an unmodified Hisense factory release; the earlier provenance qualification
still applies.

## Fastboot implementation

The instruction sequence at PE RVA 0x2f388..0x2f3ac registers 17 pointer pairs,
starting at RVA 0x4d5e8 with stride 16. The decoded table contains the familiar
flash, erase, unlock, getvar, download, reboot and continue handlers, plus the
Hisense command family. There is no `boot` prefix in that table.

The dispatch sequence at 0x2e514..0x2e548 walks the registered list and calls the
matching handler. If none matches, it sends the unknown-command response. The
`continue` handler at 0x32204 prepares a fresh boot-info structure and calls the
image-loading/verification path before BootLinux; it is not evidence that a
downloaded image can be executed. Do not substitute continue for fastboot boot.

This is stronger evidence than a missing diagnostic string, but it does not
prove that every possible alternate boot mechanism is absent. A RAM-only test
through standard fastboot boot should not be assumed available on this firmware.

The binary publishes max-download-size, secure, unlocked and the partition
variables that failed in the live query. Thus those failures do not establish
that the variables are unsupported. The alternating remote errors remain a
transport/parser question for the next read-only inspection. No custom fastboot
binary or Hisense unlock subcommand was executed.

### Follow-up packet inspection

Physical stock recovery entry and return were subsequently verified; see the
[recovery checkpoint](recovery-probe-20260914.md). A restricted PyUSB logger,
`tools/Inspect-FastbootPackets.py`, was prepared to compare repeated getvar
requests in one USB session. It accepts only a fixed query list and reboot,
records raw request/status bytes, uses bounded transfers and attempts a standard
fastboot reboot if its own reboot fails. Four offline command/parser tests passed.
Ubuntu's `python3-usb` 1.2.1-2 package was installed for this host diagnostic.

The physical attempt did not produce a successful query. USB descriptors matched
the exact spare, fastboot interface ff/42/03, bulk OUT 0x01 and IN 0x81, maximum
packet size 512. The first `getvar:product` write returned EIO without a confirmed
byte count; no status packet was received. USB then disappeared from WSL and
Windows. The raw reboot returned ENODEV and the standard fastboot fallback timed
out. The user reported the phone remained connected with a black screen.

The cause is unresolved. This does not establish that the query was accepted or
that the variable is unsupported, and it does not isolate a bootloader crash
from a USB transport failure. Do not repeat this raw-USB path without investigating
the disconnect. The earlier standard-client query/reboot result remains separate.

Only this spare's sharing registration was removed. After a Power-button forced
restart, ADB verified the original fingerprint, boot complete, flash.locked=1
and green verified boot state. No unlock, image download, flash or erase was
attempted. Evidence is `captures/fastboot-packets-20260914/`, particularly
`report.json`, `verify-return.json` and `boot-reason.json`; its preflight is in
`captures/fastboot-packets-20260914-entry/`. The earlier USB setup/cleanup results
were preserved under `logs/usbipd-prior-to-packet-check/`.

## Fixed address and tested adaptation

The loader code at 0x12fac..0x12fe8 reads its memory base and combines it with
three compiled constants using OR:

| Constant RVA | Value | Destination |
| --- | ---: | --- |
| 0x3e30c | 0x00080000 | 64-bit kernel |
| 0x3e310 | 0x03200000 | device tree |
| 0x3e314 | 0x03400000 | RAM filesystem |

With an assumed 0x80000000 DRAM base, these would imply 0x80080000,
0x83200000 and 0x83400000. The saved DTB's memory reg is a zero placeholder;
the bootloader supplies the live value, which was not readable through stock
ADB. These absolute addresses therefore remain conditional, not runtime memory
measurements on the phone. Simply changing Android boot-header address fields
does not establish a different kernel load location.

The prototype originally advertises text_offset=0. Its pinned kernel source
arch/arm64/kernel/pi/map_kernel.c handles the physical placement remainder in
early_map_kernel(), and the compiled configuration has CONFIG_RELOCATABLE=y.
This permits testing a header-only adaptation rather than assuming that a
relocation stub or rebuilt kernel is required.

tools/Test-AblLoadOffset.py changes only the ARM64 header's text_offset in a
separate Image to 0x80000. Only byte offset 10 differs from the preserved build.
All instructions, image_size and remaining metadata are unchanged. Its runtime
region ends before the stock device-tree destination.

The test starts diskless QEMU paused, uses local QMP to read all 64 header bytes
at physical address 0x40080000, and compares them with the adapted Image before
running it. That confirms placement 512 KiB above QEMU's 0x40000000 RAM base.
The same diagnostic PID 1 reaches READY and emits heartbeats; no kernel panic
occurs before the intentional 35-second timeout. All eight checks passed.

Adapted Image SHA-256:
bce9e6d3b330bb4a77cbd30b5bff71f1c8b5ba94f71417b8e0a495425079fb43.

Artifacts: firmware/extracted/abl-offset-test-20260914/, including report.json,
the measured physical header, console log, separate Image and gzip copy. The
original kernel Image remains unchanged. This test establishes CPU/kernel
startup at the required relative offset in QEMU; it does not emulate ABL,
Qualcomm memory fixups, A6L peripherals, display, or the stock DTBO application.

## Remaining hardware-test work

The adapted Image has now been packaged into a separate unsigned
[recovery diagnostic candidate](recovery-probe-20260914.md). Recovery's embedded
DTBO allows a minimal overlay without the stock USB symbol contract. Selection
metadata, overlay application, image layout and round-trip checks passed offline;
an exact stock restore image and one-partition description are prepared. Physical
stock recovery entry and return are now verified. The subsequent
[transport investigation](fastboot-transport-20260914.md) captured intermittent
query rejection; direct USB on a Linux laptop is the next comparison. Bootloader
unlock changes and their user-data wipe require a concrete reviewed procedure.
The separate e-ink SPI region remains outside the verified eMMC backup.

## References

- [UEFI Firmware Parser](https://github.com/theopolis/uefi-firmware-parser)
- [Pinned kernel source](https://github.com/sdm660-mainline/linux/tree/e47d622cb6d2440a9eacdc8bb2df32c037bec7b8),
  especially Documentation/arch/arm64/booting.rst and arch/arm64/kernel/pi/map_kernel.c.
- [Microsoft WSL USB documentation](https://learn.microsoft.com/en-us/windows/wsl/connect-usb)
- [Official usbipd-win 5.3.0 release](https://github.com/dorssel/usbipd-win/releases/tag/v5.3.0)
