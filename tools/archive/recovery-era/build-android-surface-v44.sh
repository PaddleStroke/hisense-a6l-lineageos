#!/usr/bin/env bash
set -eo pipefail
cd /home/a6l/android/a6l-lineage24
workspace=/mnt/c/Users/Pierre/Desktop/A6L
for part in Android.bp diagnostic/surface_services.c diagnostic/surface_client.cpp diagnostic/private_properties.h; do
    cmp -s "$workspace/device/hisense/a6l/$part" "device/hisense/a6l/$part" || cp "$workspace/device/hisense/a6l/$part" "device/hisense/a6l/$part"
done
build_log=${A6L_GRAPHICS_BUILD_LOG:-/home/a6l/logs/build-android-surface-v44.log}
test ! -e "$build_log"
exec > >(tee "$build_log") 2>&1
export A6L_SOONG_GOMEMLIMIT=36GiB
source build/envsetup.sh
source vendor/lineage/vars/aosp_target_release
lunch lineage_gsi_a6l "$aosp_target_release" userdebug
m -j8 a6l_surface_services a6l_surface_client \
    out/target/product/a6l/system/bin/surfaceflinger \
    out/target/product/a6l/system/bin/hwservicemanager \
    out/target/product/a6l/vendor/lib64/hw/vulkan.pastel.so \
    out/target/product/a6l/vendor/lib64/hw/mapper.minigbm.so \
    out/target/product/a6l/vendor/bin/hw/android.hardware.graphics.allocator-service.minigbm \
    out/target/product/a6l/vendor/bin/hw/android.hardware.composer.hwc3-service.drm
