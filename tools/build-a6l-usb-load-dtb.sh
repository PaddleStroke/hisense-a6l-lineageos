#!/usr/bin/env bash
set -euo pipefail
workspace=/mnt/c/Users/Pierre/Desktop/A6L
kernel_dir="$HOME/kernel/a6l-mainline"
out="$HOME/kernel/out-a6l-probe"
export PATH="$HOME/android/a6l-lineage24/prebuilts/clang/host/linux-x86/clang-r584948/bin:$PATH"
test "$(git -C "$kernel_dir" rev-parse HEAD)" = e47d622cb6d2440a9eacdc8bb2df32c037bec7b8
exec 9>"$out/.build.lock"
flock -n 9
# Preserve prior DT artifacts; build a separately named V17 target.
for name in sdm660-hisense-a6l-probe.dts sdm660-hisense-a6l-recovery.dts sdm660-hisense-a6l-usb-only.dts; do
    cmp "$workspace/device/hisense/a6l/kernel/$name" "$kernel_dir/arch/arm64/boot/dts/qcom/$name"
done
cp "$workspace/device/hisense/a6l/kernel/sdm660-hisense-a6l-usb-load.dts" "$kernel_dir/arch/arm64/boot/dts/qcom/"
cd "$kernel_dir"
make O="$out" ARCH=arm64 LLVM=1 DTC_FLAGS=-@ -j4 qcom/sdm660-hisense-a6l-usb-load.dtb
