# Hisense A6L HAL/service layer on the mainline kernel (agent hals, 24 Sep 2026).
# inherit from lineage_gsi_a6l.mk:  $(call inherit-product, device/hisense/a6l/hals/hals.mk)
# Requires external/linux-msm/{qrtr,rmtfs,tqftpserv} in the tree (tools/build-radio-userspace.sh copies them).
PRODUCT_SOONG_NAMESPACES += device/hisense/a6l/hals

# --- modem companions (RF gate: persist.vendor.a6l.radio.enable=1) ---
PRODUCT_PACKAGES += \
    rmtfs \
    tqftpserv \
    qrtr-lookup \
    a6l-diag-router \
    a6l-modem.sh \
    a6l-sensors.sh \
    a6l_macs

# --- Wi-Fi ---
PRODUCT_PACKAGES += \
    android.hardware.wifi-service \
    wpa_supplicant \
    hostapd \
    wificond \
    iw
$(call soong_config_set,wpa_supplicant,platform_version,$(PLATFORM_VERSION))
$(call soong_config_set,wpa_supplicant,nl80211_driver,CONFIG_DRIVER_NL80211_QCA)

# --- Bluetooth (HCI_CHANNEL_USER on hci0, WCN3990 via serdev hci_uart QCA) ---
PRODUCT_PACKAGES += \
    android.hardware.bluetooth-service.default

# --- sensors: AIDL multihal + IIO sub-HAL (device/google/trout, see doc) ---
PRODUCT_PACKAGES += \
    android.hardware.sensors-service.multihal \
    android.hardware.sensors@2.1-Google-IIO-Subhal

# --- lights, health, power, thermal, USB ---
PRODUCT_PACKAGES += \
    android.hardware.light-service.lineage \
    android.hardware.health-service.example \
    android.hardware.power-service.example \
    android.hardware.thermal-service.example \
    android.hardware.usb-service.basic \
    android.hardware.usb.gadget-service.a6l

PRODUCT_COPY_FILES += \
    device/hisense/a6l/hals/rootdir/etc/init/a6l-radio.rc:$(TARGET_COPY_OUT_VENDOR)/etc/init/a6l-radio.rc \
    device/hisense/a6l/hals/rootdir/etc/init/a6l-wifi-bt.rc:$(TARGET_COPY_OUT_VENDOR)/etc/init/a6l-wifi-bt.rc \
    device/hisense/a6l/hals/rootdir/etc/init/a6l-usb.rc:$(TARGET_COPY_OUT_VENDOR)/etc/init/a6l-usb.rc \
    device/hisense/a6l/hals/rootdir/etc/init/a6l-sensors-misc.rc:$(TARGET_COPY_OUT_VENDOR)/etc/init/a6l-sensors-misc.rc \
    device/hisense/a6l/hals/wifi/wpa_supplicant.conf:$(TARGET_COPY_OUT_VENDOR)/etc/wifi/wpa_supplicant.conf \
    device/hisense/a6l/hals/wifi/wpa_supplicant_overlay.conf:$(TARGET_COPY_OUT_VENDOR)/etc/wifi/wpa_supplicant_overlay.conf \
    device/hisense/a6l/hals/wifi/p2p_supplicant.conf:$(TARGET_COPY_OUT_VENDOR)/etc/wifi/p2p_supplicant.conf \
    device/hisense/a6l/hals/wifi/p2p_supplicant_overlay.conf:$(TARGET_COPY_OUT_VENDOR)/etc/wifi/p2p_supplicant_overlay.conf \
    device/hisense/a6l/hals/sensors/hals.conf:$(TARGET_COPY_OUT_VENDOR)/etc/sensors/hals.conf \
    device/hisense/a6l/hals/sensors/sensor_hal_configuration.xml:$(TARGET_COPY_OUT_VENDOR)/etc/sensors/sensor_hal_configuration.xml \
    frameworks/native/data/etc/android.hardware.wifi.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.wifi.xml \
    frameworks/native/data/etc/android.hardware.bluetooth.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.bluetooth.xml \
    frameworks/native/data/etc/android.hardware.bluetooth_le.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.bluetooth_le.xml \
    frameworks/native/data/etc/android.hardware.sensor.accelerometer.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.sensor.accelerometer.xml \
    frameworks/native/data/etc/android.hardware.sensor.gyroscope.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.sensor.gyroscope.xml \
    frameworks/native/data/etc/android.hardware.sensor.compass.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.sensor.compass.xml \
    frameworks/native/data/etc/android.hardware.sensor.light.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.sensor.light.xml \
    frameworks/native/data/etc/android.hardware.sensor.proximity.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.sensor.proximity.xml \
    frameworks/native/data/etc/android.hardware.usb.accessory.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.usb.accessory.xml

PRODUCT_VENDOR_PROPERTIES += \
    wifi.interface=wlan0 \
    ro.vendor.a6l.hals=20260924
# NOT declared on purpose: android.hardware.wifi.direct (P2P untested), telephony.*, location.gps, camera, fingerprint,
# android.hardware.vibrator (no working vibrator HAL yet).
