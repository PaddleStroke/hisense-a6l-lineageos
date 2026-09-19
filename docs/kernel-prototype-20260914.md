# A6L kernel prototype and RAM diagnostic environment

## Result

Built Linux 7.2.3-a6l-probe+ from sdm660-mainline/linux revision
e47d622cb6d2440a9eacdc8bb2df32c037bec7b8, plus a new minimal A6L device tree.
The same kernel booted the new static diagnostic PID 1 on QEMU's ARM64 virt
machine. It reached its ready marker, reported virtual filesystem mounts only,
and produced heartbeats through 40 seconds. The test ended at its intentional
45-second timeout without a kernel panic. No disk or host directory was exposed
to the guest. This is not an A6L hardware boot or an Android boot.

Source checkout: /home/a6l/kernel/a6l-mainline, local branch a6l-bringup.
Build output: /home/a6l/kernel/out-a6l-probe.
The branch is local; no public kernel fork or upstream submission was made.

The newer kernel is an experimental hardware bring-up route, not a drop-in
replacement for the stock kernel. Its upstream DRM/Adreno interfaces differ
from the stock KGSL/MDSS interfaces. A successful basic boot would still leave
display drivers, graphics userspace and the Hisense e-ink path to implement.
Matching Hisense source remains useful; no matching A6L tree was found.

## Measured board wiring

Android reported ro.boot.dtbo_idx=0. The verified dtbo partition contains one
757-byte overlay, extracted with tools/Inspect-StockDtbo.py. Applying it to
both saved base trees succeeded. It changes the first tree's USB configuration
to high-speed with the SuperSpeed PHY disabled; the second tree already has
those settings. The base-tree index remains unknown, but the selected controller
addresses, supply mappings and fixed reserved regions agree in both variants.

The new device/hisense/a6l/kernel/sdm660-hisense-a6l-probe.dts enables:

- The measured UART at 0x0c170000.
- The eMMC controller at 0x0c0c4000, eight-bit bus, initially capped at 50 MHz.
  Supplies resolve to PM660L L4 at 2.95 V and PM660 L8 at 1.8 V.
- USB2 peripheral mode using QUSB2 PHY0 and PM660L L1/L7 plus PM660 L10.
- The inherited SoC infrastructure needed by these controllers.

It preserves the stock fixed reserved-memory regions, including Hisense boot
and diagnostic buffers. The inherited TZ-region end is adjusted to match the
stock removed-region extent. Additional inherited reservations remain reserved;
this audit does not establish their future use. No ramoops buffer was invented.

The LCD/MDSS, GPU, modem, ADSP, CDSP and charger are disabled in the compiled
tree. No A6L SPI/e-ink driver is added. The temporary boot arguments preserve
unused clocks, power domains and regulators during bring-up; these are not
production power-management settings. Regulator parent wiring, PHY tuning and
actual sequencing still require validation.

tools/Audit-KernelProbe.py passed: both stock variants match the selected
addresses, interrupt IDs, five supply identities/voltage constraints, eMMC bus
width, USB speed and fixed-region coverage. Reserved regions do not overlap.
Required USB, storage, BPF, Binder and core SoC drivers are built in. The kernel
uses 4 KiB pages and allows enough eMMC partition minors for the captured GPT.
QSEECom, RMTFS and inline MMC encryption are excluded from this probe.
The board source passed checkpatch with zero errors and warnings. This is not
a complete dt-schema validation or a claim of upstream readiness.

## Reproducible components and private artifacts

- tools/build-linux-kernel-probe.sh builds the kernel and DTB separately from
  Android, with a source-revision guard, build lock, configuration checks and
  timestamped logs. It uses the existing Android Clang r584948 toolchain.
- device/hisense/a6l/Android.bp defines the explicit a6l_probe_init target;
  it is not added to PRODUCT_PACKAGES. tools/build-linux-diagnostic.sh builds
  only this component. The build completed in 4m23s, including graph regeneration.
- device/hisense/a6l/diagnostic/init.c is a static, PID-1-only diagnostic process.
  It mounts devtmpfs, proc, sysfs and configfs; reads kernel diagnostics; and
  configures a USB ACM gadget if a controller appears. It has no shell, unlock
  or flashing functions and never opens a block device or mounts persistent data.
- tools/Test-DiagnosticRamdisk.py verifies the static ELF, makes a newc CPIO
  with the kernel's gen_init_cpio, checks its listing, and runs diskless QEMU.

Saved kernel artifacts: firmware/extracted/kernel-probe-20260914/.

| Artifact | Bytes | SHA-256 |
| --- | ---: | --- |
| Image | 28,510,720 | 57120be5a70114bf476f26a8f39ac6af0660d6240d6017379d160d258f76bf92 |
| Image.gz | 11,785,829 | a634c51fb27536d976c78621e069c28d6d722aa6210219e06706969273d0a8b0 |
| sdm660-hisense-a6l-probe.dtb | 52,613 | 1228d8bc1b7ae2ee477ab0f6d25fce133f440156ce33fad8af10f53a614c4a2c |

Saved init, RAM filesystem, archive listing and emulator logs:
firmware/extracted/diagnostic-ramdisk-20260914/.

