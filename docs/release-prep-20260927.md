# Release preparation: signing, OTA, SELinux enforcing, encryption, Play (agent `rel27`, 27 Sep 2026)

This is preparation only. Nothing was built (no `m`, no target-files, no OTA), no key was generated, the phone was not
touched, nothing was committed. Every new switch defaults to **off**: with `A6L_RELEASE`, `A6L_SEPOLICY_ROM`, `A6L_FSTAB`
and `A6L_SELINUX` unset, the tree builds exactly the r3 configuration.

| # | item | status | what Pierre has to decide / do |
|---|---|---|---|
| 1 | Release signing | scripts ready, **not run**. Key generation is one command. Signing and OTA are one command after a release build. | key subject, password or no password, where the offline backup lives (keys are forever) |
| 2 | OTA updates | Updater URI, JSON generator (tested on a synthetic zip), recovery + OTA board and product config (**untested**, gated by `A6L_RELEASE=1`) | whether the Lineage recovery replaces the V74 diagnostic recovery; whether to host the JSON in the public repo |
| 3 | SELinux enforcing | new ROM-level policy `rom/sepolicy/vendor` passes an offline compile, link and neverallow check (userdebug and user). An rc seclabel patch is prepared, **not applied**. | first install stays permissive; switch to enforcing after the avc pass (3.5) |
| 4 | Encryption | FBE fstab + kernel fragment prepared. **Blocked:** the kernel has no `CONFIG_FS_ENCRYPTION` (new Image needed); metadata encryption needs dm-default-key (not in mainline); KeyMint and Gatekeeper are software only | rebuild the Image? accept software-only KeyMint/Gatekeeper? |
| 5 | Play certification | documented (5): uncertified-device registration route; Play Integrity limits | none (register after first boot) |

## 0. Facts this rests on (checked 27 Sep 2026)
- **Boot chain.** A-only. Fixed GPT with no super partition (`docs/flash-20260924.md` 1). The ABL is Qualcomm LinuxLoader: boot header v1, `Image.gz` plus an appended DTB, and a normal boot reads the **dtbo partition**. vbmeta is stock with `flags=2`, and the bootloader is unlocked, so AVB result 5 is accepted and nothing is verified. There is no `fastboot boot`, so installs go through EDL (`RomFlashEngineV1`, kit `rom-v3`).
- **Images.** Our boot.img is made by `tools/Prepare-RomV2Boot.py` from the V67 Image, the V75 DTB and a first-stage ramdisk with `sdhci-msm.ko`. The cmdline has `androidboot.selinux=permissive`. system and vendor are EROFS images from `m systemimage vendorimage`, with product `lineage_gsi_a6l` on the generic_arm64 GSI board (`TARGET_NO_KERNEL`/`TARGET_NO_RECOVERY := true`, `PRODUCT_BUILD_RECOVERY_IMAGE := false`, `TARGET_FORCE_OTA_PACKAGE := false`, variant userdebug, `test-keys`).
- **Recovery partition.** It holds the **V74 diagnostic recovery**: a custom ramdisk (init + sdhci-msm + tps65185), with no recovery binary and no updater. It **cannot install an OTA**. No Lineage recovery is configured for a6l.
- **Keystore.** r3 ships `android.hardware.security.keymint-service.nonsecure` (software) and **no Gatekeeper HAL at all**. `gatekeeperd` says "Could not find Gatekeeper device", so a lock-screen PIN or pattern cannot be enrolled. In r3 the KeyMint, minigbm allocator and QTI vibrator binaries are **unlabeled** (`vendor_file`). That is harmless while permissive, but fatal in enforcing: init refuses to start KeyMint, and keystore2 aborts.
- **Kernel.** Checked in `out-a6l-phone-v67/.config`. `FS_ENCRYPTION` is not set. `BLK_INLINE_ENCRYPTION` is not set. `DM_CRYPT=y`, `DM_VERITY=y`, `CRYPTO_AES_ARM64_CE_BLK=y`, `XTS/CTS/SHA512/HMAC=y`, `QCOM_QSEECOM` is not set, `SECURITY_SELINUX_DEVELOP=y`, `MODULE_SIG=y`.
- **Git remote.** `origin` is `https://github.com/PaddleStroke/hisense-a6l-lineageos.git`. The repository is **public** (GitHub API, 27 Sep) and the default branch is `main`.
- **Updater (LineageOS 24, `packages/apps/Updater`).** It reads `lineage.updater.uri` (`{device}`, `{type}` and `{incr}` are substituted) and requires HTTPS. The JSON fetch uses OkHttp with `followRedirects(false)`, so a GitHub Release **redirect URL cannot serve the JSON**. The format is the **v2 list**: `[{datetime, type, version, files:[{filename, sha256, size, url, os_patch_level, os_sdk_level}]}]`. If `os_sdk_level` is missing it is read as 0 < 37, which counts as a downgrade, and the update is **blocked**. A non-A/B device (`ro.build.ab_update` unset, as here) installs through `RecoverySystem.installPackage`, which runs uncrypt, then the BCB, then recovery.

