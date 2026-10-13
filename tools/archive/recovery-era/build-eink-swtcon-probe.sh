#!/usr/bin/env bash
# Rebuild a6l_eink_swtcon_probe (dynamic bionic binary that dlopens the stock libtcon_eink.so) in the Lineage tree.
set -eo pipefail
cd /home/a6l/android/a6l-lineage24
W=/mnt/c/Users/Pierre/Desktop/A6L/device/hisense/a6l
cp "$W/Android.bp" device/hisense/a6l/; cp "$W/diagnostic/eink_swtcon_probe.c" device/hisense/a6l/diagnostic/
log=/home/a6l/logs/build-eink-swtcon-$(date +%Y%m%d-%H%M%S).log
exec > "$log" 2>&1
export A6L_SOONG_GOMEMLIMIT=36GiB
source build/envsetup.sh
source vendor/lineage/vars/aosp_target_release
lunch lineage_gsi_a6l "$aosp_target_release" userdebug
m -j8 a6l_eink_swtcon_probe
