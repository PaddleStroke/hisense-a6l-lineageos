# SPDX-License-Identifier: Apache-2.0
# A6L SELinux enforcing readiness, product side (selinux-release, 29 Sep 2026). Inherited by rom/rom.mk; inert unless
# A6L_SELINUX_PREP=1 or A6L_RELEASE=1 (board side and the tree check: BoardConfig-selinux.mk).
ifneq ($(filter 1,$(A6L_SELINUX_PREP) $(A6L_RELEASE)),)
# debug logcat -> kmsg stream (androidboot.a6l_logcat=1), removed from init.qcom.rc by the prep transform: su domain,
# so userdebug/eng only
ifneq ($(TARGET_BUILD_VARIANT),user)
PRODUCT_COPY_FILES += \
    device/hisense/a6l/rom/selinux/init.a6l.logcat-debug.rc:$(TARGET_COPY_OUT_VENDOR)/etc/init/init.a6l.logcat-debug.rc
endif
PRODUCT_VENDOR_PROPERTIES += \
    ro.vendor.a6l.selinux_prep=1
endif
