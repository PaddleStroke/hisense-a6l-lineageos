# SPDX-License-Identifier: Apache-2.0
# A6L rom-v1 "full" variant (agent flash, 24 Sep 2026) = base + the parts of the other agents' deliveries that cannot
# break the boot: audio3 (routing daemon, mixer paths, policy, jack overlay; patched q6asm-dai.ko is in the prebuilts),
# hals (Wi-Fi HAL + wpa_supplicant, Bluetooth HCI HAL, lights, power/thermal examples, a6l_macs). NOT included (see
# docs/flash-20260924.md): hals radio rc (replaced by rom a6l-radio.sh, real EFS partitions), hals USB gadget HAL
# (conflicts with the a6l_manual_usb gate), sensors multihal (IIO names unverified), eink3 (needs the composer patch).
# Enabled by tools/rom-v1-pipeline.sh <tag> ... full (copies this dir to device/hisense/a6l/rom/variant/).

# --- audio3 ---
PRODUCT_PACKAGES += \
    a6l-audio-route \
    mixer_paths_a6l.xml
DEVICE_PACKAGE_OVERLAYS += device/hisense/a6l/audio/overlay

# --- hals subset ---
PRODUCT_PACKAGES += \
    android.hardware.wifi-service \
    wpa_supplicant \
    hostapd \
    wificond \
    iw \
    android.hardware.bluetooth-service.default \
    android.hardware.light-service.lineage \
    android.hardware.power-service.example \
    android.hardware.thermal-service.example
$(call soong_config_set,wpa_supplicant,platform_version,$(PLATFORM_VERSION))
$(call soong_config_set,wpa_supplicant,nl80211_driver,CONFIG_DRIVER_NL80211_QCA)

PRODUCT_COPY_FILES += \
    device/hisense/a6l/rom/variant/init.a6l.wifibt.rc:$(TARGET_COPY_OUT_VENDOR)/etc/init/init.a6l.wifibt.rc \
    device/hisense/a6l/hals/wifi/wpa_supplicant.conf:$(TARGET_COPY_OUT_VENDOR)/etc/wifi/wpa_supplicant.conf \
    device/hisense/a6l/hals/wifi/wpa_supplicant_overlay.conf:$(TARGET_COPY_OUT_VENDOR)/etc/wifi/wpa_supplicant_overlay.conf \
    device/hisense/a6l/hals/wifi/p2p_supplicant.conf:$(TARGET_COPY_OUT_VENDOR)/etc/wifi/p2p_supplicant.conf \
    device/hisense/a6l/hals/wifi/p2p_supplicant_overlay.conf:$(TARGET_COPY_OUT_VENDOR)/etc/wifi/p2p_supplicant_overlay.conf \
    frameworks/native/data/etc/android.hardware.wifi.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.wifi.xml \
    frameworks/native/data/etc/android.hardware.bluetooth.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.bluetooth.xml \
    frameworks/native/data/etc/android.hardware.bluetooth_le.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.bluetooth_le.xml

PRODUCT_VENDOR_PROPERTIES += \
    wifi.interface=wlan0 \
    ro.vendor.a6l.rom.variant=full