## 1. Release signing
### 1.1 Keys (NOT generated)
`tools/release/gen-release-keys.sh` follows the Lineage wiki *Signing builds* page (fetched from `LineageOS/lineage_wiki` on 27 Sep):
- **Platform certs:** `bluetooth cyngn-app media networkstack nfc platform releasekey sdk_sandbox shared testcert verity`, with `testkey` linked to `releasekey`.
- **APEX keys:** one RSA-4096 key per APEX (`make_key` copy with 2048 changed to 4096, plus a `.pem` payload key). The list is in `tools/release/apex-list.txt` (the wiki list plus `com.android.hardware.audio` and the r3 VNDK APEXes). `--apex-from <target_files.zip>` adds every APEX named in that build's `META/apexkeys.txt`.
- **AVB key:** `avb.pem` + `avb_pkmd.bin`, RSA-4096, for future use only (see 1.3).

The script writes to `/home/a6l/.android-certs` with umask 077 and the directory chmod 700. It refuses Windows-side or repo paths, refuses to overwrite existing keys, and never prints private material; only `fingerprints.txt` (public cert SHA-256) is written. **Command (Pierre):**
```
A6L_KEY_SUBJECT='/C=FR/ST=<region>/L=<city>/O=A6L LineageOS/OU=release/CN=A6L LineageOS/emailAddress=<mail>' \
  bash /mnt/c/Users/Pierre/Desktop/A6L/tools/release/gen-release-keys.sh            # add --password for protected keys
# after the first release target-files exists (catches any APEX the list misses):
A6L_KEYS_ADD_MISSING=1 A6L_KEY_SUBJECT='...' bash .../gen-release-keys.sh --apex-from <target_files.zip>
```
Then back up `/home/a6l/.android-certs` offline, on encrypted media. If the keys are lost, installed phones get no more OTAs.
### 1.2 Sign and OTA (NOT run)
`tools/release/sign-a6l-release.sh <unsigned target_files.zip> <boot-dir> <out-dir>` does the following:
1. Runs `sign_target_files_apks -o -d $K` with `--extra_apks` for the wiki's 15 APEX APKs, and `--extra_apks`/`--extra_apex_payload_key` for **every non-PRESIGNED APEX in `META/apexkeys.txt`**. It stops if a key is missing.
2. Verifies that no apkcerts or apexkeys entry still points outside `$K`.
3. Injects **our** `boot.img` (Prepare-RomV2Boot) as `BOOTABLE_IMAGES/boot.img` and `IMAGES/boot.img`. `common.GetBootableImage` prefers `BOOTABLE_IMAGES`.
4. Runs `ota_from_target_files -k releasekey --block`.
5. Checks that the OTA's boot.img equals ours, writes sha256, and runs `make-updater-json.py`.

