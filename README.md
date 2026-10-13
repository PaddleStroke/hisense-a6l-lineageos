# Hisense A6L: LineageOS 24 (Android 17) port

An unofficial LineageOS 24 / Android 17 port for the **Hisense A6L (HLTE730T)**: Snapdragon 660, front 1080x2340 LCD and
a rear 720x1440 e-ink panel. It is built on a mainline-based Linux 7.2 kernel, Mesa freedreno for the GPU, libcamera
for the cameras and an A6L-specific e-ink stack (mirror, LCD/e-ink switching, lock-screen clock).

One person developed it on one spare phone, with help from AI coding agents. **The project is paused as of
13 October 2026.** This repository holds the device tree, kernel patches and device-tree overlays, the flashing
tools and the dated engineering notes. **It does not contain flashable images.** See [Installation](#4-installation)
before you plan anything.

## 1. Status

The maintained checklist is [docs/port-status.md](docs/port-status.md). In short (13 Oct 2026):

**Works** (checked on the phone):
- boot and restart; SELinux **enforcing**;
- LCD, touch, GPU;
- rear e-ink: mirror, LCD/e-ink switching, e-ink key wake, lock-screen clock;
- CPU frequency scaling on both clusters;
- hardware video decode/encode;
- all 3 cameras (photo, video, autofocus, flash/torch). Image quality is a little below stock;
- speaker, earpiece, wired headset, microphone;
- Wi-Fi, Bluetooth, GNSS, sensors;
- charging and battery status, thermal mitigation;
- Google apps and the Play Store.

**Partial:**
- Mobile data, calls and SMS work. **VoLTE does not**: the tested carrier (Orange France) refuses Hisense devices on
  its network side. This is not a ROM bug, and only one carrier was tried.
- Idle power: a fix for the overnight drain was verified live. An unplugged overnight measurement is still missing.
- The release ("user") build works, with known bugs. See [4.4](#44-user-release-build-vs-userdebug-build).

**Not working / open:**
- **Stability.** Occasional hard resets and freezes under heavy load were seen on several days. The cause is unknown.
- **No /data encryption.** /data is plain ext4 (no FBE). Anyone holding the phone can read it.
- **Apps that require an unmodified, certified device** may refuse to run: banking, identity, some DRM apps. The
  bootloader is unlocked (`verifiedbootstate=orange`), the device is not Google-certified, and KeyMint is a software
  implementation (no TEE). A French identity app was tested on the user build and still refused the device. Bypassing
  these checks is not a goal of this project.
- adb drops at every suspend (USB is not kept alive while asleep).
- No fingerprint reader support, no Widevine DRM, no Vulkan. The Wi-Fi MAC address is random at every boot.

## 2. Warnings

- **Experimental software. It can brick your phone.** All flashing is done with low-level tools (Qualcomm EDL/Firehose
  and raw partition writes from a diagnostic recovery). A mistake can leave the phone unbootable. EDL may be the only way
  back, and only if you made your own backup first.
- **Your warranty is void**, and the bootloader unlock may not be reversible. The unlock changes persistent security
  state. Normal relocking is not possible with non-Hisense-signed images, and no relock procedure was ever tested.
- **Unlocking and installing erase all user data.** Back up everything first.
- **Back up the whole phone over EDL before anything else** (all partitions except userdata), and keep a copy off your
  PC. Restoring that backup was never tested end to end. Your modem calibration (`modemst1/2`, `fsg`) and `persist` are
  unique to your phone: never use another phone's copies.
- **The flashing tools are pinned to the developer's phone and laptop** (see [4.1](#41-read-this-first)). They refuse to
  run on any other phone. This is on purpose.
- Only one unit was tested: an HLTE730T with 128 GB eMMC on stock firmware L1632.6.01.04 (Android 9). Other variants
  (B1/B6, other storage sizes, other stock versions) are untested.
- Do not use it as your only phone for anything critical: emergency calls, 2FA, banking.

## 3. Requirements

**Phone**
- Hisense A6L, model HLTE730T.
- Stock Android 9. The install tools check for the fingerprint of firmware L1632.6.01.04.
- Battery at 60 % or more for the EDL install. The tools refuse below 40–60 %, depending on the step.

**Computer**
- A Linux PC with a direct USB connection.
  - Bootloader (fastboot) traffic was unreliable through Windows/WSL USB forwarding. It worked on a native Linux laptop
    once `fwupd` was stopped and the runtime USB quirk `18d1:d00d:k` (no link power management) was applied.
  - The first EDL backup was made on Windows with a patched `edl` (see below). Later EDL work ran on Linux.
- A reliable USB data cable.
- The laptop tools assume a GNOME desktop (`gnome-session-inhibit` keeps the laptop awake during a flash), a non-root
  user, `/usr/bin/python3` and `/usr/bin/adb`. Each run needs about 12 GB of free disk space.
- A Linux build machine, only if you build from source (see [5](#5-building-from-source)).

**Software**
- Python 3.
- Android platform-tools: `adb` and Google `fastboot` 37.0.1 were used.
- [bkerler/edl](https://github.com/bkerler/edl) at commit `2f8e89a848afaaef68997fcbcb5b178d958d497b`, plus
  `tools/edl-a6l-windows.patch` (Sahara/serial fixes) and `pyusb`/`libusb`.
- The SDM660 Firehose programmer `prog_emmc_ufs_firehose_Sdm660_ddr_30060000.elf` (the one used has SHA-256
  `6003242582a610712c6b32c8f09475fb78a166e4bb3b018e9f01a7b9bb083642`). It is **not redistributed here**. A6L community
  sources describe where it comes from (see [Credits](#8-credits-and-licence)). A generic SDM660 loader is not a
  substitute.
- For the bootloader unlock: a `fastboot` built from AOSP `system/core` with the A6L vendor command added
  (`tools/Prepare-FastbootVendorCommand.py`).

## 4. Installation

### 4.1 Read this first

- **No images are published.** `*.img`, `*.erofs`, the flash kits, the stock firmware backup and the build outputs are
  all excluded from git (see `.gitignore`). You need to build the images yourself, or ask in the
  [issues](https://github.com/PaddleStroke/hisense-a6l-lineageos/issues) whether images will be shared.
- **The tools are not plug-and-play for another phone.** On purpose, they hard-code and check:
  - the developer's phone serial and its Sahara/EDL identity;
  - its GPT, byte for byte (the GPT holds a disk GUID unique to each phone);
  - the hashes of its stock partitions;
  - the laptop account name and the physical USB port.
  To use them on your phone you must first make your own verified backup, then change these pins and review the
  result. That adaptation is **not documented** yet.
- The procedures below are what was done on the developer's phone, taken from the dated notes in `docs/`. They are a
  map, not a tested guide for new users.

**Partitions the ROM uses.** The A6L has one eMMC, an A-only layout and no dynamic partitions.
- The ROM writes `boot`, `dtbo`, `system` (EROFS) and `vendor` (EROFS).
- A wipe zeroes the first MiB of `metadata` and of `userdata`. The first boot then formats both as ext4.
- `recovery` holds the diagnostic recovery, which is the maintenance path. The ROM never writes it.
- `vbmeta` stays stock (verification disabled; the unlocked bootloader accepts this).
- Never written: `persist`, modem EFS (`modemst1/2`, `fsg`, `fsc`), `misc`, `devinfo`, the GPT and the bootloader
  chain.

### 4.2 First install from stock (the path actually taken)

This is the order of the steps on the developer's phone. Every step was attended and gated by hash checks.

1. **Read-only capture.** With USB debugging on:
   `pwsh ./tools/Collect-A6L.ps1 -Serial '<adb serial>'`.
   This collects diagnostic evidence. It is not a backup. See [docs/bringup.md](docs/bringup.md).
2. **Full EDL backup.**
   - Enter EDL with `adb reboot edl` from stock Android. The phone then shows up as USB `05c6:9008`.
   - Load the programmer with the patched `edl`, read the GPT and back up every partition except `userdata`.
   - Verify the result with `tools/Verify-RawBackup.py` and `tools/Verify-FirmwareBackup.py`.
     `tools/Resume-FirmwareBackup.py` resumes an interrupted read.
   - See [docs/bringup.md](docs/bringup.md), sections "EDL attempt" and "Successful firmware backup".
   - A hardware way into EDL (key combination or test point) is **not documented**. Without stock Android or the
     diagnostic recovery, there may be no way in.
3. **Bootloader unlock.** See [docs/unlock-checkpoint-20260915.md](docs/unlock-checkpoint-20260915.md),
   [docs/bootloader-inspection-20260914.md](docs/bootloader-inspection-20260914.md), and the
   [community procedure](https://github.com/tombaczynski/Hisense-A6L/blob/main/rooting.md).
   - The standard `fastboot flashing unlock` is rejected by this bootloader ("Unlock is not allowed!").
   - What worked:
     1. reboot to the bootloader;
     2. send the vendor command `Hisense unlock` with the patched client;
     3. read back both unlock flags;
     4. run `fastboot erase avb_custom_key` with the official client, which persists the change;
     5. reboot.
   - The first boot after the unlock showed "Decryption unsuccessful" and needed a factory reset.
   - The bootloader has no `fastboot boot` (no RAM boot), so every test image has to be written to a partition.
4. **Diagnostic recovery.**
   - A custom recovery (V74, later revisions labelled V75) was written to `recovery` over EDL, with full readback.
     Tools: `tools/Launch-ControlsV74CInstall.py` / `Run-LaptopDiagnosticInstall-user-v74c.py` and the V75 variants.
   - It boots an A6L mainline kernel and gives a root adb shell outside Android. Its adb serial is `HLTE730T-PROBE`.
   - These tools also require stock Android, so this step comes before the ROM install.
   - See [docs/recovery-v74-candidate-20260923.md](docs/recovery-v74-candidate-20260923.md) and
     [docs/usbrec-20260926.md](docs/usbrec-20260926.md).
5. **ROM install over EDL**, from stock Android, unlocked, with the battery at 60 % or more.
   - Laptop, in the kit directory:
     ```
     python3 Verify-RomV1Stage.py          # offline, must print "passed": true
     python3 Launch-RomV1Install.py && tail -f rom-v1-install-launch.log
     ```
   - The coordinator (`Run-LaptopRomInstall-v1.py`) checks the phone state, then runs `adb reboot edl`. The worker
     (`Write-LaptopRomV1.py`, using `RomFlashEngineV1.py` and `RomFlashLayoutV1.py`):
     - backs up every region it will touch;
     - refuses unless system/vendor/dtbo match the stock hashes;
     - writes boot, dtbo, vendor and system, and zeroes the first MiB of `metadata` and `userdata`;
     - reads every written range back, then powers the phone off.
   - Firehose commands are allowlisted (no erase, no patch).
   - **Keep `capture-rom-v1-install/`**: it holds the backup that `Launch-RomV1Restore.py` needs to go back to stock.
   - See [docs/flash-20260924.md](docs/flash-20260924.md).
6. **First boot.** Press Power.
   - The bootloader shows its orange-state warning.
   - The first boot formats `/metadata` and `/data` and takes 3–5 minutes.

### 4.3 Updating between rounds (keep-data kit)

Development builds came in "rounds", each shipped as a kit. A kit holds the images plus pinned copies of the tools.
An update writes **only the partitions that changed** among boot/dtbo/vendor/system, so **data is kept**. The update
runs over adb from the diagnostic recovery, not over EDL: EDL entry from LineageOS is unproven, because the A6L kernel
has no `reboot edl`.

1. **Stage the kit (build machine, offline).** The kit must be staged against the build that is **installed** on the
   phone.
   - `tools/Prepare-RomUpdateStage.py <new kit> <installed kit>` adds the update tools and the compatibility data.
   - It also runs dry runs and prints `A6L_UPDATE_STAGE_PASS` or `A6L_UPDATE_STAGE_FAIL`.
   - It refuses any change of signing key, of /data format, or a security-patch downgrade.
   - `tools/stage-rom-v2-kit-laptop.sh` copies the kit to the laptop and checks every hash there.
   - The per-round wrapper the developer used (`stage-round4-kit.py`) lives in the ignored `firmware/` tree and is not
     published.
2. **Check the kit (laptop, offline):**
   ```
   sha256sum -c --quiet KIT-SHA256SUMS && python3 Verify-RomV1Stage.py
   python3 Run-LaptopRomUpdate-v1.py --mode update --transport adb-recovery --dry-run
   ```
3. **Enter the diagnostic recovery.** Either:
   - `adb reboot recovery` from LineageOS (or pass `--enter-recovery` in the next step), or
   - unplug USB, hold Power until the phone is off, then Power+Volume Down, and choose Recovery in the bootloader menu.
   `adb devices` must show `HLTE730T-PROBE  recovery`.
4. **Flash:**
   ```
   python3 Launch-RomUpdateV1.py --mode update --transport adb-recovery --backup-policy hash-only [--enter-recovery]
   ```
   It checks:
   - the recovery identity;
   - the eMMC geometry against the GPT;
   - vbmeta, devinfo and the boot control block;
   - that the installed images are the kit's predecessor.
   Then it writes each changed image in chunks with an on-phone hash check, reads everything back, and re-hashes the
   protected regions (data, EFS, persist, recovery, GPT), which must be unchanged. A run takes from a few minutes (one
   image) to about 15 minutes (all four). Read
   `capture-rom-update-update-<UTC>/edl/report.json`:
   - `readback_verified` and `invariants_unchanged` must be true;
   - `worker_exit` must be 0.
5. **Boot Android from the recovery:**
   `adb -s HLTE730T-PROBE shell 'echo b > /proc/sysrq-trigger'`, or press Power if the phone is off.

If a run stops: keep USB connected, do not retry blindly, and read `report.json` first.

### 4.4 User (release) build vs userdebug build

| | userdebug ("round 29b") | user (release keys) |
|---|---|---|
| Purpose | development ROM, recommended for testing | daily use; testing apps that reject debug builds |
| Signing | test keys | the developer's private release keys |
| Root / `adb root` | yes | no |
| Kernel log kept after a crash | yes (copy in /data) | no |
| SELinux | enforcing | enforcing |
| Known issues | stability under load (open) | USB debugging comes up **enabled** (cause not found yet); setup with USB plugged in can loop ("Copy apps & data" crash), so unplug during setup; two hard resets seen, cause unknown |

Build recipe for the user build: `build-user.sh check | build | sign | images | audit | prekit`. It lives in the
ignored `firmware/` tree. The signing scripts are in `tools/release/` (`gen-release-keys.sh`, `sign-a6l-release.sh`).
Private keys are never part of this repository. If you build your own user build, you sign it with your own keys.

**Switching between test keys and release keys needs a data wipe**, in both directions. The platform signing
certificate changes, and Android cannot boot the old /data. The keep-data kit refuses this switch. The **wipe-data
mode** of the same tools handles it:

```
# phone booted in LineageOS (the kit's predecessor build), exactly one adb device, battery >= 50 %
python3 Run-LaptopRomUpdate-v1.py --mode update --wipe-data --confirm-wipe <phone serial> --transport adb-recovery \
        --backup-policy hash-only --enter-recovery --dry-run
python3 Launch-RomUpdateV1.py --mode update --wipe-data --confirm-wipe <phone serial> --transport adb-recovery \
        --backup-policy hash-only --enter-recovery
```

- The kit must have been staged for a wipe (`Prepare-RomUpdateStage.py ... --wipe-data`).
- The mode accepts only the key-switch compatibility problems.
- It writes the images and reads them back. **Only then** does it zero the first MiB of `metadata` and `userdata`.
  It checks that neither is mounted, and that everything else is unchanged.
- The coordinator's expected serial is a constant set to the developer's phone.
- **Back up first:** photos and files (`adb pull /sdcard/...`), app data, 2FA apps. Also note any
  `persist.vendor.a6l.*` setting you changed by hand: it lives only in /data and will be lost.
- After the flash, the first boot formats /data and takes about 3 minutes. If it lands in the diagnostic recovery
  instead, reboot with the sysrq command above.
- Updates between two builds signed with the **same** keys keep data with the normal kit.

## 5. Building from source

Outline. **The public repository is not enough to reproduce a build.**

1. **Host.** Ubuntu 24.04. The developer used WSL2 with 48 GB RAM and 16 GB swap for WSL.
   - Plan for roughly 270 GB for the source tree and the userdebug `out/`, plus about 185 GB for a separate user
     `OUT_DIR`.
   - A full build takes several hours: about 4 h for the last user build, 6–8 h estimated from scratch.
   - Keep the source on the Linux filesystem, not on `/mnt/c`.
2. **Source.** `tools/prepare-linux-build.sh` installs the build dependencies, runs
   `repo init -u https://github.com/LineageOS/android.git -b lineage-24.0` and syncs, then copies
   `device/hisense/a6l` into the tree.
   - The exact revisions used were saved as `a6l-source-revisions.xml` in `lineage-manifest/`, which is **not
     published** (gitignored).
   - Soong needs a memory limit on this host: `tools/Configure-SoongMemory.py` and `A6L_SOONG_GOMEMLIMIT`.
3. **Kernel.** Linux 7.2.3, based on [sdm660-mainline](https://github.com/sdm660-mainline/linux).
   - A6L patches, configs and device-tree overlays are in `device/hisense/a6l/kernel/`.
   - The kernel trees and build scripts lived on the build machine. They are only partly in this repo.
4. **Product and pipeline.** The lunch target is `lineage_gsi_a6l` (`lunch lineage_gsi_a6l <aosp_target_release> userdebug`; the release used was `cp2a`).
   - `tools/rom-v2-pipeline.sh <tag> prep build boot qemu flash` does the rest:
     - prep: syncs the subtrees, stages the prebuilts, builds the DT;
     - build: `m systemimage vendorimage`;
     - boot: packages `boot.img`/`dtbo.img` and checks them with an emulation of the captured bootloader;
     - qemu: test boot in QEMU;
     - flash: install test on a virtual eMMC;
     - then it creates the kit.
   - Useful environment flags: `A6L_KERNEL=r5`, `A6L_SEPOLICY_ROM=1`, and `A6L_SELINUX_PREP=1` (required for enforcing
     and user builds).
5. **Private inputs.** `tools/stage-rom-v2-prebuilts.sh` pulls firmware blobs and modules from local paths and from
   the developer's private stock backup (`firmware/`, gitignored). Proprietary firmware is not redistributed. You need
   to extract it from **your own** phone's backup. That extraction is **not documented** as a standalone procedure.
6. Later rounds were built incrementally and audited image by image, with per-round scripts that are not published.

Start with [docs/build-checkpoint.md](docs/build-checkpoint.md), [docs/merge-20260925.md](docs/merge-20260925.md),
[docs/rom-r6-build-20260929.md](docs/rom-r6-build-20260929.md) and
[docs/release-prep-20260927.md](docs/release-prep-20260927.md).

## 6. Recovery and rollback

- **Get to a root shell outside Android.** Boot the diagnostic recovery (see [4.3](#43-updating-between-rounds-keep-data-kit),
  step 3). It reads `/data` (not encrypted) for logs and tombstones, and it is the transport for every update.
- **Phone stuck on a backlit black screen** after a restart with USB plugged in: unplug USB first, then force it off
  by holding Power, and reboot.
- **Undo a keep-data update.** Re-flash the previous kit. If the update was run with `--backup-policy snapshot`,
  `Launch-RomUpdateV1.py --mode rollback --transport adb-recovery --update-capture <capture>/edl` writes the saved
  partitions back.
  - Hash-only updates (the default) save no partition copies: roll back by flashing the previous kit.
  - A rollback across a wipe is itself a wipe (see [4.4](#44-user-release-build-vs-userdebug-build)).
- **Back to stock Android.** Use `Launch-RomV1Restore.py` over EDL with the `capture-rom-v1-install/` backup from the
  first install.
  - It writes the saved partitions back byte for byte and zeroes the head of `userdata`. Stock recovery then has to
    wipe data.
  - Getting into EDL from LineageOS is unproven. Documented ways in: `adb reboot edl` from stock Android, or a hardware
    method (not documented).
  - The full restore has only been tested on a virtual eMMC, not on the phone.
  - Putting the stock recovery back is a separate EDL tool (`Run-LaptopDiagnosticRestore-user-*.py`).
- **Last resort.** Your own full EDL backup (step 2 of [4.2](#42-first-install-from-stock-the-path-actually-taken)).
  It does not cover RPMB, fuses or the separate e-ink SPI flash.

## 7. Repository layout

| Path | What |
|---|---|
| `device/hisense/a6l/` | Device tree: product makefiles, `rom/` (init scripts, fstab, sepolicy, the `user` variant, release/OTA prep), `kernel/` (patches, configs, DT overlays), `eink/`, `camera/`, `audio/`, `radio/`, `hals/`, `power/`, `wifi/`, `usb/`, `gnss/`, `sens/`, `video/`, `watchdog/` |
| `tools/` | Flashing tools (`RomFlash*`/`RomUpdate*` engines and layouts, `Launch-*`, `Run-Laptop*`, `Write-Laptop*`, `Prepare-RomUpdateStage.py`, `Verify-RomV1Stage.py`), build pipeline (`rom-v2-pipeline.sh`, `prepare-linux-build.sh`, `stage-*`), offline tests (`Test-*`, `tests/`), signing and SELinux helpers (`release/`). Many `*-vNN` files are historical bring-up steps. |
| `docs/` | Dated engineering notes. Start with `port-status.md`, then `handoff-20261005-claude.md` for the most recent history. Older "current" statements describe their date. |
| `research/` | Analysis notes and test evidence from the bring-up. |
| `firmware/`, `captures/`, `lineage-manifest/`, `references/`, `logs/` | Local only (gitignored): stock firmware, captures, kits, build inputs. Never publish these: they contain device identifiers, calibration data and proprietary files. |

## 8. Credits and licence

- [LineageOS](https://lineageos.org/) and AOSP.
- [sdm660-mainline](https://github.com/sdm660-mainline/linux) (kernel base), Mesa (freedreno), libcamera.
- [bkerler/edl](https://github.com/bkerler/edl) (Qualcomm EDL client).
- [tombaczynski/Hisense-A6L](https://github.com/tombaczynski/Hisense-A6L): rooting, unlock and downgrade reports.
- [WanderingArrow/Hisense_A6L_Eink_Display](https://github.com/WanderingArrow/Hisense_A6L_Eink_Display): early e-ink
  experiments.
- The A6L threads on Baidu Tieba and the Hisense forum (see [docs/bringup.md](docs/bringup.md)).

**Licence:** there is no top-level LICENSE file yet. Some files carry their own SPDX header (for example
Apache-2.0). Kernel patches follow the licence of the code they modify. Until a licence is chosen, see the individual
files. No proprietary Hisense or Qualcomm firmware is distributed here.

Questions, reports from other A6L owners and help with the open items are welcome in the
[issues](https://github.com/PaddleStroke/hisense-a6l-lineageos/issues).
