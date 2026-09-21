# SPDX-License-Identifier: Apache-2.0
# Deliberately reuse the upstream LineageOS 24 GSI architecture while assessing
# the A6L's Android 9 vendor compatibility. This is not a deployable native port.
$(call inherit-product, vendor/lineage/build/target/product/lineage_gsi_arm64.mk)

PRODUCT_NAME := lineage_gsi_a6l
PRODUCT_DEVICE := a6l
PRODUCT_BRAND := Hisense
PRODUCT_MANUFACTURER := Hisense
PRODUCT_MODEL := A6L bringup probe

# The real device has fixed GPT partitions and no super partition. Do not make
# unrelated images or an OTA. Preserve upstream GSI shipping API semantics;
# the actual stock shipping API is recorded separately in hardware.json.
PRODUCT_USE_DYNAMIC_PARTITIONS := false
PRODUCT_USE_DYNAMIC_PARTITION_SIZE := false
PRODUCT_BUILD_BOOT_IMAGE := false
PRODUCT_BUILD_RECOVERY_IMAGE := false
# 21 Sep 2026: real-init flow needs /vendor/etc/selinux (split policy) and a labeled vendor image
PRODUCT_BUILD_VENDOR_IMAGE := true
PRODUCT_BUILD_USERDATA_IMAGE := false
PRODUCT_BUILD_SUPER_PARTITION := false
PRODUCT_BUILD_SUPER_EMPTY_IMAGE := false
PRODUCT_BUILD_PVMFW_IMAGE := false
TARGET_FORCE_OTA_PACKAGE := false
