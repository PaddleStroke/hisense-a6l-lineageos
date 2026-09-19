#!/usr/bin/env bash
# Separate offline runtime/QEMU kernel. Never updates a phone or a validated image.
set -euo pipefail
workspace=/mnt/c/Users/Pierre/Desktop/A6L
kernel_dir=/home/a6l/kernel/a6l-baseline-7.2
out=/home/a6l/kernel/out-a6l-framework-v50
baseline=$workspace/firmware/extracted/android-init-kernel-20260917
archive=$workspace/firmware/extracted/framework-kernel-v50-20260918
clang_dir=/home/a6l/android/a6l-lineage24/prebuilts/clang/host/linux-x86/clang-r584948/bin
test "$(git -C "$kernel_dir" rev-parse HEAD)" = e47d622cb6d2440a9eacdc8bb2df32c037bec7b8
test ! -e "$out"
test ! -e "$archive"
mkdir "$out" "$archive"
exec 9>"$out/.build.lock"
flock -n 9
exec > >(tee "$out/build.log") 2>&1
git -C "$kernel_dir" diff --binary > "$archive/source.patch"
cmp "$baseline/source.patch" "$archive/source.patch"
cp "$baseline/config" "$out/.config"
cp "$out/.config" "$archive/config-input"
export PATH="$clang_dir:$PATH"
export KBUILD_BUILD_USER=a6l KBUILD_BUILD_HOST=a6l-build
export KBUILD_BUILD_TIMESTAMP='2026-09-14 00:00:00 UTC'
cd "$kernel_dir"
scripts/config --file "$out/.config" -e USERFAULTFD -e CPUSETS_V1 \
    -e VIRTIO_MMIO -e VIRTIO_BLK -e DM_VERITY -e EROFS_FS
make O="$out" ARCH=arm64 LLVM=1 olddefconfig
cp "$out/.config" "$archive/config"
scripts/diffconfig "$archive/config-input" "$archive/config" > "$archive/config-differences.txt"
for setting in USERFAULTFD CPUSETS_V1 VIRTIO_MMIO VIRTIO_BLK DM_VERITY EROFS_FS; do
    grep -qx "CONFIG_$setting=y" "$out/.config"
done
make O="$out" ARCH=arm64 LLVM=1 -j12 Image.gz
cp "$out/arch/arm64/boot/Image" "$out/arch/arm64/boot/Image.gz" "$archive/"
git diff --binary > "$out/source-after.patch"
cmp "$archive/source.patch" "$out/source-after.patch"
sha256sum "$archive/Image" "$archive/Image.gz" > "$archive/SHA256SUMS"
printf '\nA6L_FRAMEWORK_KERNEL_V50_BUILD_PASS\n'
cp "$out/build.log" "$archive/build.log"