`--backup=true` (addon.d) is not used, because the GApps are built in. With password-protected keys, set `ANDROID_PW_FILE`.
### 1.3 Product / board configuration (gated `A6L_RELEASE=1`)
- **Variant `user`.** The pipeline's build phase lunches `user` and adds `target-files-package otatools` (variable `A6L_VARIANT` overrides the variant).
- **`PRODUCT_DEFAULT_DEV_CERTIFICATE` is not set.** Signing happens after the build (wiki method, test keys in the build and `release-keys` after `sign_target_files_apks`). The alternative, `vendor/lineage-priv/keys/keys.mk` (included by `vendor/lineage/config/common.mk`), would put key paths in the tree; not recommended.
- **AVB: `BOARD_AVB_ENABLE := false`.** The GSI board enables AVB with the AOSP *test* keys. Our chain never verifies (vbmeta flags=2, and the unlocked ABL cannot enforce a user key; there is no avb_custom_key flow on this ABL). A vbmeta signed with `avb.pem` would only be cosmetic. It would matter only if Pierre ever wants dm-verity on system/vendor: the `avb` fstab flag, a vbmeta with flags 0 and our key, still ORANGE state. That is left as an option.
- **Kernel modules** are signed by the kernel build's own autogenerated key (`MODULE_SIG=y`, not forced). Keep that key with the release keys if a later Image is ever built with `MODULE_SIG_FORCE`.

## 2. OTA updates
**How an OTA installs on the A6L (non-A/B).** The Updater downloads to `/data/lineageos_updates` and calls `RecoverySystem.installPackage`. uncrypt writes a block map of the zip (required once /data is FBE) and writes the BCB to `misc`. The phone reboots to recovery, where the updater applies the **full block OTA**: system and vendor `.new.dat`, `boot.img` raw to `/boot`, and `recovery.img` (full-recovery-image mode).
- **update_engine is not used** (A-only).
- **The dtbo partition is not rewritten** by a non-A/B full OTA. It keeps the V74 board-id table that the installer wrote, which never changes.
- `persist`, modem EFS, `userdata` and `metadata` are untouched.

**Needed first:** an installable **Lineage recovery**. The V74 diagnostic recovery cannot install. Prepared, and untested:
- `rom/release/BoardConfig-release.mk`:
  - `TARGET_NO_RECOVERY := false` and `BOARD_USES_FULL_RECOVERY_IMAGE := true`.
  - Header v1 with the V74 offsets (kernel 0x8000, ramdisk 0x1000000, second 0xf00000, tags 0x100, pagesize 4096).
  - `TARGET_PREBUILT_KERNEL` = V67 Image.gz + the V75 DTB, **`BOARD_INCLUDE_RECOVERY_DTBO := true`** with the V74 `recovery-dtbo.img` (the same recovery-DTBO path V71/V74 proved), and a 64 MiB boot/recovery partition size.
  - Recovery ramdisk modules: `sdhci-msm.ko`, `a6l_simplefb.ko` (simpledrm on the splash framebuffer for minui), and `edt-ft5x06.ko` if its dependencies are present.
  - `AB_OTA_UPDATER := false`, `TARGET_OTA_ASSERT_DEVICE := a6l`.
- `rom/release/recovery/recovery.fstab` (by-name, erofs system/vendor, `/boot /recovery /dtbo /misc` as emmc, `/data` with the FBE flags, `/metadata`, `/cache`).
- `rom/release/recovery/init.recovery.qcom.rc`: USB configfs plus the `soft_connect` gate (the kernel runs `a6l_manual_usb=1`), so adb and sideload work in recovery.
- `tools/release/stage-release-prebuilts.sh`: builds `Image.gz-dtb`, `dtbo.img` and the recovery modules in the tree. The pipeline calls it in prep when `A6L_RELEASE=1` and checks module dependencies.
- The build-made boot.img exists only to satisfy the build. The **shipped boot.img is always Prepare-RomV2Boot's**, injected by the sign script.

**Updater.** `rom/release/release.mk` sets `lineage.updater.uri=https://raw.githubusercontent.com/PaddleStroke/hisense-a6l-lineageos/main/updater/{device}.json`. It is served raw because the Updater does not follow redirects for the JSON. The OTA zip itself can be a GitHub Release asset: the downloader follows redirects, and the asset limit is 2 GiB against an expected OTA of roughly 1.3 to 1.7 GB, which should fit (to verify).
- `tools/release/make-updater-json.py <ota.zip> --url <https url> [--merge updater/a6l.json] [--legacy]` reads `META-INF/com/android/metadata`: `post-timestamp` becomes `datetime`, and it takes `post-sdk-level` and `post-security-patch-level`. It refuses non-HTTPS URLs, a missing SDK level, or another device, and computes sha256 and size.
- Tested on a synthetic zip; the output format matches `NetworkUpdate.kt`.

