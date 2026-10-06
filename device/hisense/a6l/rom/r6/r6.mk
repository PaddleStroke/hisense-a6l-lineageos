# SPDX-License-Identifier: Apache-2.0
# A6L r6 product additions (completeness audit, 29 Sep 2026; docs/completeness-audit-20260929.md). Prepared OFFLINE while
# r5 was building; NOT active until rom/r6/apply-r6.sh --apply is run by the merge worker (adds the inherit line to rom.mk and
# rom/r6/sepolicy/vendor to the board policy dirs). Nothing here was built with `m`.
R6_DIR := device/hisense/a6l/rom/r6

# --- lock screen: LockSettings needs a Gatekeeper HAL (r3 log: "Could not find Gatekeeper device" -> no PIN/pattern, and
# no credential-bound CE keys once FBE is on). Was only in release.mk (A6L_RELEASE=1); now in every build. Software
# Gatekeeper, same trust level as the nonsecure KeyMint (no TEE on this port). Duplicate with release.mk is harmless.
PRODUCT_PACKAGES += \
    com.android.hardware.gatekeeper.nonsecure

# --- thermal: real temperatures (tsens zones + fuel-gauge battery temp) with throttling severities and a battery thermal
# shutdown at 60 degC, replacing "no thermal HAL" (the example HAL, fake 30 degC values, was never in rom.mk).
PRODUCT_SOONG_NAMESPACES += $(R6_DIR)/thermal
PRODUCT_PACKAGES += \
    android.hardware.thermal-service.a6l

# --- framework overlay: auto-brightness from the front STK3338, VoLTE/VT/WFC unavailable, no notification LED, power_profile.xml (3800 mAh).
DEVICE_PACKAGE_OVERLAYS += $(R6_DIR)/overlay

# Bluetooth audio (A2DP software encoding, HFP) and USB device mode are NOT here: audio/bluetooth/bt-audio.mk (android-bt-audio)
# and device/hisense/a6l/usb (android-usb) were prepared in parallel on 29 Sep.

# --- on-phone test helpers (not a daemon; run from adb shell as root): suspend/wake/drain checks for H53/H54/H62
PRODUCT_COPY_FILES += \
    $(R6_DIR)/tools/a6l-suspend-check.sh:$(TARGET_COPY_OUT_VENDOR)/bin/a6l-suspend-check.sh

# --- APSS hardware watchdog (cpufreq-watchdog 29 Sep; H64): vendor watchdogd service, opt-in persist.vendor.a6l.watchdog=1
$(call inherit-product, device/hisense/a6l/watchdog/watchdog.mk)
