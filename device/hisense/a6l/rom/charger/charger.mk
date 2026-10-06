# SPDX-License-Identifier: Apache-2.0
# A6L off-mode charging UI (selinux-release, 29 Sep 2026; docs/selinux-release-20260929.md). Inherited by rom/rom.mk.
# The UI is the AIDL health HAL's charger mode: service vendor.charger = /vendor/bin/hw/android.hardware.health-service.example
# --charger (class charger, libhealthd_charger_ui + minui on /dev/dri/card0), started by init.rc `on charger`. It reads its
# animation from /vendor/etc/res/values/charger/animation.txt + /vendor/etc/res/images/charger/*.png = LineageOS charger
# images (vendor/lineage/charger). Those already come from vendor/lineage/config/common_mobile.mk; listed here so the product
# does not depend on it. Density bucket: rom/charger/BoardConfig-charger.mk. Kernel/module side: init.qcom.rc `on charger`
# (a6l_modules_offcharge + a6l_chg_guard, H49).
PRODUCT_PACKAGES += \
    android.hardware.health-service.example \
    charger_res_images \
    lineage_charger_animation_vendor