**Publish** (after 1.2):
```
gh release create r<N> <out>/lineage-24.0-<date>-UNOFFICIAL-a6l-signed.zip{,.sha256sum}
python3 tools/release/make-updater-json.py <ota> --url https://github.com/PaddleStroke/hisense-a6l-lineageos/releases/download/r<N>/<ota-name> --merge updater/a6l.json > /tmp/a6l.json && mv /tmp/a6l.json updater/a6l.json
git add updater/a6l.json && git commit && git push          # (Pierre)
```
**First install of a release build** stays the EDL kit (the recovery slot is written by the OTA or by the kit). An OTA only works between builds signed with the **same** release key. The first release build must be flashed by EDL (+wipe: test-keys → release-keys changes the platform signature).

## 3. SELinux enforcing
### 3.1 Inventory (device/hisense/a6l/**/sepolicy*)
| dir | compiled in r3 | covers |
|---|---|---|
| radio/sepolicy | yes | hal_radio_default (qipcrtr, rtnetlink/rmnet), a6l_imsdcm, a6l-qmi (label only), vendor_a6l_ril_prop / vendor_a6l_voice_prop |
| audio/sepolicy | yes | a6l_audio_route |
| kvoice/q6voiced/sepolicy | yes | a6l_q6voiced |
| gnss/sepolicy/vendor | yes | hal_gnss_default qipcrtr, label of the GNSS HAL |
| eink/sepolicy/vendor | yes | a6l_epdd, a6l_eink_mirror, composer lease socket, /dev/dri + composer labels, sysfs_a6l_epd (genfs), vendor_eink_prop |
| eink/switcher/sepolicy/{vendor,system_ext} | yes | a6l_dualux, sysfs_a6l_dualux (genfs, UNVERIFIED paths), app props |
| hals/sepolicy | **no** (never compiled; wrong paths `/vendor/bin/rmtfs`, `soc/c0c4000.sdhci`) | superseded by rom/sepolicy/vendor (do not add both) |
| **rom/sepolicy/vendor (NEW)** | opt-in `A6L_SEPOLICY_ROM=1` or `A6L_RELEASE=1` | everything below |

**Services and daemons → domain after this work:**

| service / binary | domain |
|---|---|
| a6l-modules.sh (display/adsp/misc/bootinfo) | `a6l_modules` (was `seclabel vendor_modprobe`) |
| a6l-radio.sh | `a6l_radio_ctl`, which transitions to `a6l_rmtfs` / `a6l_tqftpserv` / `a6l_diag` for `/vendor/a6l/radio/bin/{rmtfs,tqftpserv,diag-router}` |
| a6l_macs | `a6l_macs` |
| a6l-chg-guard.sh | `a6l_chg_guard` |
| radio HAL | hal_radio_default |
| a6l-imsdcm | a6l_imsdcm |
| a6l-q6voiced | a6l_q6voiced |
| a6l-audio-route | a6l_audio_route |
| AIDL audio HAL APEX | hal_audio_default (+ `ro.vendor.a6l.*` read) |
| GNSS | hal_gnss_default |
| sensors multihal + sensors.a6l | hal_sensors_default (+ iio_device, sysfs iio, /data/vendor/sensors) |
| lights | hal_light_default (+ leds/backlight) |
| **vibrator** | hal_vibrator_default (**label was missing**) |
| **KeyMint nonsecure** | hal_keymint_default (**label was missing**) |
| **minigbm allocator** | hal_graphics_allocator_default (**labels were missing**) |
| BT HAL | hal_bluetooth_default (+ `/dev/rfkill` as `a6l_rfkill_device`) |
| Wi-Fi HAL / supplicant / hostapd | AOSP labels |
| epdd / eink mirror / dualux | eink dirs |
| health / power | AOSP labels (+ power_supply sysfs) |
| qrtr (pd-mapper, qrtr-ns) | in-kernel on 7.2; the userspace `qrtr-lookup` is a test tool with no service |
| camera | no HAL, nothing to label |