- init SHA-256: a84cdd7bf4876405350ade3d68ff848e3db0dbe70083972638bfb4df2c2f39fa.
- ramdisk.cpio.gz SHA-256: 14141431a5e57a4510159365eddddc5322aceb271272787a5282ee27f2a9d983.

The physical USB gadget path was not tested by QEMU virt, which has no A6L USB
controller. No claim about phone USB enumeration follows from the ready marker.

## Bootloader connection checkpoint

Before the query, the spare reported boot complete, locked/green, OEM unlocking
allowed and battery 100%. tools/Inspect-Fastboot.py verified the stock fingerprint
and issued only adb reboot bootloader. Windows then enumerated the exact spare
as USB\\VID_18D1&PID_D00D\\1E529013, problem code 28 (no driver). Standard Windows
fastboot could not see it, so no getvar, unlock, download, erase or flash command
reached the phone. The automatic reboot-to-Android path could not run.

The official Google USB driver was downloaded and its catalogs validated, but
its unmodified INF does not match D00D. The supplied older universal package
also lacked the match and its catalog did not validate cleanly. Neither was
installed; no certificate was imported and no driver signature checks disabled.

Prepared the official usbipd-win 5.3.0 x64 installer, verified against its GitHub
release digest and a valid Authenticode signature from Frans van Dorsselaer.
Installer SHA-256:
1c984914aec944de19b64eff232421439629699f8138e3ddc29301175bc6d938.

After the user approved reopening Windows UAC, the official installer completed
with exit 0. The first helper launches were blocked by Windows PowerShell 5.1's
local script policy; a process-only RemoteSigned setting allowed the inspected
local helper to run. No machine execution policy or driver signature setting was
changed. The first firewall restriction attempt rejected IPv6 loopback, so the
helper was corrected to permit only the current WSL IPv4 address. A second run
recognized the installed version, applied that restriction and bound only the
exact spare at bus 4-1. Setup result: logs/usbipd-setup-result.json.

WSL attachment succeeded. Ubuntu fastboot 34.0.4 recognized serial 1e529013;
tools/Inspect-FastbootLinux.py sent only getvar queries and reboot. The product
query returned sdm660. The other 13 queries failed remotely, alternating between
unknown command and GetVar Variable Not found. The client returned process exit
0 even for these failed queries; this does not mean they succeeded. No result
establishes unlock state, slot layout, maximum download size or temporary boot
support. The alternating errors warrant inspecting the Hisense protocol before
assuming these capabilities are absent.

Fastboot reboot returned OKAY. ADB subsequently confirmed sys.boot_completed=1,
the exact pre-test stock fingerprint, ro.boot.flash.locked=1 and green verified
boot state. Captures: captures/fastboot-wsl-20260914/report.json and
stock-return.json. No unlock, image download, flash or erase was performed.

tools/Close-A6LUsbForwarding.ps1 removed the exact persisted bootloader binding
by its GUID and verified removal. It also verified the service firewall rule
remains limited to the current WSL IPv4 address. The driver remains installed
for later development, with no phone shared. Cleanup result:
logs/usbipd-cleanup-result.json. If WSL's address changes, update the restriction
before another attachment. Both administrator helpers can be launched with
process-only RemoteSigned, and require explicit UAC acceptance.

## Before a phone kernel test

Follow-up: [offline ABL inspection and load-offset test](bootloader-inspection-20260914.md)
found no standard fastboot boot registration and established the fixed kernel
offset. A separate header-adapted Image passed QEMU at that measured relative
offset. The original artifacts above remain unchanged.

Follow-up: the kernel and RAM filesystem are now packaged as an unsigned
[recovery diagnostic candidate](recovery-probe-20260914.md). Both overlay
implementations agree and the image passed its offline round-trip check.
The following still needs physical validation before a phone kernel test:

1. Select a recoverable hardware-test route. USB fastboot and return to stock
   now work, but the stock ABL command table lacks standard temporary boot.
2. Qualcomm DTB selection and overlay application on the actual bootloader.
   The recovery wrapper matches stock selection metadata and supplies its own
   minimal embedded overlay; offline application passes without legacy symbols.
3. Bootloader memory/initramfs fixups and hardware validation. The stock
   ARM64 Image header has text_offset=0x80000; the new kernel has text_offset=0.
   The ARM64 boot protocol requires placement relative to a 2 MiB aligned base.
   A header adaptation passed the focused emulator test; physical ABL loading
   remains to be validated. The complete image layout passed offline checks.
4. A concrete installation/rollback procedure before any unlock or partition
   changes. The separate e-ink SPI region remains outside the eMMC backup.

## Primary references

- https://github.com/sdm660-mainline/linux/tree/e47d622cb6d2440a9eacdc8bb2df32c037bec7b8
- The pinned tree's Documentation/arch/arm64/booting.rst and USB/PMIC drivers.
- https://learn.microsoft.com/en-us/windows/wsl/connect-usb
- https://github.com/dorssel/usbipd-win/releases/tag/v5.3.0
- https://developer.android.com/studio/run/win-usb
