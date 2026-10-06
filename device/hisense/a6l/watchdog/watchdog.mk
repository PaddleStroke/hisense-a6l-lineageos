# SPDX-License-Identifier: Apache-2.0
# A6L APSS watchdog (cpufreq-watchdog agent, 29 Sep 2026; docs/cpufreq-watchdog-20260929.md). Included from rom/r6/r6.mk.
# /system/bin/watchdogd is part of the base system image; this only adds the vendor init service (opt-in property).
PRODUCT_COPY_FILES += \
    device/hisense/a6l/watchdog/init.a6l-watchdog.rc:$(TARGET_COPY_OUT_VENDOR)/etc/init/init.a6l-watchdog.rc