**Also added:**
- **eMMC partition labels:** system/vendor → system_block_device, boot/dtbo, recovery, userdata, metadata, misc, cache, persist, modem EFS, zram0. They are required by init/fs_mgr, vold, uncrypt and recovery in enforcing mode. r3 has none, so every node was `block_device`.
- `/vendor/lib/modules` → `vendor_kernel_modules` (r3: unlabeled).
- Data dirs and `/dev/a6l`.
- The `vendor.a6l.*` / `persist.vendor.a6l.*` / `ro.vendor.a6l.*` property types.
- **genfs** for remoteproc, ipa, the DPU/DSI0 display subsystem and power_supply. All of these paths are **UNVERIFIED**.
### 3.2 Validation (no build)
`tools/release/check-a6l-sepolicy.sh {userdebug|user} [extra dirs]` repeats soong's vendor policy steps in WSL:
1. m4 with the policy.go defines, the build flags from `soong.lineage_gsi_a6l.variables` and the policyConfOrder file order.
2. `checkpolicy -C -M -c 30`, `build_sepolicy filter_out` (reqd mask + plat_pub), and `version_policy`.
3. A full split-policy link with `secilc` (plat + system_ext + product + mappings + vendor) with **all neverallows**.
4. `sefcontext_compile` of the vendor file_contexts against the linked policy, and a check of the property_contexts types.

It was calibrated against the r3 intermediates: a regenerated `vendor_sepolicy.cil` equals the built one, apart from 7 attribute-list lines. Results, 27 Sep:
- baseline (current repo): **PASS**
- `userdebug rom/sepolicy/vendor`: **PASS** (247 vendor types, 1045 allow)
- `user rom/sepolicy/vendor`: **PASS**. The only failure is the stock tree's own `mediacodec tcp_socket` neverallow, which fails identically without our policy. It is an artefact of linking a user-flavoured vendor half with r3's userdebug plat half, and the script reports it as such.

**Real problems the neverallow check found (fixed in the policy, and they need code/config changes):**
- `net_domain(hal_gnss_default)` is forbidden (HALs may not open sockets, `hal_neverallows.te`). **XTRA/PSDS must be downloaded by the framework** (IGnssPsds → GnssPsdsDownloader, `config_psds_servers`), not by our GNSS HAL. Check what gnss3 does. If the HAL fetches it itself, that breaks in enforcing.
- `set_prop(..., graphics_config_writable_prop)` is forbidden for vendor. **`setprop persist.graphics.egl mesa` in a6l-modules.sh stops working in enforcing.** The release needs `ro.hardware.egl=mesa` statically (patch 0001 below).
- `dac_override` and `dac_read_search` are forbidden for vendor domains (removed; the scripts run as root, so owner checks pass).
- `/dev/rfkill` has no AOSP type. The never-compiled hals policy used a nonexistent `rfkill_device`; it is now `a6l_rfkill_device`.

**Known remaining enforcing gaps (not compile errors; they will show up as avc denials):**
- `a6l-radio.sh` runs `/system/bin/ip link set wlan0 address`. A vendor domain may not exec system binaries. Move the Wi-Fi MAC step into `a6l_macs` (it already has `SIOCSIFHWADDR`).
- The `a6l_logcat` service uses `seclabel u:r:su:s0` (debug only; `su` does not exist in user builds).
- The IIO/SMGR sysfs parents are unknown, so the HAL uses generic `sysfs` rw.
- **Kernel firmware loading from `/vendor/firmware`**: check `avc: ... scontext=u:r:kernel:s0 ... vendor_file`.
- `/persist` keeps stock xattrs (unknown types). Only the release fstab mounts it with `context=a6l_persist_file`.
- `ueventd.rc` sets `/dev/dri/* 0666`: world-writable DRM nodes; render nodes only for release.
- Bug found, fixed in the prepared patch: `radio/init/a6l-imsdcm.rc` starts on `persist.vendor.a6l.radio.enable=1`, a property nothing sets. The ROM uses `persist.vendor.a6l.radio`.
### 3.3 Files
- New policy: `rom/sepolicy/vendor/{README.txt, file.te, property.te, property_contexts, file_contexts, genfs_contexts, a6l_modules.te, a6l_radio.te, a6l_chg_guard.te, hal_extras.te}`.
- `rom/BoardConfig-rom.mk` adds the dir only with `A6L_SEPOLICY_ROM=1` or `A6L_RELEASE=1`.
- `rom/sepolicy/rc-enforcing.patch` (**not applied**; `git apply --check` OK) does three things:
  - removes the 10 `seclabel u:r:vendor_modprobe:s0` lines (init.qcom.rc ×8, wifibt ×1, power ×1), so the file labels select the new domains;
  - fixes the imsdcm trigger;
  - makes a6l-modules.sh also publish `vendor.a6l.gpu=mesa`.
