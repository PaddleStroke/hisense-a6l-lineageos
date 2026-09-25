# SPDX-License-Identifier: Apache-2.0
# A6L rom-v1 (agent flash, 24 Sep 2026): installable-ROM vendor additions on top of the realinit vendor image.
# Inherited from lineage_gsi_a6l.mk. Prebuilt payloads (kernel modules, firmware, radio/e-ink daemons, Mesa) are staged
# into device/hisense/a6l/rom/prebuilt by tools/stage-rom-v1-prebuilts.sh; rom-files.mk (generated) lists them.
ROM_DIR := device/hisense/a6l/rom

PRODUCT_COPY_FILES += \
    $(ROM_DIR)/vendor-etc/fstab.qcom:$(TARGET_COPY_OUT_VENDOR)/etc/fstab.qcom \
    $(ROM_DIR)/vendor-etc/ueventd.rc:$(TARGET_COPY_OUT_VENDOR)/etc/ueventd.rc \
    $(ROM_DIR)/init/init.qcom.rc:$(TARGET_COPY_OUT_VENDOR)/etc/init/hw/init.qcom.rc \
    $(ROM_DIR)/init/init.a6l.usb.rc:$(TARGET_COPY_OUT_VENDOR)/etc/init/hw/init.a6l.usb.rc \
    $(ROM_DIR)/bin/a6l-modules.sh:$(TARGET_COPY_OUT_VENDOR)/bin/a6l-modules.sh \
    $(ROM_DIR)/bin/a6l-radio.sh:$(TARGET_COPY_OUT_VENDOR)/bin/a6l-radio.sh \
    $(ROM_DIR)/bin/a6l-logcat.sh:$(TARGET_COPY_OUT_VENDOR)/bin/a6l-logcat.sh

-include $(ROM_DIR)/rom-files.mk

# Graphics on the phone: msm DPU (card0) + Adreno 512 via Mesa freedreno (proven 21 Sep, V71 LineageOS-from-RAM run),
# selected at boot by a6l-modules.sh (persist.graphics.egl=mesa when renderD128 exists); ro.hardware.egl=angle remains
# the software fallback (and the QEMU test path).
PRODUCT_VENDOR_PROPERTIES += \
    debug.renderengine.backend=skiaglthreaded \
    debug.hwui.renderer=skiagl \
    ro.sf.lcd_density=400 \
    ro.surface_flinger.default_composition_pixel_format=5 \
    service.sf.prime_shader_cache=false \
    ro.vendor.a6l.rom=v1 \
    persist.vendor.a6l.radio=0 \
    persist.sys.usb.config=adb

# GNSS (agent gnss, 24 Sep 2026): AIDL GNSS HAL over the modem's QMI LOC service + a6l_gnss_test (device/hisense/a6l/gnss,
# copied into the tree by tools/rom-v1-pipeline.sh). Idle until the radio is enabled (waits for the LOC service).
$(call inherit-product-if-exists, device/hisense/a6l/gnss/gnss.mk)

# ART heap sizes (agent gnss, 24 Sep 2026). Without a dalvik-heap config the GSI product runs every VM - system_server
# included - with the 16 MiB default growth limit: QEMU r6 system_server died with OutOfMemoryError ("target footprint
# 16777216, growth limit 16777216") ~16 min into the first boot. The A6L has 4 GiB of RAM.
$(call inherit-product, frameworks/native/build/phone-xhdpi-4096-dalvik-heap.mk)
