#!/bin/bash
# stk agent (25 Sep 2026): build the FRONT STK3338 pieces for 7.2.3-a6l-probe+ (V74 out dir), check the DT overlays,
# stage firmware/extracted/stk-20260925 and laptop ~/A6L-usb-20260915/v75/stk. Run with nohup (log in .relay/outbox).
exec < /dev/null
set -u
W=/mnt/c/Users/Pierre/Desktop/A6L; S=$W/device/hisense/a6l/kernel/stk3338
K=/home/a6l/kernel/a6l-baseline-7.2; O=/home/a6l/kernel/out-a6l-phone-v67
CL=/home/a6l/android/a6l-lineage24/prebuilts/clang/host/linux-x86/clang-r584948/bin
NDK=/home/a6l/ndk/android-ndk-r27c/toolchains/llvm/prebuilt/linux-x86_64/bin
export PATH="$CL:$PATH" KBUILD_BUILD_USER=a6l KBUILD_BUILD_HOST=a6l-build
B=/home/a6l/stk; rm -rf $B; mkdir -p $B/drv $B/ovl
A=$W/firmware/extracted/stk-20260925; mkdir -p $A
BASE=$W/firmware/extracted/ipa-20260924/m-a6l-ipa-v75-on-v74.dtb
fail=0
echo "== driver"; cat $O/include/config/kernel.release
cp $S/stk3338_a6l.c $S/Kbuild $B/drv/; sed -i 's/\r$//' $B/drv/*
make -C $K O=$O ARCH=arm64 LLVM=1 W=1 M=$B/drv modules > $B/drv.log 2>&1 || fail=1
grep -E "error|warning" $B/drv.log | head -20; ls -la $B/drv/*.ko || fail=1
modinfo $B/drv/stk3338_a6l.ko | grep -E "vermagic|alias|parm|depends"
echo "== overlays"
DTC=$O/scripts/dtc/dtc; [ -x $DTC ] || DTC=dtc
for n in a6l-stk3338-v75:$W/device/hisense/a6l/kernel/a6l-stk3338-v75.dtso a6l-stk-power-v74:$S/a6l-stk-power-v74.dtso; do
  name=${n%%:*}; src=${n#*:}
  sed 's/\r$//' $src > $B/$name.dtso
  cpp -nostdinc -undef -D__DTS__ -x assembler-with-cpp -I $K/include -I $K/arch/arm64/boot/dts $B/$name.dtso -o $B/$name.pp
  $DTC -@ -I dts -O dtb -o $B/$name.dtbo $B/$name.pp 2>&1 | grep -v unit_address | head -5
  [ -s $B/$name.dtbo ] || { echo "DTC_FAIL $name"; fail=1; continue; }
  if fdtoverlay -i $BASE -o $B/$name-merged.dtb $B/$name.dtbo; then echo "MERGE_PASS $name"; else echo "MERGE_FAIL $name"; fail=1; fi
done
dtc -I dtb -O dts $B/a6l-stk3338-v75-merged.dtb 2>/dev/null | grep -A14 "light-sensor@47 {" | head -16
dtc -I dtb -O dts $B/a6l-stk-power-v74-merged.dtb 2>/dev/null | grep -A10 "a6l-stk-vdd-vote {" | head -12
cp $S/ovl/a6l_stk_ovl.c $S/ovl/Kbuild $B/ovl/; sed -i 's/\r$//' $B/ovl/*
python3 - $B/a6l-stk-power-v74.dtbo $B/ovl/a6l_stk_dtbo.h <<'PY'
import sys
d=open(sys.argv[1],'rb').read()
o=open(sys.argv[2],'w'); o.write('/* generated from a6l-stk-power-v74.dtbo */\nstatic const unsigned char a6l_stk_dtbo[] __aligned(8) = {\n')
for i in range(0,len(d),12): o.write('\t'+', '.join('0x%02x'%b for b in d[i:i+12])+',\n')
o.write('};\n')
PY
make -C $K O=$O ARCH=arm64 LLVM=1 M=$B/ovl modules > $B/ovl.log 2>&1 || { fail=1; tail -20 $B/ovl.log; }
ls -la $B/ovl/*.ko || fail=1
echo "== tools (NDK static)"
for t in a6l_i2cprobe a6l_iio_ev; do
  sed 's/\r$//' $S/tools/$t.c > $B/$t.c
  $NDK/aarch64-linux-android34-clang -O2 -Wall -static -o $B/$t $B/$t.c 2>&1 | head -10
  $NDK/llvm-strip $B/$t 2>/dev/null; file $B/$t | cut -c1-110; [ -s $B/$t ] || fail=1
done
echo "== stage"
for f in drv/stk3338_a6l.ko ovl/a6l_stk_ovl.ko; do llvm-strip --strip-debug -o $B/$(basename $f) $B/$f; done
cp $B/stk3338_a6l.ko $B/a6l_stk_ovl.ko $B/a6l-stk3338-v75.dtbo $B/a6l-stk-power-v74.dtbo $B/a6l_i2cprobe $B/a6l_iio_ev $A/
sed 's/\r$//' $S/bundle/run-stk.sh > $A/run-stk.sh
(cd $A && sha256sum stk3338_a6l.ko a6l_stk_ovl.ko a6l-stk3338-v75.dtbo a6l-stk-power-v74.dtbo a6l_i2cprobe a6l_iio_ev run-stk.sh > SHA256SUMS; cat SHA256SUMS)
$W/.relay/lap.sh 20 'mkdir -p A6L-usb-20260915/v75/stk && echo mk-ok'
for f in $(cd $A && ls); do timeout 60 /mnt/c/Windows/System32/OpenSSH/scp.exe -F C:/Users/Pierre/Desktop/A6L/tools/a6l-laptop-ssh.conf -o ConnectTimeout=10 "C:/Users/Pierre/Desktop/A6L/firmware/extracted/stk-20260925/$f" a6l-laptop:A6L-usb-20260915/v75/stk/ < /dev/null >/dev/null && echo "scp $f ok"; done
$W/.relay/lap.sh 20 'cd A6L-usb-20260915/v75/stk && sha256sum -c SHA256SUMS'
[ $fail = 0 ] && echo STK_BUILD_PASS || echo STK_BUILD_FAIL
