#!/usr/bin/env bash
set -euo pipefail
workspace=/mnt/c/Users/Pierre/Desktop/A6L
kernel_dir="$HOME/kernel/a6l-mainline"
out="$HOME/kernel/out-a6l-probe"
clang_dir="$HOME/android/a6l-lineage24/prebuilts/clang/host/linux-x86/clang-r584948/bin"
exec 9>"$out/.build.lock"
flock -n 9 || { echo 'A kernel build is already running'; exit 1; }
export PATH="$clang_dir:$PATH"
export KBUILD_BUILD_USER=a6l KBUILD_BUILD_HOST=a6l-build
export KBUILD_BUILD_TIMESTAMP='2026-09-14 00:00:00 UTC'
python3 "$workspace/tools/Prepare-A6LKeepBootDomains.py"
cd "$kernel_dir"
make O="$out" ARCH=arm64 LLVM=1 -j8 Image
echo A6L_KEEP_BOOT_DOMAINS_BUILD_SUCCESS
