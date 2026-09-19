#!/usr/bin/env bash
set -eo pipefail
cd /home/a6l/android/a6l-lineage24
workspace=/mnt/c/Users/Pierre/Desktop/A6L
for part in Android.bp diagnostic/gralloc_probe.cpp; do
    cp "$workspace/device/hisense/a6l/$part" "device/hisense/a6l/$part"
done
build_log=${A6L_ALLOCATOR_BUILD_LOG:-/home/a6l/logs/build-android-allocator-v41-r2.log}
test ! -e "$build_log"
exec > >(tee "$build_log") 2>&1
export A6L_SOONG_GOMEMLIMIT=36GiB
source build/envsetup.sh
source vendor/lineage/vars/aosp_target_release
lunch lineage_gsi_a6l "$aosp_target_release" userdebug
m -j8 a6l_gralloc_probe android.hardware.graphics.allocator@4.0-service.minigbm android.hardware.graphics.mapper@4.0-impl.minigbm android.hardware.composer.hwc3-service.drm