### 3.4 Order of operations
1. **First install (r4 or r5): userdebug, permissive**, with `A6L_SEPOLICY_ROM=1` and the rc patch applied, so the new domains exist and the avc log is attributable. Then collect:
   ```
   adb shell 'dmesg | grep "avc: denied"; logcat -b all -d | grep "avc: denied"' > avc-<tag>.txt
   adb shell 'ls -lZd /sys/devices/platform/soc@0/{15700000,4080000}.remoteproc /sys/devices/platform/soc@0/14780000.ipa; readlink -f /sys/class/power_supply/* /sys/class/leds/* /sys/class/backlight/* /sys/bus/iio/devices/*; ls -lZ /dev/block/by-name/'
   ```
2. Fix the policy from the log, rerun `check-a6l-sepolicy.sh`, rebuild.
3. **Enforcing trial on userdebug:** boot.img with `A6L_SELINUX=enforcing` (Prepare-RomV2Boot drops `androidboot.selinux=permissive`). Rescue is simple: reflash the permissive boot.img (EDL kit) or use the V74 recovery.
4. A **user** build is always enforcing: init is built with `ALLOW_PERMISSIVE_SELINUX=0`, and the cmdline flag is ignored. Per-domain `permissive` statements are also rejected in user builds.

**How to flip:** `A6L_SELINUX=enforcing` (pipeline boot phase) ↔ unset (permissive, r3 default). At runtime on userdebug: `adb root; adb shell setenforce 1|0` (not persistent).

## 4. Encryption
- **Current state:** `/data` is plain ext4 (`formattable`) and `/metadata` is ext4 (aconfig, vold). Android 17 boots this way (QEMU r2/r3 reached boot_completed), but it is a CDD violation and anyone with EDL or an unlocked bootloader can read /data.
- **Prepared:** `rom/vendor-etc/fstab.qcom.fbe`, selected by `A6L_FSTAB=fbe` (the pipeline copies it over fstab.qcom in the tree, and Prepare-RomV2Boot puts it in the first-stage ramdisk):
  - `/data`: `fileencryption=aes-256-xts:aes-256-cts:v2`.
  - Phase-2 line, commented out: `metadata_encryption=aes-256-xts,keydirectory=/metadata/vold/metadata_encryption`.
  - `/persist` with `context=u:object_r:a6l_persist_file:s0`.
  - Kernel requirements are in `kernel/rom-v2/release.config.fragment`.
- **Blockers:**
  1. **Kernel.** `CONFIG_FS_ENCRYPTION` is off in V67 → a new Image, a new vermagic, and **every module rebuilt**. That breaks the "same proven Image" rule of rom-v2.
  2. **Metadata encryption** needs `dm-default-key`, which exists only in the Android Common Kernel; `system/vold/MetadataCrypt.cpp` uses only `DmTargetDefaultKey`. It must be ported (dm-default-key + blk-crypto-fallback) or skipped. FBE without metadata encryption works but leaves file sizes, names-length and layout visible.
  3. **No hardware crypto.** SDM660 has eMMC ICE, but mainline needs `BLK_INLINE_ENCRYPTION` + `MMC_CRYPTO` + `qcom-ice`. The `qcom_scm` ICE key calls are **unproven on this TZ**. So: software AES-XTS on the ARMv8 CE (acceptable speed; measure).
  4. **KeyMint and Gatekeeper are software only.** The stock device uses a QSEE keymaster TA through downstream `qseecom`. Mainline has no general `qseecom` (`QCOM_QSEECOM` is off; upstream only supports uefisecapp). Android 17 with target-level 202604 accepts only AIDL KeyMint, while the stock blob is HIDL keymaster 3/4 → **no hardware-backed keystore is realistic**. vold therefore wraps the FBE keys with **software KeyMint**. The CE keys are still bound to the lock-screen credential (synthetic password + scrypt), but there is no TEE rate limiting and no Weaver, so a short PIN can be brute-forced offline from an EDL dump. `com.android.hardware.gatekeeper.nonsecure` (added by release.mk) makes PIN/pattern work at all. The same trust level holds, and throttling is software.
  5. Software KeyMint/Gatekeeper is **not acceptable for a Google-certified release**, and "strong" or hardware attestation is impossible (5).
  6. Switching an installed unencrypted /data to FBE needs a **wipe** (the EDL kit already zeroes the start of userdata; `formattable` + fileencryption formats with `-O encrypt`).
