#!/usr/bin/env bash
# misc2 agent, 25 Sep 2026. OFFLINE build (nothing touches the phone). Run from WSL via nohup (relay script misc2-15).
# Builds: rest3 (vibrator), dualux2 (frontlight fix), gnss3 (XTRA fix), audio5 (capture diag + q6adm endpoint_id_2).
set -uo pipefail
R=/mnt/c/Users/Pierre/Desktop/A6L; B=/home/a6l/misc2/build; K=/home/a6l/kernel/a6l-baseline-7.2; O=/home/a6l/kernel/out-a6l-phone-v67
L=/home/a6l/android/a6l-lineage24; CL=$L/prebuilts/clang/host/linux-x86/clang-r584948/bin
export PATH=$CL:$PATH KBUILD_BUILD_USER=a6l KBUILD_BUILD_HOST=a6l-build
NDK=/home/a6l/ndk/android-ndk-r27c/toolchains/llvm/prebuilt/linux-x86_64/bin
DTC=$O/scripts/dtc/dtc; [ -x $DTC ] || DTC=dtc
BASE=$R/firmware/extracted/recovery-v74-candidate-20260923/base.dtb
KA=$R/device/hisense/a6l/kernel
rm -rf $B; mkdir -p $B; FAIL=""
f() { FAIL="$FAIL $1"; echo "STEP_FAIL $1"; }
stripcp() { llvm-strip --strip-debug -o $B/.s.ko "$1" && cp $B/.s.ko "$2"; }
hdr() { { echo "/* generated from $(basename $1) by tools/build-misc2-v75.sh */"; echo "static const unsigned char $2[] __aligned(8) = {"; xxd -i < $1; echo "};"; } > $3; }
kmod() { make -C $K O=$O ARCH=arm64 LLVM=1 W=1 M=$1 modules > $1/build.log 2>&1 || { grep -a -B2 -A8 "error" $1/build.log | head -30; return 1; }; echo "warnings: $(grep -a -c 'warning:' $1/build.log)"; grep -a "warning:" $1/build.log | head -5; return 0; }
dtbo() { cpp -nostdinc -undef -D__DTS__ -x assembler-with-cpp -I $K/include -I $K/arch/arm64/boot/dts $1 -o $2.pp && $DTC -@ -q -I dts -O dtb -o $2 $2.pp; }
echo "== 1. vibrator (rest3)"
V=$B/vib; mkdir -p $V/drv $V/ovl
cp $KA/a6l-gpio-vibrator/a6l_gpio_vib.c $KA/a6l-gpio-vibrator/Kbuild $V/drv/; sed -i 's/\r$//' $V/drv/*
kmod $V/drv || f vib-driver
sed 's/\r$//' $KA/a6l-vibrator-v75.dtso > $V/a6l-vibrator-v75.dtso
python3 - $V/a6l-vibrator-v75.dtso $V/a6l-vibrator-test.dtso <<'PY'
import sys,re
s=open(sys.argv[1]).read()
s=re.sub(r'&pm660_haptics \{\n\tstatus = "disabled";\n\};\n','',s)
assert 'pm660_haptics {' not in s
open(sys.argv[2],'w').write(s)
PY
dtbo $V/a6l-vibrator-v75.dtso $V/a6l-vibrator-v75.dtbo || f vib-dtbo
dtbo $V/a6l-vibrator-test.dtso $V/a6l-vibrator-test.dtbo || f vib-test-dtbo
echo "base: $(sha256sum $BASE | cut -c1-16)"
dtc -I dtb -O dts $BASE 2>/dev/null > $B/base.dts
grep -n "gpio-reserved-ranges" $B/base.dts | head -3
grep -n "gpio79\|\"gpio79\"" $B/base.dts | head -5
grep -n "pm660_haptics\|haptics = \|vibrator@c000" $B/base.dts | head -4
fdtoverlay -i $BASE -o $V/merged.dtb $V/a6l-vibrator-v75.dtbo && echo VIB_MERGE_PASS || f vib-merge
fdtoverlay -i $BASE -o $V/merged-test.dtb $V/a6l-vibrator-test.dtbo && echo VIB_TEST_MERGE_PASS || f vib-test-merge
dtc -I dtb -O dts $V/merged.dtb 2>/dev/null | grep -n -A6 "a6l-vibrator {" | head -8
dtc -I dtb -O dts $V/merged.dtb 2>/dev/null | grep -n -B2 -A3 "a6l-vib-en-state {" | head -8
cp $KA/a6l-gpio-vibrator/ovl/a6l_vib_ovl.c $KA/a6l-gpio-vibrator/ovl/Kbuild $V/ovl/; sed -i 's/\r$//' $V/ovl/*
hdr $V/a6l-vibrator-test.dtbo a6l_vib_dtbo $V/ovl/a6l_vib_dtbo.h
kmod $V/ovl || f vib-ovl
modinfo -F vermagic $V/drv/a6l_gpio_vib.ko; modinfo -F depends $V/drv/a6l_gpio_vib.ko
A=$R/firmware/extracted/rest3-20260925; rm -rf $A; mkdir -p $A
stripcp $V/drv/a6l_gpio_vib.ko $A/a6l_gpio_vib.ko; stripcp $V/ovl/a6l_vib_ovl.ko $A/a6l_vib_ovl.ko
cp $V/a6l-vibrator-v75.dtbo $V/a6l-vibrator-test.dtbo $A/; cp $R/firmware/extracted/rest-20260924b/a6l_vib $A/
sed 's/\r$//' $R/device/hisense/a6l/rest/bundle/run-rest3.sh > $A/run-rest3.sh; bash -n $A/run-rest3.sh || f rest3-syntax
(cd $A && sha256sum *.ko *.dtbo a6l_vib run-rest3.sh > SHA256SUMS; cat SHA256SUMS)

echo "== 2. frontlight (dualux2)"
F=$B/fl; mkdir -p $F
cp $KA/frontlight/ovl/a6l_fl_ovl.c $KA/frontlight/ovl/Kbuild $F/; sed -i 's/\r$//' $F/*
DX=$R/firmware/extracted/dualux-20260925
sed 's/\r$//' $KA/a6l-eink-frontlight-v75.dtso > $F/fl.dtso; dtbo $F/fl.dtso $F/fl-new.dtbo || f fl-dtbo
cmp -s $F/fl-new.dtbo $DX/a6l-eink-frontlight-v75.dtbo && echo "FL_DTBO same as dualux build" || echo "FL_DTBO differs from dualux build (using the new one)"
head -c 4 $F/fl-new.dtbo | xxd | head -1
hdr $F/fl-new.dtbo a6l_fl_dtbo $F/a6l_fl_dtbo.h
kmod $F || f fl-ovl
echo "-- V74 base: pmic@3 children"
python3 - $B/base.dts <<'PY'
import sys,re
s=open(sys.argv[1]).read()
i=s.find('pmic@3 {')
if i<0: print('NO pmic@3'); sys.exit()
depth=0; j=i; out=[]
for line in s[i:].splitlines():
    if depth==1 and re.match(r'\s+[\w@,-]+ \{',line): out.append(line.strip())
    if depth<=1 and ('compatible' in line or 'status' in line): out.append('   '+line.strip())
    depth+=line.count('{')-line.count('}')
    if depth==0: break
print('\n'.join(out[:40]))
PY
fdtoverlay -i $BASE -o $F/merged.dtb $F/fl-new.dtbo && echo FL_MERGE_PASS || f fl-merge
A=$R/firmware/extracted/dualux2-20260925; rm -rf $A; mkdir -p $A
stripcp $F/a6l_fl_ovl.ko $A/a6l_fl_ovl.ko
cp $F/fl-new.dtbo $A/a6l-eink-frontlight-v75.dtbo; cp $DX/leds-qcom-lpg.ko $DX/leds-pwm.ko $DX/led-class-multicolor.ko $A/
sed 's/\r$//' $KA/frontlight/run-frontlight.sh > $A/run-frontlight.sh; bash -n $A/run-frontlight.sh || f fl-syntax
for m in $A/*.ko; do echo "$(basename $m): $(modinfo -F vermagic $m) | depends=$(modinfo -F depends $m)"; done
(cd $A && sha256sum *.ko *.dtbo run-frontlight.sh > SHA256SUMS; cat SHA256SUMS)

echo "== 3. gnss3"
G=$B/gnss; mkdir -p $G; cp -r $R/device/hisense/a6l/gnss/. $G/src/; find $G/src -type f -exec sed -i 's/\r$//' {} +
cd $G/src; SRC="lib/qmi.cpp lib/loc_v02.cpp lib/nmea.cpp lib/loc_client.cpp lib/android_map.cpp lib/qrtr_transport.cpp"
g++ -std=c++17 -O1 -g -Wall -Wextra -Werror -pthread -fsanitize=address,undefined -Ilib test/host_test.cpp $SRC -o $G/h && timeout 120 $G/h | tail -1 || f gnss-asan
g++ -std=c++17 -O1 -g -pthread -fsanitize=thread -Ilib test/host_test.cpp $SRC -o $G/ht && timeout 180 setarch "$(uname -m)" -R $G/ht > $G/tsan.log 2>&1; tail -1 $G/tsan.log
grep -q "WARNING: ThreadSanitizer" $G/tsan.log && { f gnss-tsan; grep -A14 "WARNING: ThreadSanitizer" $G/tsan.log | head -30; }
grep -q "A6L_GNSS_HOST_TESTS PASS" $G/tsan.log || f gnss-tsan-pass
g++ -std=c++17 -O1 -Wall -Wextra -Werror -pthread -Ilib tools/a6l_gnss_test.cpp $SRC -o $G/cli && python3 test/make_replay.py $G/synth.log >/dev/null && timeout 60 $G/cli --replay $G/synth.log --speed 4 --seconds 30 | tail -1 || f gnss-replay
$NDK/aarch64-linux-android34-clang++ -std=c++17 -O2 -Wall -Wextra -Werror -static -static-libstdc++ -Ilib tools/a6l_gnss_test.cpp $SRC -o $G/a6l_gnss_test && $NDK/llvm-strip $G/a6l_gnss_test || f gnss-ndk
INC=$(find $L/out/soong/.intermediates/hardware/interfaces/gnss/aidl -maxdepth 3 -type d -name 'android.hardware.gnss-V7-ndk-source' 2>/dev/null | head -1)
GI=$(find $INC -type d -path '*gen/include' 2>/dev/null | head -1)
[ -n "$GI" ] && { $NDK/aarch64-linux-android34-clang++ -std=c++17 -fsyntax-only -Wall -Wextra -Werror -Wno-deprecated-declarations -Ilib -Ihal -I$GI \
   -I$L/frameworks/native/libs/binder/ndk/include_cpp -I$L/frameworks/native/libs/binder/ndk/include_ndk \
   -I$L/frameworks/native/libs/binder/ndk/include_platform -I$L/system/libbase/include -I$L/system/logging/liblog/include \
   -I$L/system/core/libcutils/include hal/Gnss.cpp 2>&1 | grep -E "error" | head -5; echo "HAL syntax rc=${PIPESTATUS[0]}"; }
bash -n test/gnss3-test.sh || f gnss3-syntax
cd /
A=$R/firmware/extracted/gnss3-20260925; rm -rf $A; mkdir -p $A
X=/home/a6l/misc2/xtra
cp $G/a6l_gnss_test $A/; sed 's/\r$//' $G/src/test/gnss3-test.sh > $A/gnss3-test.sh
cp $X/path1.xtracloud.net-xtra3grcej.bin $A/xtra3grcej.bin; cp $X/path1.xtracloud.net-xtra3grc.bin $A/xtra3grc.bin
cp $R/firmware/extracted/gnss-20260924b/xtra2.bin $A/
(cd $A && sha256sum a6l_gnss_test gnss3-test.sh xtra3grcej.bin xtra3grc.bin xtra2.bin > SHA256SUMS; cat SHA256SUMS)

echo "== 4. audio5 (q6adm endpoint_id_2 + capture mode)"
Q=$B/q6adm/qdsp6; mkdir -p $Q; cp $K/sound/soc/qcom/qdsp6/*.h $Q/; cp $K/sound/soc/qcom/common.h $B/q6adm/
cp $K/sound/soc/qcom/qdsp6/q6adm.c $Q/q6adm.c.orig; cp $Q/q6adm.c.orig $Q/q6adm.c
python3 - $Q/q6adm.c <<'PY'
import sys
p=sys.argv[1]; s=open(p).read()
old="\topen->endpoint_id_1 = afe_port;\n"
new=old+"""\t/*
\t * A6L (misc2, 25 Sep 2026): stock msm-4.4 adm_open() always sends endpoint_id_2 = 0xFFFF ("no second endpoint /
\t * EC reference"). Upstream leaves it 0 (kzalloc) = AFE port 0x0000 (PRIMARY_I2S_RX), which the SDM660 ADSP may
\t * take as the voice-processor Tx reference of a LIVE_REC COPP. Suspect for "capture returns no data".
\t */
\topen->endpoint_id_2 = 0xFFFF;
"""
assert s.count(old)==1; s=s.replace(old,new); open(p,'w').write(s)
PY
(cd $B/q6adm && mkdir -p a/sound/soc/qcom/qdsp6 b/sound/soc/qcom/qdsp6 && cp $Q/q6adm.c.orig a/sound/soc/qcom/qdsp6/q6adm.c && cp $Q/q6adm.c b/sound/soc/qcom/qdsp6/q6adm.c && diff -u a/sound/soc/qcom/qdsp6/q6adm.c b/sound/soc/qcom/qdsp6/q6adm.c > $B/a6l-q6adm-endpoint2-v75.patch; true)
(cd $K && patch -p1 --dry-run -s < $B/a6l-q6adm-endpoint2-v75.patch && echo Q6ADM_PATCH_APPLIES_TO_TREE)
rm -f $Q/q6adm.c.orig; printf 'obj-m += q6adm.o\n' > $Q/Makefile
kmod $Q || f q6adm
A4=$R/firmware/extracted/audio4-20260924/v75/audio4
file $V/drv/a6l_gpio_vib.ko 2>/dev/null | cut -c1-120
echo "q6adm vermagic new: $(modinfo -F vermagic $Q/q6adm.ko) | audio4: $(modinfo -F vermagic $A4/modules/q6adm.ko)"
echo "q6adm depends new: $(modinfo -F depends $Q/q6adm.ko) | audio4: $(modinfo -F depends $A4/modules/q6adm.ko)"
llvm-nm $A4/modules/q6adm.ko | grep " __crc_\| __ksymtab_" | head -3
A=$R/firmware/extracted/audio5-20260925; rm -rf $A; mkdir -p $A/v75 $A/modules
cp -r $A4 $A/v75/audio5; U=$A/v75/audio5; rm -f $U/SHA256SUMS
stripcp $Q/q6adm.ko $U/modules/q6adm.ko; cp $U/modules/q6adm.ko $A/modules/
cp $B/a6l-q6adm-endpoint2-v75.patch $A/; cp $B/a6l-q6adm-endpoint2-v75.patch $KA/audfix/
for m in $KA/audfix/bundle5/mixer3/*; do sed 's/\r$//' $m > $U/mixer3/$(basename $m); done; sed 's/\r$//' $KA/audfix/bundle5/run.sh > $U/run.sh
bash -n $U/run.sh || f audio5-syntax
(cd $U && find . -type f ! -name SHA256SUMS | sed 's|^\./||' | sort | xargs sha256sum > SHA256SUMS; grep -E "q6adm|run.sh|mm1-capture" SHA256SUMS)

echo "== 5. laptop"
SSH="/mnt/c/Windows/System32/OpenSSH/ssh.exe -F C:/Users/Pierre/Desktop/A6L/tools/a6l-laptop-ssh.conf -o ConnectTimeout=10"
SCP="/mnt/c/Windows/System32/OpenSSH/scp.exe -F C:/Users/Pierre/Desktop/A6L/tools/a6l-laptop-ssh.conf -o ConnectTimeout=10"
timeout 60 $SSH a6l-laptop 'mkdir -p A6L-usb-20260915/v75 && cd A6L-usb-20260915/v75 && rm -rf rest3 dualux2 gnss3 audio5 && mkdir rest3 dualux2 gnss3' < /dev/null
for p in rest3:rest3-20260925 dualux2:dualux2-20260925 gnss3:gnss3-20260925; do d=${p%%:*}; s=${p#*:}
  timeout 120 $SCP -q C:/Users/Pierre/Desktop/A6L/firmware/extracted/$s/* a6l-laptop:A6L-usb-20260915/v75/$d/ < /dev/null || f scp-$d; done
timeout 180 $SCP -r -q C:/Users/Pierre/Desktop/A6L/firmware/extracted/audio5-20260925/v75/audio5 a6l-laptop:A6L-usb-20260915/v75/ < /dev/null || f scp-audio5
timeout 60 $SSH a6l-laptop 'cd A6L-usb-20260915/v75; for d in rest3 dualux2 gnss3 audio5; do (cd $d && sha256sum -c --quiet SHA256SUMS && echo "LAPTOP_${d}_OK $(ls | wc -l) files"); done' < /dev/null
echo "FAILED_STEPS:${FAIL:- none}"
[ -z "$FAIL" ] && echo MISC2_BUILD_PASS || echo MISC2_BUILD_FAIL
