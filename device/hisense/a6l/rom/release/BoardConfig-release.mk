# SPDX-License-Identifier: Apache-2.0
# A6L RELEASE board flags (release-prep, 27 Sep 2026). Included from BoardConfig.mk only with A6L_RELEASE=1.
# UNTESTED (no build was run): first release build = `A6L_RELEASE=1 bash tools/rom-v2-pipeline.sh <tag> prep build boot`
# with the build phase running `m target-files-package otatools` (pipeline A6L_RELEASE branch).
#
# Boot chain facts (docs/flash-20260924.md 1): A-only, fixed GPT, no super; ABL = Qualcomm LinuxLoader, boot header v1,
# Image.gz + appended DTB, vbmeta flags=2 + unlocked -> AVB result 5 accepted, nothing is verified. Normal boot reads the
# dtbo PARTITION; the recovery path uses the recovery_dtbo embedded in recovery.img (as the V74 diagnostic recovery).

# --- kernel for the build-made boot/recovery images: the proven V67 Image + V75 DTB (Image.gz-dtb staged by
# tools/release/stage-release-prebuilts.sh from the pipeline's boot-<tag> output). The ROM's real boot.img stays the
# tools/Prepare-RomV2Boot.py one (injected into the target-files by tools/release/sign-a6l-release.sh).
TARGET_NO_KERNEL := false
TARGET_PREBUILT_KERNEL := device/hisense/a6l/rom/release/prebuilt/Image.gz-dtb
TARGET_FORCE_PREBUILT_KERNEL := true
BOARD_BOOT_HEADER_VERSION := 1
BOARD_KERNEL_BASE := 0x00000000
BOARD_KERNEL_PAGESIZE := 4096
BOARD_MKBOOTIMG_ARGS += --header_version 1 --kernel_offset 0x00008000 --ramdisk_offset 0x01000000 \
    --second_offset 0x00f00000 --tags_offset 0x00000100
# = rom-v2 boot cmdline WITHOUT androidboot.selinux=permissive (user builds are enforcing whatever the cmdline says)
BOARD_KERNEL_CMDLINE := console=ttyMSM0,115200n8 earlycon=a6lfb keep_bootcon androidboot.hardware=qcom loglevel=6 \
    clk_ignore_unused pd_ignore_unused regulator_ignore_unused panic=0 a6l_probe=1 a6l_manual_usb=1 \
    androidboot.boot_devices=soc@0/c0c4000.mmc firmware_class.path=/vendor/firmware printk.devkmsg=on
BOARD_BOOTIMAGE_PARTITION_SIZE := 67108864
BOARD_RECOVERYIMAGE_PARTITION_SIZE := 67108864
BOARD_DTBOIMG_PARTITION_SIZE := 8388608
BOARD_CACHEIMAGE_PARTITION_SIZE :=

# --- recovery: full Lineage recovery in the recovery partition (replaces the V74 diagnostic recovery when installed!)
TARGET_NO_RECOVERY := false
BOARD_USES_RECOVERY_AS_BOOT := false
BOARD_USES_FULL_RECOVERY_IMAGE := true
BOARD_INCLUDE_RECOVERY_DTBO := true
BOARD_PREBUILT_DTBOIMAGE := device/hisense/a6l/rom/release/prebuilt/dtbo.img
TARGET_RECOVERY_FSTAB := device/hisense/a6l/rom/release/recovery/recovery.fstab
TARGET_RECOVERY_PIXEL_FORMAT := RGBX_8888
TARGET_USERIMAGES_USE_EXT4 := true
# eMMC (sdhci-msm is a module in V67), splash-framebuffer display for minui (a6l_simplefb -> simpledrm), touch
BOARD_RECOVERY_KERNEL_MODULES := $(wildcard device/hisense/a6l/rom/release/prebuilt/recovery-modules/*.ko)
BOARD_RECOVERY_KERNEL_MODULES_LOAD := $(BOARD_RECOVERY_KERNEL_MODULES)

# --- OTA: non-A/B full block OTA, installed by the recovery above
AB_OTA_UPDATER := false
TARGET_OTA_ASSERT_DEVICE := a6l
# No AVB: the stock vbmeta (flags=2) disables verification and the unlocked ABL cannot enforce a custom key; an AVB
# vbmeta built with our key would only be cosmetic (and the GSI config would sign with the AOSP test keys otherwise).
BOARD_AVB_ENABLE := false
