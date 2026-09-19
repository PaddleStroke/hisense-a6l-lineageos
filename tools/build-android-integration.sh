#!/usr/bin/env bash
set -eo pipefail
cd /home/a6l/android/a6l-lineage24
workspace=/mnt/c/Users/Pierre/Desktop/A6L
for part in Android.bp diagnostic/integration_probe.c diagnostic/binder_client.cpp; do
    if ! cmp -s "$workspace/device/hisense/a6l/$part" "device/hisense/a6l/$part"; then
        cp "$workspace/device/hisense/a6l/$part" "device/hisense/a6l/$part"
    fi
done
exec > >(tee /home/a6l/logs/build-android-integration-v39.log) 2>&1
export A6L_SOONG_GOMEMLIMIT=36GiB
source build/envsetup.sh
source vendor/lineage/vars/aosp_target_release
lunch lineage_gsi_a6l "$aosp_target_release" userdebug
m -j8 servicemanager.recovery a6l_binder_client a6l_integration_probe
