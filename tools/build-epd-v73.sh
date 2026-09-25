#!/usr/bin/env bash
# V73 e-paper: panel module (bring-up in .enable + epd_power) and the a6l_epdd service; then the service in QEMU --dry.
set -uo pipefail
W=/mnt/c/Users/Pierre/Desktop/A6L; K=/home/a6l/kernel/a6l-baseline-7.2; O=/home/a6l/kernel/out-a6l-phone-v67
M=/home/a6l/kernel/a6l-epd-v73; A=$W/firmware/extracted/epd-v73; mkdir -p $M $A
cp $W/device/hisense/a6l/kernel/panels/panel-a6l-epd-dsi.c $M/; printf 'obj-m += panel-a6l-epd-dsi.o\n' > $M/Makefile
export PATH=/home/a6l/android/a6l-lineage24/prebuilts/clang/host/linux-x86/clang-r584948/bin:$PATH KBUILD_BUILD_USER=a6l KBUILD_BUILD_HOST=a6l-build KBUILD_BUILD_TIMESTAMP='2026-09-14 00:00:00 UTC'
if make -C $K O=$O ARCH=arm64 LLVM=1 -j8 M=$M modules > $M/build.log 2>&1; then
  llvm-strip --strip-debug -o $A/panel-a6l-epd-dsi.ko $M/panel-a6l-epd-dsi.ko; echo "MODULE_BUILD_PASS"
  grep -a -i "warning" $M/build.log | head -5
  modinfo -F vermagic $A/panel-a6l-epd-dsi.ko; modinfo -F vermagic $W/firmware/extracted/phone-kernel-v67-candidate-20260919/display-modules/panel-a6l-epd-dsi.ko 2>/dev/null
  modinfo -F parm $A/panel-a6l-epd-dsi.ko | cut -c1-60
else grep -a -B2 -A3 "error" $M/build.log | head -40; echo MODULE_BUILD_FAIL; fi
OUT=$HOME/mesa-a6l; CC=$(ls $HOME/ndk/android-ndk-r27c/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android34-clang)
if $CC -O2 -Wall -Wextra -I$OUT/sysroot/include -I$OUT/sysroot/include/libdrm -o $A/a6l_epdd $W/device/hisense/a6l/diagnostic/a6l_epdd.c $OUT/sysroot/lib/libdrm.a -ldl -Wl,--export-dynamic 2> $A/epdd-build.log; then
  $HOME/ndk/android-ndk-r27c/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip $A/a6l_epdd; echo EPDD_BUILD_PASS; readelf -d $A/a6l_epdd | grep NEEDED
else cat $A/epdd-build.log | head -30; echo EPDD_BUILD_FAIL; exit 1; fi
cat $A/epdd-build.log | head -10
cd $W && python3 tools/Test-EpddQemu.py "$1" firmware/extracted/eink-spi-nor-20260920/epd-nor.bin $A/a6l_epdd .relay/inbox-files/v73/script-dry.txt .relay/inbox-files/v73/test-landscape.pgm .relay/inbox-files/v73/test-portrait.pgm .relay/inbox-files/v73/test-gradient.pgm 2>&1 | grep -v "A6L_EINK_RLE" | tail -n 40
R=$W/firmware/extracted/epdd-qemu-$(date +%Y%m%d)-r$1; ls -la $R/*.a6lepd
cmp $R/update3-t25.a6lepd $W/firmware/extracted/eink-swtcon-20260923-r4/update3-t25.a6lepd && echo "IDENTICAL_TO_R137_PICTURE"
sha256sum $A/* | cut -c1-80
