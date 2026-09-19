#!/usr/bin/env bash
set -euo pipefail
kernel_dir=/home/a6l/kernel/a6l-mainline
out=/home/a6l/kernel/out-a6l-probe
archive=/mnt/c/Users/Pierre/Desktop/A6L/firmware/extracted/storage-ordered-kernel-v31-20260916
clang_dir=/home/a6l/android/a6l-lineage24/prebuilts/clang/host/linux-x86/clang-r584948/bin
test "$(git -C "$kernel_dir" rev-parse HEAD)" = e47d622cb6d2440a9eacdc8bb2df32c037bec7b8
exec 9>"$out/.build.lock"
flock -n 9
test ! -e "$archive"
mkdir "$archive"
cp "$out/.config" "$archive/config-before"
exec > >(tee /home/a6l/logs/kernel-storage-ordered-v31.log) 2>&1
export PATH="$clang_dir:$PATH"
export KBUILD_BUILD_USER=a6l KBUILD_BUILD_HOST=a6l-build
export KBUILD_BUILD_TIMESTAMP='2026-09-14 00:00:00 UTC'
cd "$kernel_dir"
cmp "$out/.config" /mnt/c/Users/Pierre/Desktop/A6L/firmware/extracted/storage-module-kernel-v19-20260916/config
make O="$out" ARCH=arm64 LLVM=1 -j8 Image.gz
make O="$out" ARCH=arm64 LLVM=1 -j8 drivers/mmc/host/sdhci-msm.ko
cp "$out/.config" "$archive/config"
cp "$out/arch/arm64/boot/Image" "$out/arch/arm64/boot/Image.gz" "$archive/"
cp "$out/drivers/mmc/host/sdhci-msm.ko" "$archive/"
modinfo "$archive/sdhci-msm.ko" > "$archive/module-info.txt"
cp /home/a6l/logs/kernel-storage-ordered-v31.log "$archive/build.log"
printf '\nA6L_STORAGE_COMMIT_KERNEL_BUILD_SUCCESS\n'
