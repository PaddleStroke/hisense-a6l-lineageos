#!/usr/bin/env bash
set -euo pipefail
version=${1:?Specify 6.19 or 7.2}
attempt=${2:-1}
case "$version" in
  6.19) revision=a587e4f18b483d0a17579e6325c861b303988bda ;;
  7.2) revision=e47d622cb6d2440a9eacdc8bb2df32c037bec7b8 ;;
  *) exit 2 ;;
esac
workspace=/mnt/c/Users/Pierre/Desktop/A6L
kernel_dir=/home/a6l/kernel/a6l-baseline-$version
out=/home/a6l/kernel/out-a6l-baseline-$version
archive=$workspace/firmware/extracted/baseline-$version-kernel-20260917
clang_dir=/home/a6l/android/a6l-lineage24/prebuilts/clang/host/linux-x86/clang-r584948/bin
test "$(git -C "$kernel_dir" rev-parse HEAD)" = "$revision"
mkdir -p "$out"
exec 9>"$out/.build.lock"
flock -n 9
if [ "$attempt" = 1 ]; then
  test ! -e "$archive"
  mkdir "$archive"
  cp "$workspace/firmware/extracted/storage-state-kernel-v32-20260917/config" "$out/.config"
  cp "$out/.config" "$archive/config-input"
else
  test -e "$archive/config"
  cmp "$out/.config" "$archive/config"
fi
test ! -e "$out/build-r$attempt.log"
exec > >(tee "$out/build-r$attempt.log") 2>&1
export PATH="$clang_dir:$PATH"
export KBUILD_BUILD_USER=a6l KBUILD_BUILD_HOST=a6l-build
export KBUILD_BUILD_TIMESTAMP='2026-09-14 00:00:00 UTC'
cd "$kernel_dir"
make O="$out" ARCH=arm64 LLVM=1 olddefconfig
cp "$out/.config" "$archive/config"
scripts/diffconfig "$archive/config-input" "$archive/config" > "$archive/config-differences.txt"
for setting in CONFIG_MMC_SDHCI_MSM=m CONFIG_USB_CONFIGFS_ACM=y CONFIG_DEVTMPFS=y CONFIG_SERIAL_EARLYCON=y CONFIG_FONT_8x16=y; do
  grep -qx "$setting" "$out/.config"
done
make O="$out" ARCH=arm64 LLVM=1 -j12 Image.gz
make O="$out" ARCH=arm64 LLVM=1 -j12 drivers/mmc/host/sdhci-msm.ko
make O="$out" ARCH=arm64 LLVM=1 DTC_FLAGS=-@ -j4 qcom/sdm660-hisense-a6l-baseline.dtb
cp "$out/arch/arm64/boot/Image" "$out/arch/arm64/boot/Image.gz" "$out/drivers/mmc/host/sdhci-msm.ko" "$archive/"
cp "$out/arch/arm64/boot/dts/qcom/sdm660-hisense-a6l-baseline.dtb" "$archive/base.dtb"
modinfo "$archive/sdhci-msm.ko" > "$archive/module-info.txt"
cp "$out/build-r$attempt.log" "$archive/build.log"
printf '\nA6L_BASELINE_KERNEL_BUILD_SUCCESS %s\n' "$version"
