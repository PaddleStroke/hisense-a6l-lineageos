# SPDX-License-Identifier: Apache-2.0
# A6L camera (lc2, 29 Sep 2026; docs/libcamera-plan-20260929.md section 10): libcamera v0.7.2 (simple pipeline + SoftISP,
# signed simple IPA in-process) + its Android camera3 HAL module, loaded by the A6L fork of the Lineage AIDL camera provider
# (camera/provider: + torch strength levels on /sys/class/leds/white:flash). Inherited by rom/rom.mk.
# Kernel side: rom/modules/camera.txt (qcom-camss rom1, a6l_wm=3 zero-copy default), loaded by a6l-modules.sh misc when
# persist.vendor.a6l.camera=1; the provider starts after that group (its rc). SELinux: camera/sepolicy/vendor.
$(call inherit-product, device/hisense/a6l/camera/libcamera/libcamera-vendor.mk)

PRODUCT_PACKAGES += \
    android.hardware.camera.provider-service.a6l

# Features: camera, camera.any, camera.front, camera.flash. NO camera.autofocus (fixed focus until AF, milestone M5).
PRODUCT_COPY_FILES += \
    device/hisense/a6l/camera/a6l-camera-features.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/a6l-camera-features.xml \
    device/hisense/a6l/camera/media_profiles_V1_0.xml:$(TARGET_COPY_OUT_VENDOR)/etc/media_profiles_V1_0.xml
