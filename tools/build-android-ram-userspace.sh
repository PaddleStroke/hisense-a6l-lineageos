#!/usr/bin/env bash
set -eo pipefail
cd /home/a6l/android/a6l-lineage24
workspace=/mnt/c/Users/Pierre/Desktop/A6L
for part in Android.bp diagnostic/android_service.c diagnostic/storage_read.c diagnostic/storage_read_ranges.h; do
    if ! cmp -s "$workspace/device/hisense/a6l/$part" "device/hisense/a6l/$part"; then
        cp "$workspace/device/hisense/a6l/$part" "device/hisense/a6l/$part"
    fi
done
if [ -e /home/a6l/logs/build-android-ram-userspace.log ]; then
    cp /home/a6l/logs/build-android-ram-userspace.log "/home/a6l/logs/build-android-ram-userspace-$(date +%s).log"
fi
exec > >(tee /home/a6l/logs/build-android-ram-userspace.log) 2>&1
export A6L_SOONG_GOMEMLIMIT=36GiB
source build/envsetup.sh
source vendor/lineage/vars/aosp_target_release
lunch lineage_gsi_a6l "$aosp_target_release" userdebug
m -j8 init_second_stage.recovery linker.recovery adbd.recovery sh.recovery toybox_recovery toolbox.recovery sepolicy.recovery a6l_android_probe
