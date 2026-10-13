#!/usr/bin/env bash
# V61: AOSP example AIDL audio HAL for the diskless VM (audioserver exits without any audio HAL).
set -eo pipefail
cd /home/a6l/android/a6l-lineage24
log=/home/a6l/logs/build-framework-audio-v61-r${1:-1}.log
test ! -e "$log"
exec > "$log" 2>&1
export A6L_SOONG_GOMEMLIMIT=36GiB
source build/envsetup.sh
source vendor/lineage/vars/aosp_target_release
lunch lineage_gsi_a6l "$aosp_target_release" userdebug
m -j8 android.hardware.audio.service-aidl.example android.hardware.audio.effect.service-aidl.example
