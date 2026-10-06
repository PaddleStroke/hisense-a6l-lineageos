#!/bin/bash
# flash/microSD agent (28 Sep 2026): builds a6l_flash_ovl.ko + a6l_sd_ovl.ko (v67 kernel, vermagic 7.2.3-a6l-probe+)
# and dtc/fdtoverlay-checks the runtime and ROM overlays against the V74 base DT. Run in WSL.
set -u
R=/mnt/c/Users/Pierre/Desktop/A6L; KD=$R/device/hisense/a6l/kernel
K=/home/a6l/kernel/a6l-baseline-7.2; O=/home/a6l/kernel/out-a6l-phone-v67
BASE=/home/a6l/kernel/kvoice-build/dt/v74.dtb
export PATH=/home/a6l/android/a6l-lineage24/prebuilts/clang/host/linux-x86/clang-r584948/bin:$PATH KBUILD_BUILD_USER=a6l KBUILD_BUILD_HOST=a6l-build
W=/home/a6l/fsd/build; rm -rf $W; mkdir -p $W/flash $W/sd; fail=0
dtbo() {  # src out [cppdefs]
  cpp -nostdinc -undef -D__DTS__ ${3:-} -x assembler-with-cpp -I $K/include -I $K/arch/arm64/boot/dts "$1" -o $W/$2.pp || return 1
  dtc -@ -q -I dts -O dtb -o $W/$2.dtbo $W/$2.pp || return 1
  fdtoverlay -i $BASE -o $W/m-$2.dtb $W/$2.dtbo && echo "MERGE_PASS $2" || { echo "MERGE_FAIL $2"; return 1; }
}
dtbo $KD/flash/ovl/a6l-flash-rt-v75.dtso flash-rt || fail=1
dtbo $KD/a6l-flash-v75.dtso flash-rom || fail=1
for m in 0 1 2; do dtbo $KD/microsd/ovl/a6l-microsd-rt-v75.dtso sd-rt$m -DCD_MODE=$m || fail=1; done
dtbo $KD/a6l-microsd-v75.dtso sd-rom || fail=1
cpp -nostdinc -undef -D__DTS__ -x assembler-with-cpp $KD/microsd/ovl/a6l-microsd-fix-v75.dtso -o $W/sd-fix.pp && dtc -@ -q -I dts -O dtb -o $W/sd-fix.dtbo $W/sd-fix.pp && fdtoverlay -i $W/m-sd-rt0.dtb -o $W/m-sd-fix.dtb $W/sd-fix.dtbo && echo MERGE_PASS sd-fix || { echo MERGE_FAIL sd-fix; fail=1; }
echo "== merged flash-rt"; dtc -I dtb -O dts $W/m-flash-rt.dtb 2>/dev/null | sed -n '/led-controller@d300 {/,/^\t\t\t\t};/p'
echo "== merged sd-rt0"; dtc -I dtb -O dts $W/m-sd-rt0.dtb 2>/dev/null > $W/m-sd-rt0.dts
sed -n '/regulators-a6lsd {/,/^\t\t\t\t};/p' $W/m-sd-rt0.dts
sed -n '/mmc@c084000 {/,/opp-table/p' $W/m-sd-rt0.dts | grep -E "status|supply|cd-gpios|broken-cd|pinctrl"
sed -n '/a6l-sd-cd-state {/,/};/p' $W/m-sd-rt0.dts
for m in 1 2; do echo "sd-rt$m: $(dtc -I dtb -O dts $W/m-sd-rt$m.dtb 2>/dev/null | sed -n '/mmc@c084000 {/,/opp-table/p' | grep -E 'cd-gpios|broken-cd')"; done
hdr() { { echo "/* generated from $2 */"; echo "static const unsigned char $1[] __aligned(8) = {"; xxd -i < $W/$2; echo "};"; }; }
cp $KD/flash/ovl/{Kbuild,a6l_flash_ovl.c,a6l_ovl_reparent.h} $W/flash/; hdr a6l_flash_dtbo flash-rt.dtbo > $W/flash/a6l_flash_dtbo.h
cp $KD/microsd/ovl/{Kbuild,a6l_sd_ovl.c,a6l_ovl_reparent.h} $W/sd/; { for m in 0 1 2; do hdr a6l_sd_dtbo$m sd-rt$m.dtbo; done; hdr a6l_sd_fix_dtbo sd-fix.dtbo; } > $W/sd/a6l_sd_dtbo.h
sed -i 's/\r$//' $W/flash/* $W/sd/*
for d in flash sd; do
  make -C $K O=$O ARCH=arm64 LLVM=1 W=1 M=$W/$d modules > $W/$d.log 2>&1 || { fail=1; tail -20 $W/$d.log; }
  grep -E "warning|error" $W/$d.log | head
done
for k in $W/flash/a6l_flash_ovl.ko $W/sd/a6l_sd_ovl.ko; do
  [ -f $k ] || { fail=1; continue; }; llvm-strip --strip-debug $k
  echo "$(basename $k) $(modinfo -F vermagic $k) depends=$(modinfo -F depends $k) $(sha256sum $k | cut -c1-64)"
done
[ $fail = 0 ] && echo A6L_FLASHSD_BUILD_PASS || echo A6L_FLASHSD_BUILD_FAIL
