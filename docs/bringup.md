# A6L investigation record — 2026-09-14

## Objective and current state

Develop a maintainable LineageOS 24.0 port with both displays, reliable touch
routing, e-ink refresh control, frontlight, sleep/wake, and ordinary phone hardware.
The spare phone is the development target. Avoiding a brick is a requirement;
zero risk cannot be guaranteed. No device modification has been performed.

The live upstream branch exists. Its manifest selects Android 17.0.0_r1.
The default branch is still lineage-23.2; branch existence is not evidence of
release readiness or A6L compatibility.

Source: https://github.com/LineageOS/android/tree/lineage-24.0

## Hardware and build environment

Initial inspection and normal bootloader flashing use a computer and a reliable
USB data cable. No soldering equipment or specialist flashing box is needed for
this stage. Have a microSD card and reader available for the stock TF recovery
route; format and capacity requirements must be matched to the recovered package.

Qualcomm EDL/9008 is a possible lower-level read/restore route. It requires a
compatible programmer and usable USB drivers, not merely a cable. Neither a
generic Snapdragon loader nor an EDL cable establishes recoverability.

This Windows host has about 1.2 TB free on C:, 61.7 GiB usable RAM (64 GB class),
and a Ryzen 7 9800X3D with 8 cores / 16 threads. WSL is not installed.
Building Android requires an
appropriate Linux environment; use a Linux filesystem for source/build output.
Do not start a full sync into this Windows desktop directory. Size RAM/swap and
parallel build jobs after inspecting the host.

Sources:
- https://developer.android.com/tools/releases/platform-tools
- https://source.android.com/docs/setup/start/requirements
- https://github.com/bkerler/edl

## Chinese and Baidu findings

Baidu was searched directly through the browser for `海信 A6L 刷机` after the
web-fetch tool could not access its results. This found an A6L-specific Tieba
guide, not located by the earlier indexed searches.

1. https://tieba.baidu.com/p/9273160250 — titled
   `海信A6L解Bootloader(BL锁)以及root教程（HLTE730）`, dated 2024-11-17.
   The visible author replies report using QFIL with
   `prog_emmc_ufs_firehose_Sdm660_ddr_30060000.elf` from an A6 package on an A6L.
   This is a loader-compatibility lead, NOT permission to flash A6 firmware.
   A later reply reports expired resource links. The browser exposed only part
   of the discussion; the complete procedure and downloads remain unverified.
2. https://fans.hisense.com/forum.php?mod=viewthread&tid=168968 — official
   A6L TF-card package announcement for HLTE730T/B1/B6,
   L1632.6.02.10.00. Describes FAT-formatted TF recovery with an HLTE730T_TF
   folder. The download is hidden behind a reply/login. This old version is a
   recovery lead, not a selected firmware for our phone.
3. https://fans.hisense.com/thread-216775-1-1.html — March 2025 user report
   corroborating a Hisense-specific fastboot and Magisk route; also documents
   recovery/boot-loop pitfalls. Do not copy its flashing commands blindly.

These sources establish useful backup/unlock leads. They do not establish an
existing modern ROM with complete e-ink support. No such ROM was found.

## Independent A6L backup evidence

https://github.com/tombaczynski/Hisense-A6L/blob/main/rooting.md reports reading
boot, recovery, and vbmeta using QFIL and the same named SDM660 programmer on
stock software 6.08.03. It identifies storage as eMMC. The model, exact build,
partition layout, and loader compatibility must still be checked on our unit.

The companion downgrade notes identify a two-file stock TF package:
`LA6929(HLTE730T)_L1632.6.06.01.00_TFDownload_202009130456_user.7z`.
It has not been downloaded or authenticated here. The repository links a MEGA
resource folder, not an official cryptographically verified release.

## Backup and recovery gates

Before unlock or flash:

1. Record the spare's product/variant, fingerprint, bootloader state, kernel,
   storage geometry, partition names/sizes, and stock screen behavior.
2. Obtain matching stock firmware and inspect its contents and provenance.
   Hash files locally; a local hash proves subsequent consistency, not origin.
3. Establish a supported way to read raw partitions. Ordinary unrooted ADB does
   not generally expose block devices. An app backup is not a ROM image.
4. Prefer a pre-unlock EDL read if the documented route is validated. Otherwise
   assess a recovery/root route explicitly, including the unlock data wipe.
5. Preserve GPT/layout and readable partitions, especially original boot,
   recovery, vbmeta, system/vendor and any other OS partitions actually present.
   Preserve device-specific radio/NV/calibration partitions (such as modemst1,
   modemst2, fsg, persist when present), plus panel assets discovered during
   inspection. Never substitute these with the everyday phone's copies.
6. Verify byte sizes, capture tool exit status and errors, hash the dumps, and
   compare repeat reads where the source is stable. Keep a second copy off this
   PC. Record how each image can be restored, including image format and offsets.
7. Establish entry AND exit for recovery/bootloader; evaluate EDL separately.
   A TF package cannot be assumed to recover a destroyed bootloader. Reading a
   partition is not proof that restoration works. A complete backup may still
   omit secure/RPMB state and cannot guarantee recovery from every failure.

