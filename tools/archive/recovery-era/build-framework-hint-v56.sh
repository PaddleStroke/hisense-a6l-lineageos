#!/usr/bin/env bash
set -eo pipefail
cd /home/a6l/android/a6l-lineage24
log=/home/a6l/logs/build-framework-hint-v56-r1.log
test ! -e "$log"
exec > "$log" 2>&1
export A6L_SOONG_GOMEMLIMIT=36GiB
source build/envsetup.sh
source vendor/lineage/vars/aosp_target_release
lunch lineage_gsi_a6l "$aosp_target_release" userdebug
m -j8 services
