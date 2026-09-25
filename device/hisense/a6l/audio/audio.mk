# audio3 (24 Sep 2026): Hisense A6L real-card audio. $(call inherit-product, device/hisense/a6l/audio/audio.mk)
# from lineage_gsi_a6l.mk, and REMOVE the realinit audio_policy_configuration.xml line from PRODUCT_COPY_FILES there
# (this file installs the new one to the same path). Keeps the AOSP AIDL example HAL + effects already in the product.
PRODUCT_PACKAGES += \
    a6l-audio-route \
    mixer_paths_a6l.xml

PRODUCT_COPY_FILES += \
    device/hisense/a6l/audio/audio_policy_configuration.xml:$(TARGET_COPY_OUT_VENDOR)/etc/audio_policy_configuration.xml

# Wired headset detection through the ASoC jack input device
DEVICE_PACKAGE_OVERLAYS += device/hisense/a6l/audio/overlay

# sepolicy for the routing daemon: add to BoardConfig.mk (board variable):
#   BOARD_VENDOR_SEPOLICY_DIRS += device/hisense/a6l/audio/sepolicy