Initial experiments should preserve the stock boot chain and recovery wherever
possible. Do not relock with custom images, mass-flash partitions, erase NV data,
or alter panel voltage/waveform calibration. Do not write to guessed sysfs nodes.

## E-ink technical starting point

Source: https://github.com/WanderingArrow/Hisense_A6L_Eink_Display

The Android 11 experiment reports rear-panel and touch activation through sysfs,
plus Hisense-specific SurfaceFlinger extensions. These are hypotheses to verify
against stock traces. In particular:

- The prose calls mode 6 a full refresh while the script calls it a high-speed
  interactive mode. Do not treat these labels as a validated mode table.
- Its claim that the Java path is unchanged conflicts with its own descriptions
  of EPD-specific framework methods; scope and behavior need investigation.
- It hardcodes an input-event device and uses global display size changes.
  A proper port needs discovered input routing and coherent display lifecycle.
- Android 11 as an absolute ceiling is unproven. Diagnose newer-version failures
  with logs rather than assuming a universal cryptographic limit.

AOSP explicitly supported Keymaster 4.0/4.1 in Android 12:
https://source.android.com/static/docs/compatibility/12/android-12-cdd.pdf

## Implementation milestones

1. Stock evidence and recoverability, then raw backup.
2. Extract boot configuration, DTB/DTBO where present, fstab, vendor interfaces,
   proprietary libraries, init and SELinux policy; locate usable kernel sources.
3. Define a real device tree from evidence. No guessed partition sizes, offsets,
   kernel configuration or claimed build target.
4. Reproduce a conservative boot baseline, then pursue LineageOS 24 booting.
   Keep graphics, encryption/key services, vendor compatibility and kernel
   requirements separately diagnosable. A temporary older baseline is a test
   instrument, not a change to the target.
5. Implement and validate screen switching, refresh modes, touch, frontlight,
   lock screen, orientation, suspend/resume and battery consumption.
6. Validate calls/SMS/data/IMS, audio, cameras, fingerprint, sensors, Wi-Fi,
   Bluetooth, GPS, charging, storage, encryption and SELinux enforcing.
7. Reproducible installation/rollback and maintenance documentation; submit
   upstream when the device meets requirements.

Official support is a separate milestone from a working unofficial ROM.
LineageOS requires source-built kernels for non-GKI devices and has hardware,
security and maintenance requirements. Changes go through Gerrit, with the
new-device submission process, rather than a single GitHub pull request.

Sources:
- https://github.com/LineageOS/charter/blob/main/device-support-requirements.md
- https://wiki.lineageos.org/submitting_device/
- https://github.com/LineageOS/android/tree/lineage-24.0

## Next evidence needed

- Phone's user-visible software version (the build fingerprint alone may not
  identify the matching update package) and confirmation of modification history.
- A verified stock firmware package and compatible EDL programmer.
- Linux build environment and usable A6L kernel source.

## Tool verification

Google platform-tools 37.0.1 was downloaded from the official dl.google.com
endpoint. Download SHA-256:
`45F4D63113E895EBDE0C90F194099A4676B6AC653BD28D54314A9E022BBC1A99`.
ADB starts successfully. The spare device subsequently connected and authorized USB debugging.
The PowerShell 7 collector parses successfully and its missing/mismatched-device
guard was exercised successfully.

## First stock capture

Capture: `captures/20260914-105716-cf2497c2/`. USB debugging is authorized.

- HLTE730T, Hisense, SDM660; Android 9 / API 28.
- Fingerprint suffix L1632.6.01.04; system patch 2020-06-05,
  vendor patch 2019-03-05. Kernel 4.4.153-perf, built October 2020.
- Boot properties report locked and green verified boot; SELinux enforcing.
- Treble enabled, VNDK 28, system-as-root; block encryption enabled.
- Partition links show eMMC, unsuffixed boot/recovery/system/vendor, plus dtbo,
  vbmeta, modemst1/2, fsg and persist. Raw contents/sizes are not backed up.
- Display service exposes 1080x2340 LCD and a 720x1440 secondary display
  labelled HDMI by the stock stack. Both were reported OFF in this snapshot.
- Binder service `epd` implements `com.hmct.epd.IEpdManager`.
- Manifest declares Keymaster 4.0 and graphics composer 2.1.
- Input service reports ft5x06_ts and ft8719_ts devices.

Collection was read-only. Some probes failed or returned partial data (including
restricted proc files, missing paths and lshal exit 136); command exit codes and
stderr are retained. These are not proof that the phone hardware is malfunctioning.
The collector initially tried to hash its own open checksum output. That host-side
bug was fixed by materializing the input file list, and checksums were generated
from the existing capture without repeating phone probes. This capture is
diagnostics, not a ROM backup.

## Firmware backup attempt, 2026-09-14

