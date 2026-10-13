#!/usr/bin/env bash
# Offline only: preserve the physically validated V37 kernel and output tree.
set -euo pipefail
workspace=/mnt/c/Users/Pierre/Desktop/A6L
kernel_dir=/home/a6l/kernel/a6l-baseline-7.2
out=/home/a6l/kernel/out-a6l-android-init
archive=$workspace/firmware/extracted/android-init-kernel-20260917
clang_dir=/home/a6l/android/a6l-lineage24/prebuilts/clang/host/linux-x86/clang-r584948/bin
test "$(git -C "$kernel_dir" rev-parse HEAD)" = e47d622cb6d2440a9eacdc8bb2df32c037bec7b8
test ! -e "$archive"
test ! -e "$out"
mkdir "$archive" "$out"
exec 9>"$out/.build.lock"
flock -n 9
exec > >(tee "$out/build.log") 2>&1
cp /home/a6l/kernel/out-a6l-baseline-7.2/.config "$out/.config"
cp "$out/.config" "$archive/config-input"
git -C "$kernel_dir" diff --binary > "$archive/source.patch"
export PATH="$clang_dir:$PATH"
export KBUILD_BUILD_USER=a6l KBUILD_BUILD_HOST=a6l-build
export KBUILD_BUILD_TIMESTAMP='2026-09-14 00:00:00 UTC'
cd "$kernel_dir"
scripts/config --file "$out/.config" -e SECURITY_NETWORK -e SECURITY_SELINUX -e F2FS_FS_SECURITY
make O="$out" ARCH=arm64 LLVM=1 olddefconfig
cp "$out/.config" "$archive/config"
scripts/diffconfig "$archive/config-input" "$archive/config" > "$archive/config-differences.txt"
for setting in CONFIG_SECURITY_NETWORK=y CONFIG_SECURITY_SELINUX=y CONFIG_F2FS_FS_SECURITY=y CONFIG_EXT4_FS_SECURITY=y CONFIG_MMC_SDHCI_MSM=m; do
  grep -qx "$setting" "$out/.config"
done
make O="$out" ARCH=arm64 LLVM=1 -j12 Image.gz
make O="$out" ARCH=arm64 LLVM=1 -j12 drivers/mmc/host/sdhci-msm.ko
cp "$out/arch/arm64/boot/Image" "$out/arch/arm64/boot/Image.gz" "$out/drivers/mmc/host/sdhci-msm.ko" "$archive/"
modinfo "$archive/sdhci-msm.ko" > "$archive/module-info.txt"
git diff --binary > "$out/source-after.patch"
cmp "$archive/source.patch" "$out/source-after.patch"
printf '\nA6L_ANDROID_INIT_KERNEL_BUILD_SUCCESS\n'
cp "$out/build.log" "$archive/build.log"
