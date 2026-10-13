#!/usr/bin/env bash
# Compile only a recovery DTB and its embedded overlay; no phone access.
set -euo pipefail
workspace=/mnt/c/Users/Pierre/Desktop/A6L
kernel_dir="$HOME/kernel/a6l-mainline"
out="$HOME/kernel/out-a6l-probe"
clang_dir="$HOME/android/a6l-lineage24/prebuilts/clang/host/linux-x86/clang-r584948/bin"
test "$(git -C "$kernel_dir" rev-parse HEAD)" = e47d622cb6d2440a9eacdc8bb2df32c037bec7b8
exec 9>"$out/.build.lock"
flock -n 9 || { echo 'A kernel build is already running'; exit 1; }
export PATH="$clang_dir:$PATH"
cd "$kernel_dir"
cp "$workspace/device/hisense/a6l/kernel/"sdm660-hisense-a6l-{probe,recovery}.dts arch/arm64/boot/dts/qcom/
# Hisense's captured ufdt rejects even target-path overlays without __symbols__.
make O="$out" ARCH=arm64 LLVM=1 DTC_FLAGS=-@ -j4 qcom/sdm660-hisense-a6l-recovery.dtb
dtc -@ -I dts -O dtb \
    -o "$out/arch/arm64/boot/dts/qcom/a6l-recovery-overlay.dtbo" \
    "$workspace/device/hisense/a6l/kernel/a6l-recovery-overlay.dts"
echo A6L_RECOVERY_DTB_BUILD_SUCCESS
