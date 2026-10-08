# A6L DEBUG VARIANT ONLY (merge2, 25 Sep 2026; Astra R14): pre-trust the laptop's adb key at post-fs-data.
# Included by rom/variant/variant.mk ONLY when tools/rom-v2-pipeline.sh runs with A6L_DEBUG_ADBKEY=1. Never in a release.
# The key file is staged by tools/stage-rom-v2-prebuilts.sh into rom/debug/adb_keys (outside rom-files.mk).
PRODUCT_COPY_FILES += device/hisense/a6l/rom/debug/adb_keys:$(TARGET_COPY_OUT_VENDOR)/etc/a6l/adb_keys
# selinux-20261007 pass 2: the copy into /data/misc/adb runs from system_ext (init), not from a vendor rc
PRODUCT_COPY_FILES += device/hisense/a6l/rom/debug/init.a6l.adbkey-debug.rc:$(TARGET_COPY_OUT_SYSTEM_EXT)/etc/init/init.a6l.adbkey-debug.rc
PRODUCT_VENDOR_PROPERTIES += ro.vendor.a6l.debug.adbkey=1
