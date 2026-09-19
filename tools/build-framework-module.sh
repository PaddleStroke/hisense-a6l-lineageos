#!/usr/bin/env bash
# usage: build-framework-module.sh <label> <module> [module...]   (runs `m` for VM-test modules)
set -eo pipefail
label=$1; shift
cd /home/a6l/android/a6l-lineage24
log=/home/a6l/logs/build-$label-$(date +%Y%m%d-%H%M%S).log
exec > "$log" 2>&1
export A6L_SOONG_GOMEMLIMIT=36GiB
source build/envsetup.sh
source vendor/lineage/vars/aosp_target_release
lunch lineage_gsi_a6l "$aosp_target_release" userdebug
m -j8 "$@"
