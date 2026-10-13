#!/usr/bin/env bash
set -eo pipefail
workspace=/mnt/c/Users/Pierre/Desktop/A6L
source_dir="$HOME/android/a6l-lineage24"
mkdir -p "$HOME/logs" "$source_dir/device/hisense/a6l/diagnostic"
# Keep unchanged build definitions' timestamps: touching Android.bp needlessly
# forces Soong to regenerate the graph for a small diagnostic-only rebuild.
for part in Android.bp diagnostic/init.c diagnostic/storage_read.c diagnostic/storage_read_ranges.h; do
    source_file="$workspace/device/hisense/a6l/$part"
    target_file="$source_dir/device/hisense/a6l/$part"
    if ! cmp -s "$source_file" "$target_file"; then
        cp "$source_file" "$target_file"
    fi
done
exec > >(tee "$HOME/logs/build-diagnostic-init.log") 2>&1
cd "$source_dir"
export A6L_SOONG_GOMEMLIMIT=36GiB
source build/envsetup.sh
source vendor/lineage/vars/aosp_target_release
lunch lineage_gsi_a6l "$aosp_target_release" userdebug
m -j4 a6l_probe_init
