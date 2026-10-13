#!/usr/bin/env bash
set -euo pipefail
workspace=/mnt/c/Users/Pierre/Desktop/A6L
kernel_dir=/home/a6l/kernel/a6l-baseline-7.2
out=/home/a6l/kernel/out-a6l-framework-v50
archive=$workspace/firmware/extracted/framework-kernel-v50-20260918
module_dir=/home/a6l/kernel/a6l-simplefb-framework-v50
grep -q '^A6L_FRAMEWORK_KERNEL_V50_BUILD_PASS$' "$archive/build.log"
if [ "${1:-}" = --resume ]; then
    test -d "$module_dir"
    test ! -e "$archive/a6l_simplefb.ko"
    cmp "$workspace/firmware/extracted/android-display-v40-20260917-r1/a6l_simplefb.c" "$module_dir/a6l_simplefb.c"
    test ! -e "$module_dir/build-missing-symvers.log"
    cp "$module_dir/build.log" "$module_dir/build-missing-symvers.log"
else
    test ! -e "$module_dir"
    mkdir "$module_dir"
    cp "$workspace/firmware/extracted/android-display-v40-20260917-r1/a6l_simplefb.c" "$module_dir/"
    printf 'obj-m += a6l_simplefb.o\n' > "$module_dir/Makefile"
fi
export PATH=/home/a6l/android/a6l-lineage24/prebuilts/clang/host/linux-x86/clang-r584948/bin:$PATH
export KBUILD_BUILD_USER=a6l KBUILD_BUILD_HOST=a6l-build
export KBUILD_BUILD_TIMESTAMP='2026-09-14 00:00:00 UTC'
make -C "$kernel_dir" O="$out" ARCH=arm64 LLVM=1 -j12 modules > "$out/modules-build.log" 2>&1
make -C "$kernel_dir" O="$out" ARCH=arm64 LLVM=1 -j12 M="$module_dir" modules > "$module_dir/build.log" 2>&1
cp "$module_dir/a6l_simplefb.c" "$module_dir/a6l_simplefb.ko" "$archive/"
cp "$module_dir/build.log" "$archive/display-module-build.log"
sha256sum "$archive/a6l_simplefb.c" "$archive/a6l_simplefb.ko" > "$archive/display-module-SHA256SUMS"
echo A6L_FRAMEWORK_DISPLAY_MODULE_V50_BUILD_PASS
