#!/usr/bin/env bash
set -eo pipefail
cd /home/a6l/android/a6l-lineage24
workspace=/mnt/c/Users/Pierre/Desktop/A6L
for part in Android.bp diagnostic/haptic_probe.c; do
    cmp -s "$workspace/device/hisense/a6l/$part" "device/hisense/a6l/$part" || cp "$workspace/device/hisense/a6l/$part" "device/hisense/a6l/$part"
done
test ! -e /home/a6l/logs/build-android-controls-20260917.log
exec > >(tee /home/a6l/logs/build-android-controls-20260917.log) 2>&1
export A6L_SOONG_GOMEMLIMIT=36GiB
source build/envsetup.sh
source vendor/lineage/vars/aosp_target_release
lunch lineage_gsi_a6l "$aosp_target_release" userdebug
m -j8 a6l_haptic_probe