- **Order, if Pierre wants FBE:**
  1. Kernel fragment → new Image + module rebuild.
  2. The attended boot regression of the new Image.
  3. `A6L_FSTAB=fbe` + gatekeeper.
  4. First boot: `getprop ro.crypto.state` = encrypted, `ro.crypto.type` = file.
  5. Set a PIN, reboot, check that CE storage unlocks.

## 5. Play certification
- **The build is not Play-certified** and cannot be. Register it as an uncertified custom ROM:
  1. First boot, then connect to Wi-Fi so GMS checks in. Do not sign in yet if Play shows "device is not certified".
  2. Get the **GSF Android ID**:
     - userdebug: `adb root; adb shell 'sqlite3 /data/data/com.google.android.gsf/databases/gservices.db "select value from main where name=\"android_id\""'`.
     - user build: no root and no sqlite3, so use a "Device ID" app (it shows the GSF ID) or `adb shell content query --uri content://com.google.android.gsf.gservices --where "name='android_id'"` (may need a permission shell does not have).
  3. Go to `https://www.google.com/android/uncertified`, sign in with the Google account, and paste the ID. The page takes the decimal ID; a GSF app shows both forms. There is a limit of **100 registered IDs per account**.
  4. Clear data of Google Play Store and Google Play services (and GSF), wait up to several minutes, reboot, then sign in.
  5. **Every factory reset or new install produces a new GSF ID**, which must be registered again.
- **Props:** keep the honest fingerprint, `Hisense/lineage_gsi_a6l/a6l:17/<build id>/<incr>:user/release-keys` after signing. **Do not spoof another device's fingerprint** (misrepresentation, and it breaks attestation consistency anyway). `ro.product.*` = Hisense/A6L is already set. Release builds must be `user` + `release-keys`. `ro.adb.secure=1` comes with user builds.
- **Play Integrity limits:**
  - Unlocked bootloader (ORANGE) plus software KeyMint means no hardware key attestation, so **`MEETS_STRONG_INTEGRITY` is impossible**.
  - Since Google's 2025 verdict change, **`MEETS_DEVICE_INTEGRITY` on Android 13+ needs hardware-backed signals**, so it is **not achievable** either.
  - At best **`MEETS_BASIC_INTEGRITY`**.
  - Consequences: banking apps, Google Wallet (no NFC anyway), some streaming apps (**no Widevine**: MindTheGapps ships no Widevine HAL, only clearkey DRM) and some games will refuse to run.
  - CTS profile or "certified" status in Play Store settings stays **Uncertified** unless registered. Registration removes the sign-in block but not the Integrity verdicts.

## 6. All file changes (27 Sep 2026)
| file | change | default effect |
|---|---|---|
| `tools/release/gen-release-keys.sh`, `apex-list.txt` | new | none (not run) |
| `tools/release/sign-a6l-release.sh` | new | none |
| `tools/release/make-updater-json.py` | new (tested on a synthetic zip) | none |
| `tools/release/stage-release-prebuilts.sh` | new | only with `A6L_RELEASE=1` |
| `tools/release/check-a6l-sepolicy.sh` | new, offline policy check | none |
| `device/hisense/a6l/rom/release/{release.mk, BoardConfig-release.mk, recovery/recovery.fstab, recovery/init.recovery.qcom.rc}` | new | only with `A6L_RELEASE=1` |
| `device/hisense/a6l/rom/release/0001-release-defaults.patch` | new, **not applied** (`git apply --check` OK): `persist.vendor.a6l.radio=1`, `persist.sys.usb.config=none`, `ro.hardware.egl=mesa` | none |
| `device/hisense/a6l/rom/sepolicy/vendor/*` | new ROM-level policy | only with `A6L_SEPOLICY_ROM=1` / `A6L_RELEASE=1` |
| `device/hisense/a6l/rom/sepolicy/rc-enforcing.patch` | new, **not applied** | none |
| `device/hisense/a6l/rom/vendor-etc/fstab.qcom.fbe` | new | only with `A6L_FSTAB=fbe` |
| `device/hisense/a6l/kernel/rom-v2/release.config.fragment` | new | none (no kernel build) |
| `device/hisense/a6l/BoardConfig.mk` | `ifeq ($(A6L_RELEASE),1) include rom/release/BoardConfig-release.mk` | none |
| `device/hisense/a6l/lineage_gsi_a6l.mk` | `ifeq ($(A6L_RELEASE),1)`: recovery/boot image + OTA on, inherit release.mk | none |
| `device/hisense/a6l/rom/BoardConfig-rom.mk` | rom sepolicy dir opt-in | none |
| `tools/rom-v2-pipeline.sh` | `A6L_FSTAB=fbe`, `A6L_RELEASE=1` (release prebuilts; variant user; `+ target-files-package otatools`; refuses the debug adb key) | none |
| `tools/Prepare-RomV2Boot.py` | `A6L_SELINUX=enforcing`, `A6L_FSTAB=fbe` | none (same boot.img bytes) |

