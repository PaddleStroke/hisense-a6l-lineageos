#!/usr/bin/env bash
# Compile only. No adb, fastboot, EDL, signing for distribution, or flashing.
set -eo pipefail
workspace=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
source_dir=${A6L_SOURCE_DIR:-"$HOME/android/a6l-lineage24"}
cd "$source_dir"
test -f a6l-source-revisions.xml || { echo 'Complete source preparation first.' >&2; exit 1; }
mkdir -p "$HOME/logs" device/hisense/a6l
rsync -a "$workspace/device/hisense/a6l/" device/hisense/a6l/
python3 "$workspace/tools/Configure-SoongMemory.py" "$source_dir"
export A6L_SOONG_GOMEMLIMIT=${A6L_SOONG_GOMEMLIMIT:-36GiB}
if [[ -f "$HOME/logs/build-probe.log" ]]; then
    cp "$HOME/logs/build-probe.log" "$HOME/logs/build-probe-$(date +%Y%m%d-%H%M%S).log"
fi
exec > >(tee "$HOME/logs/build-probe.log") 2>&1
source build/envsetup.sh
source vendor/lineage/vars/aosp_target_release
lunch lineage_gsi_a6l "$aosp_target_release" userdebug
m -j8 systemimage
