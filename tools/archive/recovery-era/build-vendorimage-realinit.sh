#!/usr/bin/env bash
# Rebuild the vendor image (HALs + VINTF manifest packaged) for the real-init VM/phone RAM flow.
set -eo pipefail
cd /home/a6l/android/a6l-lineage24
W=/mnt/c/Users/Pierre/Desktop/A6L/device/hisense/a6l
cp "$W/BoardConfig.mk" "$W/lineage_gsi_a6l.mk" "$W/a6l-software-graphics.mk" "$W/manifest.xml" device/hisense/a6l/
mkdir -p device/hisense/a6l/audio/realinit && cp "$W/audio/realinit/"*.xml device/hisense/a6l/audio/realinit/
export A6L_SOONG_GOMEMLIMIT=36GiB
source build/envsetup.sh
source vendor/lineage/vars/aosp_target_release
lunch lineage_gsi_a6l "$aosp_target_release" userdebug
m -j12 ${A6L_TARGETS:-vendorimage}
ls -la out/target/product/a6l/*.img
echo A6L_BUILD_DONE
