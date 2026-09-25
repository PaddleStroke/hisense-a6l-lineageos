#!/bin/bash
# rest agent (24 Sep): build haptics module, a6l_vib, CCI/hi846 modules, DT overlays; CPR tree patch + build.
# Offline only. Output: firmware/extracted/rest-20260924/ ; log in .relay/outbox/rest-05-build-progress.log
set -uo pipefail
exec < /dev/null
W=/mnt/c/Users/Pierre/Desktop/A6L
K=/home/a6l/kernel/a6l-baseline-7.2
O=/home/a6l/kernel/out-a6l-phone-v67
KC=/home/a6l/kernel/a6l-7.2-cpr
OC=/home/a6l/kernel/out-a6l-cpr
CL=/home/a6l/android/a6l-lineage24/prebuilts/clang/host/linux-x86/clang-r584948/bin
NDK=/home/a6l/ndk/android-ndk-r27c/toolchains/llvm/prebuilt/linux-x86_64/bin
A=$W/firmware/extracted/rest-20260924
B=/home/a6l/kernel/rest-v75
export PATH="$CL:$PATH" KBUILD_BUILD_USER=a6l KBUILD_BUILD_HOST=a6l-build
mkdir -p "$A" "$B"
step() { echo; echo "=== $(date +%T) $*"; }

