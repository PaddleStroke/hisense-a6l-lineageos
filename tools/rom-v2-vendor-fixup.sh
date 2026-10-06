#!/usr/bin/env bash
# merge2 (25 Sep 2026): r2 vendor-only fix-up WITHOUT re-syncing the other agents' in-progress source dirs (radio, gnss,
# misc2 vibrator, ...): copy only the listed rom/ files from the repo into the tree, rebuild vendorimage, re-run the boot+kit
# phase. Run as root in the background. usage: rom-v2-vendor-fixup.sh <tag> <rom-relative files...>
set -uo pipefail
TAG=$1; shift
R=/mnt/c/Users/Pierre/Desktop/A6L; L=/home/a6l/android/a6l-lineage24; T=$L/device/hisense/a6l; W=/home/a6l/rom-v2
for f in "$@"; do mkdir -p $(dirname $T/rom/$f); tr -d '\r' < $R/device/hisense/a6l/rom/$f > $T/rom/$f; echo "synced rom/$f"; done
echo "== $(date) m vendorimage"
( cd $L && export NINJA_HIGHMEM_NUM_JOBS=2 SOONG_NINJA=ninja && source build/envsetup.sh > /dev/null && source vendor/lineage/vars/aosp_target_release && lunch lineage_gsi_a6l "$aosp_target_release" userdebug > /dev/null 2>&1 && m -j12 vendorimage ) > $W/build-$TAG-vendor.log 2>&1 || { grep -E "error:|FAILED:|ninja: build stopped" $W/build-$TAG-vendor.log | head -20; echo FIXUP_FAIL; exit 1; }
tail -2 $W/build-$TAG-vendor.log
bash $W/rom-v2-pipeline.sh $TAG boot
echo FIXUP_DONE
