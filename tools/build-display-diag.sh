#!/bin/bash
# Build the display diagnostics for the recovery: static modetest (libdrm tests) + a6l_mmio. Offline, WSL.
set -eu
OUT=$HOME/mesa-a6l; A6L=/mnt/c/Users/Pierre/Desktop/A6L; DST=$HOME/display-diag; mkdir -p $DST
. $HOME/venv-mesa/bin/activate
B=$OUT/libdrm-tests-build
[ -d $B ] || meson setup $B $OUT/libdrm-src --cross-file $OUT/cross.ini -Ddefault_library=static -Dfreedreno=enabled -Dintel=disabled -Dradeon=disabled \
  -Damdgpu=disabled -Dnouveau=disabled -Dvmwgfx=disabled -Dtests=true -Dman-pages=disabled -Dvalgrind=disabled -Dcairo-tests=disabled \
  -Detnaviv=disabled -Dexynos=disabled -Dtegra=disabled -Dvc4=disabled -Domap=disabled -Dc_link_args=-static > $DST/setup.log 2>&1 || { tail -n 20 $DST/setup.log; exit 1; }
ninja -C $B tests/modetest/modetest > $DST/build.log 2>&1 || { tail -n 25 $DST/build.log; exit 1; }
cp $B/tests/modetest/modetest $DST/modetest
CC=$(ls $HOME/ndk/android-ndk-r27c/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android34-clang)
$CC -O2 -static -Wall -o $DST/a6l_mmio $A6L/device/hisense/a6l/diagnostic/a6l_mmio.c
STRIP=$HOME/ndk/android-ndk-r27c/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip; $STRIP $DST/modetest $DST/a6l_mmio
file $DST/modetest $DST/a6l_mmio | cut -c1-150; sha256sum $DST/modetest $DST/a6l_mmio
echo A6L_DISPLAY_DIAG_BUILD_PASS