step "1 haptics module"
rm -rf $B/haptics && mkdir -p $B/haptics
cp $W/device/hisense/a6l/kernel/a6l-pm660-haptics/a6l_pm660_haptics.c $B/haptics/
printf 'obj-m += a6l_pm660_haptics.o\n' > $B/haptics/Makefile
make -C $K O=$O ARCH=arm64 LLVM=1 M=$B/haptics modules 2>&1 | tail -5
ls -la $B/haptics/*.ko && echo A6L_HAPTICS_KO_BUILD_PASS
grep -n "HAP_PLAY_DIRECT\|HAP_TYPE_LRA\|HAP_WAVE_SQUARE" $K/include/dt-bindings/input/qcom,spmi-haptics.h

step "2 a6l_vib (NDK static)"
$NDK/aarch64-linux-android34-clang -static -O2 -Wall -Wextra -o $B/a6l_vib $W/device/hisense/a6l/rest/tools/a6l_vib.c 2>&1 && file $B/a6l_vib && echo A6L_VIB_BUILD_PASS

step "3 CCI + hi846 external modules"
rm -rf $B/cam && mkdir -p $B/cam
cp $K/drivers/i2c/busses/i2c-qcom-cci.c $K/drivers/media/i2c/hi846.c $B/cam/
printf 'obj-m += i2c-qcom-cci.o\nobj-m += hi846.o\n' > $B/cam/Makefile
make -C $K O=$O ARCH=arm64 LLVM=1 M=$B/cam modules 2>&1 | grep -E "error|warning|LD|MODPOST" | tail -10
ls -la $B/cam/*.ko && echo A6L_CAM_KO_BUILD_PASS
ls $O/drivers/media/platform/qcom/camss/*.ko $O/drivers/leds/flash/leds-qcom-flash.ko $O/drivers/leds/led-class-flash.ko 2>&1
grep -n '"cam_mclk"' $K/drivers/pinctrl/qcom/pinctrl-sdm660.c | head -3
grep -n "CAMSS_MCLK2_CLK\|HMSS_GPLL0_CLK_SRC\|GCC_HMSS_RBCPR_CLK" $K/include/dt-bindings/clock/qcom,mmcc-sdm660.h $K/include/dt-bindings/clock/qcom,gcc-sdm660.h
grep -n "SDM660_VDDCX_AO\|SDM660_VDDCX\b" $K/include/dt-bindings/power/qcom-rpmpd.h
grep -n "vdda\|supply" $K/drivers/media/platform/qcom/camss/camss.c | grep -i "sdm660\|regulators" | head -5

step "4 overlays"
BASE=$(ls $O/arch/arm64/boot/dts/qcom/sdm660-hisense-a6l-recovery.dtb 2>/dev/null || ls $O/arch/arm64/boot/dts/qcom/sdm660-hisense-a6l*.dtb | head -1)
echo "base dtb: $BASE"
DTC=$O/scripts/dtc/dtc
for n in haptics flash camera-hi846 cpufreq; do
  src=$W/device/hisense/a6l/kernel/a6l-$n-v75.dtso
  cpp -nostdinc -undef -D__DTS__ -x assembler-with-cpp -I $K/include -I $K/arch/arm64/boot/dts $src -o $B/a6l-$n-v75.pp 2>&1
  $DTC -@ -I dts -O dtb -o $B/a6l-$n-v75.dtbo $B/a6l-$n-v75.pp 2>&1 | grep -v "Warning (unit_address" | head -5
  if fdtoverlay -i $BASE -o $B/merged-$n.dtb $B/a6l-$n-v75.dtbo 2>&1; then echo "A6L_OVL_${n}_MERGE_PASS"; else echo "A6L_OVL_${n}_MERGE_FAIL"; fi
done
fdtget -t x $B/merged-haptics.dtb /soc@0/spmi@800f000/pmic@1/vibrator@c000 qcom,brake-pattern 2>&1 || fdtget -l $B/merged-haptics.dtb / | head
cp $B/haptics/a6l_pm660_haptics.ko $B/cam/*.ko $B/a6l_vib $B/*.dtbo $A/ 2>/dev/null
(cd $A && sha256sum *.ko a6l_vib *.dtbo > SHA256SUMS 2>/dev/null; cat SHA256SUMS)

step "5 CPR tree: apply SoMainline topic/cpr3hh onto 7.2 copy"
test -d $KC || { echo "no $KC"; exit 1; }
SRC=/home/a6l/src-cpr/linux; G="git -C $SRC -c safe.directory=*"
if [ ! -e $KC/.rest-cpr-applied ]; then
  for f in drivers/pmdomain/qcom/cpr.c drivers/pmdomain/qcom/cpr-common.c drivers/pmdomain/qcom/cpr-common.h drivers/pmdomain/qcom/cpr3.c \
           Documentation/devicetree/bindings/soc/qcom/qcom,cpr3.yaml; do
    $G show FETCH_HEAD:$f > $KC/$f
  done
  grep -q QCOM_CPR3 $KC/drivers/pmdomain/qcom/Kconfig || python3 - $KC <<'PY'
import sys
k=sys.argv[1]+"/drivers/pmdomain/qcom/Kconfig"; s=open(k).read()
s=s.replace('config QCOM_CPR\n','config QCOM_CPR_COMMON\n\ttristate\n\nconfig QCOM_CPR\n',1)
s=s.replace('\tselect PM_OPP\n','\tselect QCOM_CPR_COMMON\n\tselect PM_OPP\n',1)
i=s.index('config QCOM_RPMHPD')
s=s[:i]+'config QCOM_CPR3\n\ttristate "QCOM Core Power Reduction (CPR v3/v4/Hardened) support"\n\tdepends on ARCH_QCOM && HAS_IOMEM\n\tselect QCOM_CPR_COMMON\n\tselect PM_OPP\n\tselect REGMAP\n\thelp\n\t  CPR3/CPR4/CPRh (SoMainline topic/cpr3hh, forward-ported for A6L).\n\n'+s[i:]
open(k,'w').write(s)
m=sys.argv[1]+"/drivers/pmdomain/qcom/Makefile"; s=open(m).read()
s=s.replace('obj-$(CONFIG_QCOM_CPR)','obj-$(CONFIG_QCOM_CPR_COMMON)\t+= cpr-common.o\nobj-$(CONFIG_QCOM_CPR3)\t\t+= cpr3.o\nobj-$(CONFIG_QCOM_CPR)',1)
open(m,'w').write(s)
PY
  cp $KC/drivers/cpufreq/qcom-cpufreq-hw.c $KC/drivers/cpufreq/qcom-cpufreq-hw.c.72orig
  ok=1
  for c in 3555504e4 b9e8f1bc9 a905deed0 2e185c671; do
    if patch -d $KC -p1 -N --no-backup-if-mismatch -r - < $W/research/rest-20260924/src/cpr3hh/patch-$c.patch > $B/patch-$c.log 2>&1; then echo "patch $c OK"; else echo "patch $c FAILED"; tail -5 $B/patch-$c.log; ok=0; fi
  done
  if [ $ok = 0 ]; then
    echo "cpufreq-hw series does not apply on 7.2 -> taking FETCH_HEAD qcom-cpufreq-hw.c wholesale"
    $G show FETCH_HEAD:drivers/cpufreq/qcom-cpufreq-hw.c > $KC/drivers/cpufreq/qcom-cpufreq-hw.c
    $G show 2e185c671:drivers/cpufreq/qcom-cpufreq-hw.c > $KC/drivers/cpufreq/qcom-cpufreq-hw.c
  fi
  patch -d $KC -p1 -N --no-backup-if-mismatch -r - < $W/research/rest-20260924/src/cpr3hh/patch-e548cfd10.patch > $B/patch-e548.log 2>&1 && echo "patch e548cfd10 OK" || { echo "patch e548cfd10 failed (optional)"; tail -3 $B/patch-e548.log; }
  python3 $W/device/hisense/a6l/kernel/cpr-sdm660/add_sdm660.py $KC
  touch $KC/.rest-cpr-applied
fi
mkdir -p $OC
[ -e $OC/.config ] || cp $O/.config $OC/.config
$KC/scripts/config --file $OC/.config -e QCOM_CPR3 -e ARM_QCOM_CPUFREQ_HW
make -C $KC O=$OC ARCH=arm64 LLVM=1 olddefconfig > /dev/null 2>&1
grep -E "CONFIG_QCOM_CPR3|CONFIG_QCOM_CPR_COMMON|CONFIG_ARM_QCOM_CPUFREQ_HW" $OC/.config
step "5b targeted compile"
make -C $KC O=$OC ARCH=arm64 LLVM=1 -j14 drivers/pmdomain/qcom/ drivers/cpufreq/qcom-cpufreq-hw.o 2>&1 | grep -E "error|warning:" | head -60
ls -la $OC/drivers/pmdomain/qcom/cpr3.o $OC/drivers/cpufreq/qcom-cpufreq-hw.o 2>&1 && echo A6L_CPR_OBJ_BUILD_PASS
step "done (Image build is separate: rest-06)"
