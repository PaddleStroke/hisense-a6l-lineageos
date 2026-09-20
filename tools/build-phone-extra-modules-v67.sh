#!/usr/bin/env bash
# Extra upstream drivers for the V67 phone kernel, built as EXTERNAL modules so the Image stays byte-identical:
#   tps65185.ko (e-paper PMIC), stk3310.ko (front ALS/proximity family). Build only.
set -euo pipefail
workspace=/mnt/c/Users/Pierre/Desktop/A6L
kernel_dir=/home/a6l/kernel/a6l-baseline-7.2; out=/home/a6l/kernel/out-a6l-phone-v67
archive=$workspace/firmware/extracted/phone-kernel-v67-candidate-20260919/extra-modules
mod=/home/a6l/kernel/a6l-extra-modules-v67
test "$(git -C "$kernel_dir" rev-parse HEAD)" = e47d622cb6d2440a9eacdc8bb2df32c037bec7b8
rm -rf "$mod"; mkdir -p "$mod" "$archive"
cp "$kernel_dir/drivers/regulator/tps65185.c" "$kernel_dir/drivers/iio/light/stk3310.c" "$mod/"
printf 'obj-m += tps65185.o stk3310.o\n' > "$mod/Makefile"
export PATH=/home/a6l/android/a6l-lineage24/prebuilts/clang/host/linux-x86/clang-r584948/bin:$PATH
export KBUILD_BUILD_USER=a6l KBUILD_BUILD_HOST=a6l-build KBUILD_BUILD_TIMESTAMP='2026-09-14 00:00:00 UTC'
make -C "$kernel_dir" O="$out" ARCH=arm64 LLVM=1 -j8 M="$mod" modules > "$mod/build.log" 2>&1 || { tail -n 25 "$mod/build.log"; exit 1; }
for m in tps65185 stk3310; do llvm-strip --strip-debug -o "$mod/$m.stripped.ko" "$mod/$m.ko"; cat "$mod/$m.stripped.ko" > "$archive/$m.ko"; modinfo "$archive/$m.ko" | grep -E "vermagic|alias.*of:|depends"; done
( cd "$archive" && sha256sum *.ko > SHA256SUMS ); cp "$mod/build.log" "$archive/build.log"
sha256sum "$workspace/firmware/extracted/phone-kernel-v67-candidate-20260919/Image"
echo A6L_EXTRA_MODULES_V67_PASS