No restorable firmware backup exists yet. Stock shell access cannot read raw
block devices (root-only permissions); no root tool was found. Direct pulls of
system/vendor did not provide complete copies. Two tar attempts also failed:
exec-out mixed diagnostics into the stream; shell -T separated stderr but the
result failed structural validation. Preserve these as failed-attempt evidence,
not usable backup archives. Logs are under `firmware/stock-files-20260914/`.
The separately pulled Driver.iso has matching host/device SHA-256 (see that
directory's README), but is only a driver package, not firmware.

bkerler/edl and dependencies are prepared in a project-local Python environment;
only help was run, with no EDL connection or driver installation. The missing
prerequisite is an inspected compatible programmer:
`prog_emmc_ufs_firehose_Sdm660_ddr_30060000.elf`. A6L community reports identify
this file; a generic SDM660 loader is not an adequate substitute.

The documented MEGA resource folder is blocked by the browser tool's site
policy. An independent 4PDA attachment was unavailable and the original
AndroidFileHost package reported no mirrors. The user was asked to download
the file manually into `firmware/programmer/`. Before EDL use, inspect its
provenance, hash and ELF/signing metadata and establish the recovery procedure.
The phone remains in normal Android, with no unlock, flash or partition write.

## EDL attempt after user-supplied archive (supersedes prior status)

The user supplied HisenseA6Lsoft_backup.zip. The needed programmer was extracted
from USB_Drivers_Hisense_A6L.7z. SHA-256:
`6003242582a610712c6b32c8f09475fb78a166e4bb3b018e9f01a7b9bb083642`.
Offline parser reports HWID 3006000000010000, OEMVER hmct-slave,
VAR Sdm660LA, root certificate hash
`06a0604b3069cca35fd538f8ca5a8fc5d07c90a6e755f8cfacc1426cb9d75d22`.
This is metadata, not proof of device acceptance or an independently authenticated
stock image. Archive also contains stock 6.06.01 TF firmware and partial image
sets for 6.06.01/6.08.03, including modified images.

ADB reboot edl succeeded with battery at 75%. Windows enumerated 05c6:9008.
Older bundled driver catalogs were test signed and NOT installed. The newer
package's Windows10 qcser.cat validated as Microsoft Windows Hardware
Compatibility Publisher. After user UAC approval, qcser.inf 2.1.3.5 (2018-12-17)
installed and the interface appeared healthy on COM3. Driver installers were
administratively extracted with reboot suppressed; only qcser.inf was installed.

No GPT or raw partition data was obtained. The initial bkerler serial attempt
received Sahara END_TRANSFER error 1 in response to its probe, then incorrectly
fell into streaming handling and stalled. Stopped host process. A direct Sahara
reset request returned RESET_RSP (0800000008000000), but normal Android did not
subsequently appear. Later connection attempts stalled as well.

Local experimental seriallib.py changes: initialize binary mode for Sahara,
replace destructive flushOutput with flush, disable hardware flow control,
set finite serial/write timeouts. These changes are not yet proven on a working
handshake. No programmer upload was reported and no storage write/unlock command
was issued. Logs: firmware/raw-backup-20260914/. All tool processes were stopped.
User was instructed to unplug USB and hold power about 20 seconds to attempt a
physical restart; normal Android boot must be confirmed before continuing.

## Successful firmware backup (supersedes stalled-attempt status)

Physical restart succeeded. Windows serial RX preservation and single-transfer
writes resolved Sahara upload. Phone PK_HASH matches the loader root hash.
Programmer loaded, eMMC GPT read, and all 59 partitions were identified.
The user explicitly chose to exclude userdata. The final backup covers the
other 58 GPT partitions, primary/secondary GPT and prefix gaps using two files
in firmware/raw-backup-20260914/. Exact offsets and SHA-256 hashes are recorded
in firmware-verification.json; total region size is 10,405,042,176 bytes.

Both GPT header/entry checksums passed. Separate phone reads of boot, recovery,
vbmeta, dtbo, modemst1, modemst2, fsg and persist match their saved image slices.
The interrupted initial transfer crossed an Android reboot, which changed
modemst2. The entire region before system was re-read in the final session and
the local image refreshed before successful verification. Historical pre-reboot
data is retained but is not part of the final verified region pair.

Host fixes and tools: tools/edl-a6l-windows.patch (edl base commit
2f8e89a848afaaef68997fcbcb5b178d958d497b), Resume-FirmwareBackup.py,
Verify-RawBackup.py and Verify-FirmwareBackup.py. Verifier checks were exercised
against valid synthetic images, corrupt GPTs, truncation and mismatched
independent reads. No phone write/unlock commands were issued.

The backup excludes userdata, hardware boot areas, RPMB and fuses. Restoration
has not been tested. Never treat the prefix file as a full disk image.
The stock kernel and embedded config were recovered; it declares
CONFIG_FB_HS_MDSS_EPD_PANEL=y and identifies eink,ed052tc2, a 720x1440 panel.

Post-backup reset succeeded. Android reported boot completed, bootloader locked,
verified boot green and the original stock fingerprint via ADB. Evidence:
firmware/raw-backup-20260914/post-backup-boot.json.
