# SPDX-License-Identifier: Apache-2.0
# Hisense A6L rear e-ink for the installed ROM (agent eink3, docs/eink3-20260924.md). For the `flash` agent:
#   1. device/hisense/a6l/eink/extract-eink-blobs.sh            (copies libtcon_eink.so + waveform into proprietary/)
#   2. device/hisense/a6l/eink/patches/apply-patches.sh <tree>   (drm_hwcomposer: ignore + lease the e-ink connector)
#   3. $(call inherit-product, device/hisense/a6l/eink/eink.mk)  in lineage_gsi_a6l.mk
#   4. BoardConfig.mk: BOARD_VENDOR_SEPOLICY_DIRS += device/hisense/a6l/eink/sepolicy/vendor
#   5. vendor ueventd.rc: include config/ueventd.eink.rc (A6L_EINK_SHIP_UEVENTD := true below ships it as the whole file)

PRODUCT_PACKAGES += \
    a6l_epdd \
    a6l_eink_mirror \
    libtcon_eink \
    a6l_epd_waveform_fallback

PRODUCT_COPY_FILES += \
    device/hisense/a6l/eink/config/A6L_side_keys.kl:$(TARGET_COPY_OUT_VENDOR)/usr/keylayout/A6L_side_keys.kl \
    device/hisense/a6l/eink/config/a6l-eink-rear-touch.idc:$(TARGET_COPY_OUT_VENDOR)/usr/idc/a6l-eink-rear-touch.idc

# Only when no other part of the device ships /vendor/etc/ueventd.rc; otherwise append config/ueventd.eink.rc to it.
ifeq ($(A6L_EINK_SHIP_UEVENTD),true)
PRODUCT_COPY_FILES += device/hisense/a6l/eink/config/ueventd.eink.rc:$(TARGET_COPY_OUT_VENDOR)/etc/ueventd.rc
endif

# drm_hwcomposer (patched): the e-ink connector (only connector with a 384x725 mode) never becomes a display and is
# leased to vendor.a6l_epdd over the abstract socket @a6l.hwc.lease.
PRODUCT_VENDOR_PROPERTIES += \
    vendor.hwc.drm.ignore_connectors=mode:384x725 \
    vendor.hwc.drm.lease_socket=a6l.hwc.lease \
    persist.vendor.eink.mode=off \
    persist.vendor.eink.reading=0