## 7. Commands for later (in order)
```
# (a) userdebug build with the ROM policy, still permissive (next attended build; merge agent owns `m`)
cd /mnt/c/Users/Pierre/Desktop/A6L && git apply device/hisense/a6l/rom/sepolicy/rc-enforcing.patch
A6L_SEPOLICY_ROM=1 bash rom-v2-pipeline.sh r5 prep build boot flash qemu
# (b) after the avc pass: enforcing trial on the same build
A6L_SELINUX=enforcing bash rom-v2-pipeline.sh r5e boot
# (c) keys (Pierre, once)
A6L_KEY_SUBJECT='...' bash tools/release/gen-release-keys.sh
# (d) release build (+ patch 0001 if accepted; + A6L_FSTAB=fbe only with the FS_ENCRYPTION kernel)
git apply device/hisense/a6l/rom/release/0001-release-defaults.patch
A6L_RELEASE=1 A6L_SELINUX=enforcing bash rom-v2-pipeline.sh rel1 prep build boot
A6L_KEYS_ADD_MISSING=1 A6L_KEY_SUBJECT='...' bash tools/release/gen-release-keys.sh --apex-from $OUT/obj/PACKAGING/target_files_intermediates/lineage_gsi_a6l-target_files*.zip
bash tools/release/sign-a6l-release.sh $OUT/obj/PACKAGING/target_files_intermediates/lineage_gsi_a6l-target_files*.zip /home/a6l/rom-v2/boot-rel1 /home/a6l/release/rel1
# (e) first install of rel1 by the EDL kit (images from the signed target-files IMAGES/ + boot-rel1) + wipe; later updates = OTA + JSON (2)
# offline policy check any time:
bash tools/release/check-a6l-sepolicy.sh user rom/sepolicy/vendor
```

## 8. Blockers and decisions for Pierre
1. **Keys:** subject, password (with a password, every signing needs `ANDROID_PW_FILE`), and the offline backup location. Run 1.1 only when decided.
2. **Recovery slot:** a release OTA path needs the Lineage recovery in `recovery`, which **replaces the V74 diagnostic recovery** (the rescue and debug path used since 14 Sep). EDL stays available. Keep the V74 image in the kit.
3. **Radio default ON for release** (patch 0001). A user build has no root, so `setprop persist.vendor.a6l.radio 1` is impossible there. RF rule: Pierre's explicit go. The same applies to the charger, IPA and camera gates: decide their release defaults.
4. **ADB default** in release (`persist.sys.usb.config=none` in patch 0001) versus keeping adb on for support.
5. **EGL = Mesa statically** in release (patch 0001). There is no runtime fallback to ANGLE any more if the GPU fails to probe.
6. **FBE:** accept a new kernel Image + module rebuild (4, blocker 1) and software-only KeyMint/Gatekeeper (blocker 4)? Skip metadata encryption for now?
7. **GNSS XTRA:** confirm the HAL does not download XTRA itself (it cannot in enforcing), or move the download to the framework PSDS path.
8. **Hosting:** the JSON goes in the public repo `updater/a6l.json` on `main`, and the OTAs as GitHub Release assets (check the size is under 2 GiB).
