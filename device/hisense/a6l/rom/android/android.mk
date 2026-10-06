# SPDX-License-Identifier: Apache-2.0
# Hisense A6L android-side product config (overlays-carrier-updater, 29 Sep 2026; docs/android-overlays-carrier-updater-20260929.md).
# Inherited by rom/rom.mk. Framework overlay keys are split by owner; tests/check-android-overlays.sh fails on a key defined
# in two overlay dirs (rom/android, rom/r6, audio, usb).
A6L_ANDROID_DIR := device/hisense/a6l/rom/android

# --- framework-res (static overlay -> auto-generated vendor RRO): navigation bar, display cutout (stock notch), Doze,
# no AOD, brightness setting range (stock). Auto-brightness curve + power_profile (3800 mAh): rom/r6/overlay.
DEVICE_PACKAGE_OVERLAYS += $(A6L_ANDROID_DIR)/overlay

# --- RROs (rom/android/Android.bp): Wi-Fi APEX resources (5 GHz), CarrierConfig vendor.xml (no IMS), SettingsLib
# charging-speed thresholds in SystemUI and Settings
PRODUCT_PACKAGES += \
    A6LWifiOverlay \
    A6LCarrierConfigOverlay \
    A6LSystemUIOverlay \
    A6LSettingsOverlay

# --- APN list: Lineage's world list (vendor/apn, package apns-conf.xml from vendor/lineage/config/telephony.mk) with the
# Orange France IPv4v6 fix applied to the TREE by tools/rom-v2-pipeline.sh prep (rom/android/patches/vendor/apn).

# --- LineageOS Updater: release builds (A6L_RELEASE=1) get the real JSON URI from rom/release/release.mk. Every other
# build (test keys, userdebug) points at a placeholder that is deliberately NOT published, so a test-key image is never
# offered a release-key OTA (recovery would refuse it anyway) and never polls download.lineageos.org for "a6l".
# rom r6 build (29 Sep 2026): the soong build.prop check limits a property value to 91 bytes; the old placeholder
# (raw.githubusercontent.com/.../updater/unpublished/{device}.json, 107 bytes) broke the build. Reserved .invalid host
# (RFC 2606: never resolves) = never published by construction.
ifneq ($(A6L_RELEASE),1)
PRODUCT_SYSTEM_EXT_PROPERTIES += \
    lineage.updater.uri=https://updater.invalid/a6l/updater/unpublished/{device}.json
endif
