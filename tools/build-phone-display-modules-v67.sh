#!/usr/bin/env bash
# External display modules for the V67 phone kernel (Image unchanged): generated LCD panel driver, generated e-ink DSI
# panel driver (reference only), and the mainline TC358762 bridge with the A6L stock register values. Build only.
set -euo pipefail
W=/mnt/c/Users/Pierre/Desktop/A6L; K=/home/a6l/kernel/a6l-baseline-7.2; O=/home/a6l/kernel/out-a6l-phone-v67
M=/home/a6l/kernel/a6l-display-modules-v67; A=$W/firmware/extracted/phone-kernel-v67-candidate-20260919/display-modules
test "$(git -C $K rev-parse HEAD)" = e47d622cb6d2440a9eacdc8bb2df32c037bec7b8
rm -rf $M; mkdir -p $M $A; cp $W/device/hisense/a6l/kernel/panels/*.c $M/; cp $K/drivers/gpu/drm/bridge/tc358762.c $M/tc358762-a6l.c
# stock I2C init table (docs/eink-transport-tc358762-20260920.md): CLRSIPOCOUNT 4, SPICMR 0x60, SYSCTRL 0x205; LCDCTRL stock = 0x150 (no VSDELAY)
sed -i 's/PPI_D0S_CLRSIPOCOUNT, 5)/PPI_D0S_CLRSIPOCOUNT, 4)/; s/PPI_D1S_CLRSIPOCOUNT, 5)/PPI_D1S_CLRSIPOCOUNT, 4)/; s/tc358762_write(ctx, SPICMR, 0x00)/tc358762_write(ctx, SPICMR, 0x60)/; s/tc358762_write(ctx, SYSCTRL, 0x040f)/tc358762_write(ctx, SYSCTRL, 0x0205)/; s/lcdctrl = LCDCTRL_VSDELAY(1) |/lcdctrl = LCDCTRL_VSDELAY(0) |/' $M/tc358762-a6l.c
grep -c "SPICMR, 0x60\|SYSCTRL, 0x0205\|CLRSIPOCOUNT, 4" $M/tc358762-a6l.c
printf 'obj-m += panel-ft8719-tianma-1080x2340.o panel-epd-eink.o tc358762-a6l.o panel-a6l-epd.o\n' > $M/Makefile
export PATH=/home/a6l/android/a6l-lineage24/prebuilts/clang/host/linux-x86/clang-r584948/bin:$PATH KBUILD_BUILD_USER=a6l KBUILD_BUILD_HOST=a6l-build KBUILD_BUILD_TIMESTAMP='2026-09-14 00:00:00 UTC'
make -C $K O=$O ARCH=arm64 LLVM=1 -j8 M=$M modules > $M/build.log 2>&1 || { grep -a "error" $M/build.log | head; exit 1; }
for m in panel-ft8719-tianma-1080x2340 panel-epd-eink tc358762-a6l panel-a6l-epd; do llvm-strip --strip-debug -o $M/$m.s.ko $M/$m.ko; cat $M/$m.s.ko > $A/$m.ko; done
( cd $A && sha256sum *.ko > SHA256SUMS ); sed -n 128,140p $M/tc358762-a6l.c; echo A6L_DISPLAY_MODULES_V67_PASS
