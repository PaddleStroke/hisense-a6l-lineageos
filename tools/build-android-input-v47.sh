#!/usr/bin/env bash
set -eo pipefail
cd /home/a6l/android/a6l-lineage24
workspace=/mnt/c/Users/Pierre/Desktop/A6L
for part in Android.bp diagnostic/input_services.c diagnostic/input_fixture.c diagnostic/private_properties.h; do
    cmp -s "$workspace/device/hisense/a6l/$part" "device/hisense/a6l/$part" || cp "$workspace/device/hisense/a6l/$part" "device/hisense/a6l/$part"
done
for part in diagnostic/input_client.cpp diagnostic/input_policy.h; do
    if [ -f "$workspace/device/hisense/a6l/$part" ]; then
        cmp -s "$workspace/device/hisense/a6l/$part" "device/hisense/a6l/$part" || cp "$workspace/device/hisense/a6l/$part" "device/hisense/a6l/$part"
    fi
done
build_log=${A6L_INPUT_BUILD_LOG:-/home/a6l/logs/build-android-input-v47-r1.log}
test ! -e "$build_log"
exec > >(tee "$build_log") 2>&1
export A6L_SOONG_GOMEMLIMIT=36GiB
source build/envsetup.sh
source vendor/lineage/vars/aosp_target_release
lunch lineage_gsi_a6l "$aosp_target_release" userdebug
if [ "$#" -eq 0 ]; then set -- a6l_input_services a6l_input_fixture a6l_input_client; fi
m -j8 "$@"
