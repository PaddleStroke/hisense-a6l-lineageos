#!/usr/bin/env bash
set -euo pipefail
kernel_dir=/home/a6l/kernel/a6l-mainline
out=/home/a6l/kernel/out-a6l-probe
clang_dir=/home/a6l/android/a6l-lineage24/prebuilts/clang/host/linux-x86/clang-r584948/bin
test "$(git -C "$kernel_dir" rev-parse HEAD)" = e47d622cb6d2440a9eacdc8bb2df32c037bec7b8
exec 9>"$out/.build.lock"
flock -n 9
exec > >(tee /home/a6l/logs/kernel-event-trace-v16.log) 2>&1
export PATH="$clang_dir:$PATH"
export KBUILD_BUILD_USER=a6l KBUILD_BUILD_HOST=a6l-build
export KBUILD_BUILD_TIMESTAMP='2026-09-14 00:00:00 UTC'
cd "$kernel_dir"
make O="$out" ARCH=arm64 LLVM=1 -j8 Image.gz
printf '\nA6L_EVENT_TRACE_KERNEL_BUILD_SUCCESS\n'
