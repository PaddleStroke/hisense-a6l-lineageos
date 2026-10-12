# SPDX-License-Identifier: Apache-2.0
# A6L debug persistent boot log (r6b boot fix, 30 Sep 2026; docs/rom-r6b-bootfix-20260930.md). Inherited by rom/rom.mk.
# userdebug/eng ONLY: a user (release) build installs nothing from here.
ifneq ($(TARGET_BUILD_VARIANT),user)
PRODUCT_PACKAGES += a6l-media-ancillary a6l-log-ring
# restart hang (7 Oct 2026, firmware/extracted/restart-hang-20261007/krec): synchronous kmsg recorder on raw reserve2
PRODUCT_PACKAGES += a6l-krec
PRODUCT_COPY_FILES += device/hisense/a6l/rom/debug/init.a6l.krec-debug.rc:$(TARGET_COPY_OUT_VENDOR)/etc/init/init.a6l.krec-debug.rc
PRODUCT_VENDOR_PROPERTIES += ro.vendor.a6l.krec=1 ro.vendor.a6l.krec.after_bc_s=60
PRODUCT_COPY_FILES += \
    device/hisense/a6l/rom/debug/init.a6l.system-debug.rc:$(TARGET_COPY_OUT_SYSTEM_EXT)/etc/init/init.a6l.system-debug.rc \
    device/hisense/a6l/rom/debug/init.a6l.bootlog-debug.rc:$(TARGET_COPY_OUT_VENDOR)/etc/init/init.a6l.bootlog-debug.rc \
    device/hisense/a6l/rom/debug/a6l-bootlog.sh:$(TARGET_COPY_OUT_VENDOR)/bin/a6l-bootlog.sh \
    device/hisense/a6l/rom/debug/a6l-pm-trace.sh:$(TARGET_COPY_OUT_VENDOR)/bin/a6l-pm-trace.sh \
    device/hisense/a6l/rom/debug/a6l-iowatch.sh:$(TARGET_COPY_OUT_VENDOR)/bin/a6l-iowatch.sh \
    device/hisense/a6l/rom/debug/init.a6l.gpu-coredump-debug.rc:$(TARGET_COPY_OUT_VENDOR)/etc/init/init.a6l.gpu-coredump-debug.rc \
    device/hisense/a6l/rom/debug/a6l-gpu-coredump.sh:$(TARGET_COPY_OUT_VENDOR)/bin/a6l-gpu-coredump.sh
# r6e: logd also reads the kernel log (`adb logcat -b kernel`)
PRODUCT_VENDOR_PROPERTIES += ro.logd.kernel=true
# Retain bounded tails through attended camera/EPD/sleep tests even if USB drops.
# This extends duration only; the metadata log cap stays 3 MiB with 1 MiB free.
PRODUCT_VENDOR_PROPERTIES += ro.vendor.a6l.bootlog.after_bc_s=0
# r6j diagnostic trial: hold only GPU runtime PM; attended property=0 restores auto.
# (a user build keeps the same GPU runtime-PM behaviour without the su helpers: rom/user/user.mk)
PRODUCT_VENDOR_PROPERTIES += persist.vendor.a6l.gpu.pm_hold=1
# incident-20261012-1203 (round 28): PMIC reset-reason capture at every boot (read-only; kmsg at `on fs`, full capture in
# /data/vendor/a6l-pon/boot-NNNN.txt; optional power trace with persist.vendor.a6l.pon.watch=1).
# user-build-20261012: moved INSIDE the variant guard (it was installed in every variant). Its three services run as su
# and mount/read debugfs: su does not exist in a user policy (init cannot start them) and debugfs is neverallowed for
# vendor domains there. A user build has no PON capture (firmware/extracted/user-build-20261012/README.md, risks).
PRODUCT_COPY_FILES += \
    device/hisense/a6l/rom/debug/init.a6l.pon-debug.rc:$(TARGET_COPY_OUT_VENDOR)/etc/init/init.a6l.pon-debug.rc \
    device/hisense/a6l/rom/debug/a6l-pon-capture.sh:$(TARGET_COPY_OUT_VENDOR)/bin/a6l-pon-capture.sh
endif
# Physical A6L has no AVF guest relay / AF_VSOCK: keep local Perfetto working. Not a debug path (main init, no su, one
# setprop of a system_ext property), so user-build-20261012 moved it out of the variant guard: every variant installs it.
PRODUCT_COPY_FILES += \
    device/hisense/a6l/rom/debug/init.a6l.perfetto-debug.rc:$(TARGET_COPY_OUT_SYSTEM_EXT)/etc/init/init.a6l.perfetto-debug.rc
