# Initial LineageOS 24 development checkpoint

## Current result

Kernel follow-up: a separate Linux 7.2.3 A6L prototype and static diagnostic
RAM filesystem now build, pass offline checks, and boot in QEMU. See
kernel-prototype-20260914.md for scope, artifacts and pending bootloader access.

The systemimage build completed successfully on 2026-09-14 in 3h41m24s. Its
6,442,450,944-byte image is raw ext4 (not Android sparse format), matching the
6 GiB system partition capacity. SHA-256:
58b200d331c965bdfe58e3d50133549347625f8db56220865929c87371b43273.

tools/Inspect-BuiltSystem.py completed successfully. e2fsck -f -n passed all five
checks without repairs; the copied analysis image hash matches the source.
Image properties confirm Android 17, API 37.0/REL and ro.lineage.device=a6l.
The inherited system identity remains generic_system, as expected for this GSI.
VNDK 31–34 APEX packages are present under /system/system_ext/apex; VNDK 28 is
absent. /init points to /system/bin/init, whose bytes match the staged second-stage
binary and contain its missing-stage-argument fatal message. The image remains
a compile probe, not a boot-ready A6L ROM.

Private report: firmware/extracted/system-probe-inspection-20260914/report.json.
The complete image copy is retained there as system-expanded.img. Source and
build caches remain intact for incremental work.

The separate AOSP init_first_stage target completed successfully in 2m42s via
tools/build-linux-first-stage.sh. tools/Inspect-FirstStage.py verified its
3,321,920-byte static AArch64 executable: no interpreter or dynamic segment,
entry point inside executable code, valid load bounds and non-executable stack.
The preserved copy has SHA-256:
0a2cd37177179c8544d8e1b631c9faf90ca086fa745221640e30ea302f2578e2.
Private binary, build log, readelf output and report:
firmware/extracted/first-stage-init-20260914/.

Both build processes have exited. Their PID files are historical records.
The component build did not package or flash a boot image. Its kernel syscall
compatibility, early mounts and integration with the final root layout still
need validation. /home/a6l/logs/build-first-stage.log is the canonical Linux log;
logs/first-stage.stdout.log and .stderr.log contain the Windows job output.

## Completed

- Verified firmware-only backup; phone returned to stock Android with locked
  bootloader and green verified boot.
