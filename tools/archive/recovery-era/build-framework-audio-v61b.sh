#!/usr/bin/env bash
# V61: the example audio HAL ships only inside the vendor APEX com.android.hardware.audio.
set -eo pipefail
cd /home/a6l/android/a6l-lineage24
log=/home/a6l/logs/build-framework-audio-v61-apex-r${1:-1}.log
test ! -e "$log"
exec > "$log" 2>&1
export A6L_SOONG_GOMEMLIMIT=36GiB
source build/envsetup.sh
source vendor/lineage/vars/aosp_target_release
lunch lineage_gsi_a6l "$aosp_target_release" userdebug
m -j8 com.android.hardware.audio
find out/target/product/a6l -name "com.android.hardware.audio*" | head
