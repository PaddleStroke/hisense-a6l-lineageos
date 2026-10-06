# SPDX-License-Identifier: Apache-2.0
# A6L RELEASE product additions (release-prep, 27 Sep 2026; docs/release-prep-20260927.md). Inherited from
# lineage_gsi_a6l.mk ONLY when the build environment has A6L_RELEASE=1 (tools/rom-v2-pipeline.sh A6L_RELEASE=1 ...).
# Default builds (r1-r3 and any r4 made the old way) are unchanged.
# NOTE: PRODUCT_*_PROPERTIES keep the FIRST definition of a key; values already set by rom.mk / a6l-software-graphics.mk
# (persist.vendor.a6l.radio=0, ro.hardware.egl=angle) can NOT be overridden here -> rom/release/0001-release-defaults.patch.

# --- LockSettings needs a Gatekeeper HAL: r3 has NONE (gatekeeperd: "Could not find Gatekeeper device") -> no PIN/pattern,
# no credential-bound CE keys once FBE is on. Software Gatekeeper (same trust level as KeyMint nonsecure: no TEE).
PRODUCT_PACKAGES += \
    com.android.hardware.gatekeeper.nonsecure

# --- LineageOS Updater: self-hosted JSON (Lineage 24 v2 format, list of builds) on the public repo's raw URL.
# raw.githubusercontent.com is used because the Updater's JSON fetch does NOT follow redirects (OkHttp followRedirects(false))
# while github.com/.../releases/latest/download/... is a 302. The OTA zip itself can be a GitHub Release asset (the
# download client follows redirects). {device} = ro.lineage.device = a6l.
PRODUCT_SYSTEM_EXT_PROPERTIES += \
    lineage.updater.uri=https://raw.githubusercontent.com/PaddleStroke/hisense-a6l-lineageos/main/updater/{device}.json

# --- installable Lineage recovery (non-A/B OTA path: Updater -> RecoverySystem.installPackage -> uncrypt -> BCB -> recovery)
PRODUCT_COPY_FILES += \
    device/hisense/a6l/rom/release/recovery/init.recovery.qcom.rc:$(TARGET_COPY_OUT_RECOVERY)/root/init.recovery.qcom.rc
