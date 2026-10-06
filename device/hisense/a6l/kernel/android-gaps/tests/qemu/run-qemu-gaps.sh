#!/usr/bin/env bash
# QEMU functional test of the kernel-gaps modules on the real A6L kernels (WSL, offline, no phone).
# usage: run-qemu-gaps.sh v67|r5 [workdir]   -> prints the test log, last line A6L_GAPS_QEMU PASS|FAIL
set -euo pipefail
K=${1:?v67|r5}; HERE=$(cd "$(dirname "$0")" && pwd); REPO=$(cd "$HERE/../../../../../../.." && pwd)
W=${2:-/tmp/a6l-gaps-qemu-$K}; G=$REPO/firmware/extracted/kernel-gaps-20260929
case $K in
  v67) IMG=$REPO/firmware/extracted/phone-kernel-v67-candidate-20260919/Image;;
  r5)  IMG=$REPO/firmware/extracted/kernel-r5-20260930/Image;;
  *) echo "v67|r5"; exit 2;;
esac
DTB=$(ls -d $REPO/firmware/extracted/android-framework-v72-*-r3/virt.dtb | tail -1)
rm -rf "$W"; mkdir -p "$W/root"/{m,proc,sys,dev,tmp}
aarch64-linux-gnu-gcc -static -O2 -Wall -Wextra -Werror -Wno-sign-compare -Wno-missing-field-initializers -o "$W/root/init" "$HERE/gaps_init.c"
cp "$G/$K"/*.ko "$W/root/m/"
( cd "$W/root" && find . | cpio -o -H newc --quiet | gzip -9 > "$W/initramfs.gz" )
timeout 600 qemu-system-aarch64 -machine virt,gic-version=3 -cpu cortex-a53 -smp 4 -m 1024 -nodefaults -nographic -monitor none \
  -serial stdio -nic none -no-reboot -dtb "$DTB" -kernel "$IMG" -initrd "$W/initramfs.gz" \
  -append "console=ttyAMA0 rdinit=/init panic=-1 loglevel=4" < /dev/null > "$W/console.log" 2>&1 || true
grep -a -E "^(ok|FAIL|==) |A6L_GAPS_QEMU|tainted=|Kernel panic|CFI failure|BUG:|WARNING:|Oops" "$W/console.log" | tr -d '\r'
grep -a -q "A6L_GAPS_QEMU PASS" "$W/console.log"
