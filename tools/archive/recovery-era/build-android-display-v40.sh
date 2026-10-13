#!/usr/bin/env bash
set -euo pipefail
workspace=/mnt/c/Users/Pierre/Desktop/A6L
android=/home/a6l/android/a6l-lineage24
kernel=/home/a6l/kernel/a6l-baseline-7.2
out=/home/a6l/kernel/out-a6l-android-init
mod=/home/a6l/kernel/a6l-simplefb-v40
test ! -e "$mod"
mkdir "$mod"
cp "$workspace/device/hisense/a6l/kernel/a6l_simplefb.c" "$mod/"
printf 'obj-m += a6l_simplefb.o\n' > "$mod/Makefile"
export PATH="$android/prebuilts/clang/host/linux-x86/clang-r584948/bin:$PATH"
exec > >(tee /home/a6l/logs/build-android-display-v40.log) 2>&1
make -C "$kernel" O="$out" ARCH=arm64 LLVM=1 M="$mod" -j4 modules
modinfo "$mod/a6l_simplefb.ko"
cd "$android"
for part in Android.bp diagnostic/drm_probe.c; do
    cp "$workspace/device/hisense/a6l/$part" "device/hisense/a6l/$part"
done
export A6L_SOONG_GOMEMLIMIT=36GiB
source build/envsetup.sh
source vendor/lineage/vars/aosp_target_release
lunch lineage_gsi_a6l "$aosp_target_release" userdebug
m -j8 a6l_drm_probe
