# SPDX-License-Identifier: Apache-2.0
# A6L (lc2, 29 Sep 2026): libcamera (upstream v0.7.2 + A6L patches 0001-0007) for the ROM: prebuilt ELF modules (Android.bp here)
# + non-ELF data. Inherited by device/hisense/a6l/camera/camera.mk.
LIBCAMERA_A6L_DIR := device/hisense/a6l/camera/libcamera

PRODUCT_PACKAGES += \
    libcamera-base.a6l \
    libcamera.a6l \
    ipa_soft_simple.a6l \
    camera.libcamera.a6l \
    soft_ipa_proxy.a6l

# IPA module signature (checked by IPAManager against the key compiled into libcamera.so -> IPA runs in-process), tuning
# files (IPA config path /vendor/share/libcamera/ipa/simple/<sensor model>.yaml) and the HAL configuration (rotation/location).
PRODUCT_COPY_FILES += \
    $(LIBCAMERA_A6L_DIR)/prebuilt/lib64/libcamera/ipa/ipa_soft_simple.so.sign:$(TARGET_COPY_OUT_VENDOR)/lib64/libcamera/ipa/ipa_soft_simple.so.sign \
    $(LIBCAMERA_A6L_DIR)/prebuilt/share/libcamera/ipa/simple/hi846.yaml:$(TARGET_COPY_OUT_VENDOR)/share/libcamera/ipa/simple/hi846.yaml \
    $(LIBCAMERA_A6L_DIR)/prebuilt/share/libcamera/ipa/simple/imx576_a6l.yaml:$(TARGET_COPY_OUT_VENDOR)/share/libcamera/ipa/simple/imx576_a6l.yaml \
    $(LIBCAMERA_A6L_DIR)/prebuilt/share/libcamera/ipa/simple/s5k3t1.yaml:$(TARGET_COPY_OUT_VENDOR)/share/libcamera/ipa/simple/s5k3t1.yaml \
    $(LIBCAMERA_A6L_DIR)/prebuilt/share/libcamera/ipa/simple/uncalibrated.yaml:$(TARGET_COPY_OUT_VENDOR)/share/libcamera/ipa/simple/uncalibrated.yaml \
    device/hisense/a6l/camera/camera_hal.yaml:$(TARGET_COPY_OUT_VENDOR)/etc/libcamera/camera_hal.yaml

# hw_get_module("camera") -> /vendor/lib64/hw/camera.libcamera.so
PRODUCT_VENDOR_PROPERTIES += \
    ro.hardware.camera=libcamera
