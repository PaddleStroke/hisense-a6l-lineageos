# SPDX-License-Identifier: Apache-2.0
# Hisense A6L LCD <-> e-ink switcher (agent dualux, docs/dualux-20260925.md). For the `flash` agent:
#   1. $(call inherit-product, device/hisense/a6l/eink/switcher/dualux.mk)   (after eink/eink.mk)
#   2. BoardConfig: BOARD_VENDOR_SEPOLICY_DIRS += device/hisense/a6l/eink/switcher/sepolicy/vendor
#                   SYSTEM_EXT_PUBLIC_SEPOLICY_DIRS += device/hisense/a6l/eink/switcher/sepolicy/system_ext/public
#                   SYSTEM_EXT_PRIVATE_SEPOLICY_DIRS += device/hisense/a6l/eink/switcher/sepolicy/system_ext/private
#   3. optional, recommended: patches/external/drm_hwcomposer/0002-*.patch (no LCD flash when Android wakes on the e-ink)
#   4. kernel/DT: a6l-eink-frontlight-v75.dtso + leds-qcom-lpg.ko + leds-pwm.ko (CONFIG_LEDS_PWM is off in v67: module
#      built by dualux into firmware/extracted/dualux-20260925/)

PRODUCT_PACKAGES += \
    a6l_dualux \
    A6LDisplaySwitcher \
    privapp_whitelist_org.lineageos.a6l.dualux

PRODUCT_COPY_FILES += \
    device/hisense/a6l/eink/switcher/config/a6l-dualux-keys.idc:$(TARGET_COPY_OUT_VENDOR)/usr/idc/a6l-dualux-keys.idc \
    device/hisense/a6l/eink/switcher/config/Vendor_2a6c_Product_0d0a.idc:$(TARGET_COPY_OUT_VENDOR)/usr/idc/Vendor_2a6c_Product_0d0a.idc

# Settings (persist.sys.a6l.*) need no defaults: unset = auto refresh, clear every 10, contrast 0, e-ink key = sleep,
# frontlight on at 100 % cap (code defaults in a6l_dualux / a6l_eink_mirror / the app).
