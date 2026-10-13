#!/usr/bin/env bash
# Rebuild only the system image as labeled EROFS (input for the real-init RAM boot flow).
set -eo pipefail
cd /home/a6l/android/a6l-lineage24
W=/mnt/c/Users/Pierre/Desktop/A6L/device/hisense/a6l
cp "$W/BoardConfig.mk" "$W/lineage_gsi_a6l.mk" device/hisense/a6l/
log=/home/a6l/logs/build-systemimage-erofs-$(date +%Y%m%d-%H%M%S).log
exec > "$log" 2>&1
export A6L_SOONG_GOMEMLIMIT=36GiB
source build/envsetup.sh
source vendor/lineage/vars/aosp_target_release
lunch lineage_gsi_a6l "$aosp_target_release" userdebug
m -j8 systemimage vendorimage
ls -la out/target/product/a6l/*.img; file out/target/product/a6l/*.img