- Extracted 5,689 system and 3,514 vendor regular files directly from the backup.
  Paths, hashes and recorded symlinks are in firmware/extracted/*-inventory.json.
  Extraction does not instantiate Unix symlinks on Windows and is not a rebuilt
  filesystem image. The original verified region files remain authoritative.
  An independent ADB SHA-256 check of extracted system/lib64/libgui.so matched.
  The two selected vendor libraries deny shell access, so their live-file
  crosschecks are unavailable, not hash mismatches.
- Recovered stock kernel configuration and measured boot header/partition values
  into device/hisense/a6l/hardware.json and stock/kernel.config.
- Added lineage_gsi_a6l, a system-only compile probe inheriting the current
  upstream LineageOS GSI product. It is built and filesystem-checked, but has
  not been boot-tested.
- Located custom e-ink interfaces across libgui, vendor HWC/TCON, and kernel.
  See eink-port.md for evidence and implementation sequence.
- Installed WSL 2.7.14 and Ubuntu 24.04.4 after the Windows restart. Dedicated
  Linux build account: a6l. Build prerequisites installed successfully.
- Set WSL memory to 48 GB and swap to 16 GB in the previously absent host
  .wslconfig. Verified Linux sees 47 GiB RAM and 16 GiB swap. Initial ext4 free
  space was 955 GiB; host disk must also retain room as the virtual disk grows.
- Synced all 1,040 projects successfully in /home/a6l/android/a6l-lineage24.
  Sync log: /home/a6l/logs/source-sync.log. Pinned manifest saved in the source
  root and copied to desktop lineage-manifest/a6l-source-revisions.xml.
- Corrected the lunch choice to include cp2a, as specified by the inspected
  upstream release file. The build script reads that file from the actual sync.
- Mapped stock e-ink SurfaceComposer transactions 30, 33 and 34 using both client
  proxy disassembly and the server dispatch table. See eink-port.md.
- Extracted and disassembled the VDEX framework/services bytecode; located
  EpdManagerService's mode, touch and display-switch operations. Recovered and
  decoded the two kernel-appended device trees. All stock-derived outputs remain
  in private firmware storage.
- First compile probe: product configuration accepted Android 17,
  LineageOS 24.0, CP2A.260605.016, arm64 armv8-a with 32-bit arm secondary ABI.
  Host Soong bootstrap compiled. The first full dependency-graph analysis was
  killed by Linux OOM after filling the 48 GB memory allocation and 16 GB swap.
  No system image completed. Free Linux disk space before this: 819 GiB.
- Retried with tools/Configure-SoongMemory.py and A6L_SOONG_GOMEMLIMIT=36GiB.
  Soong clears its subprocess environment, so a small opt-in host-source patch
  forwards that value as GOMEMLIMIT. Saved diff: tools/soong-memory.patch.
  git diff --check passed and bootstrap.ninja contains GOMEMLIMIT=36GiB.
  This is a Go soft memory limit, not a hard cap.
- Subsequent tool-attached attempts ended when the WSL distribution shut down,
  without an Android source error. Switched to a separate hidden Windows
  wsl.exe process (PID recorded in logs/build-detached.pid). It remained alive
  past those interruptions with the graph builder around 35 GiB RSS and no swap
  in use. Root cause of the tool-attached interruptions is not established.
  The dependency graph then completed successfully in 2m13s. Make configuration
  and packaging-rule generation also passed; that systemimage Ninja build later
  completed successfully as recorded above. No phone boot test has occurred.

## Current source preparation

Kernel and boot investigation can proceed independently of this system build.
See boot-compatibility.md for measured boot layout and the newly discovered
Magisk metadata/AVB flag in the captured firmware. The earlier locked/green
description records Android's reported properties, not independent proof of an
untouched factory image.

Offline kernel/boot preparation results are in kernel-investigation-20260914.md:
boot/recovery body reproduction passes; 140,166 kernel symbols were recovered;
e-ink callback/event interfaces and vendor module ABIs were audited. Specific
remaining blockers include the >=5.10 networking gate, unsupported stock FDE
fstab flag, and the required first-stage init arrangement. The candidate kernel
source search has not yet yielded a matching A6L tree.

tools/Inspect-BuiltSystem.py expanded/copied and checked the completed system
image without mounting or flashing. It requires a final successful-build marker;
the actual image checks passed as recorded above.

tools/start-linux-sync.sh runs the preparation script as a6l with prerequisites
already installed. Source and build output live on the Linux ext4 disk.
The preparation script copied the local target and wrote a6l-source-revisions.xml.
tools/build-linux-probe.sh completed the system-image compile retry, logging
to /home/a6l/logs/build-probe.log.
Previous attempts are timestamped in that log directory. The pinned manifest
must be used together with the local device tree and the recorded Soong patch.
For future starts, tools/Start-A6LBuild.ps1 launches the job independently and
refuses to duplicate a still-running recorded WSL process. Desktop log mirrors:
logs/build-detached.stdout.log and logs/build-detached.stderr.log. Do not start
another build while the current recorded process is active.

Observed nonfatal configuration messages: the inherited compliance-GSI debug
policy option warns because our product name is not on AOSP's fixed allowlist;
and Lineage's dependency scanner does not recognize the unregistered local tree.
Neither prevented product configuration. The GSI security defaults require
separate review before any installation; no warning was suppressed.

First compile/configuration work must resolve old vendor dependencies and boot
layout rather than bypass checks silently. Current GSI snapshots include VNDK
31–34; the phone's vendor requires 28. Current build rules obsolete
BOARD_BUILD_SYSTEM_ROOT_IMAGE. No matching A6L kernel source was found in the
initial GitHub repository/code searches. These are specific unresolved tasks,
not proof that Android 17 or e-ink support is impossible.

## References inspected

- https://github.com/LineageOS/android/tree/lineage-24.0
- https://github.com/LineageOS/android_build/tree/lineage-24.0
- https://github.com/LineageOS/android_vendor_lineage/tree/lineage-24.0
- https://github.com/LineageOS/android_device_xiaomi_sdm660-common/tree/lineage-20
- https://learn.microsoft.com/en-us/windows/wsl/install
- https://source.android.com/docs/setup/start/requirements

Inspected build rules commit: e7cc87a995b25ae412aefa493d2b9c2b0157f29d.
Inspected vendor/lineage commit: 7ca77de13fbdac1c3d3b99518c54a81ab1176635.
The bootstrap script passed bash syntax validation; the extractor parsed and
completed both filesystems. Both Android build targets and their offline artifact
checks completed successfully; runtime compatibility remains unresolved.

The Xiaomi tree is a platform reference, not an A6L hardware donor. Its newer
kernel does not contain evidence that the Hisense e-ink driver is supported.
