#!/usr/bin/env bash
# agent power, 26 Sep 2026. OFFLINE build + checks (nothing touches the phone). Run from WSL via nohup (relay power-1x).
# Builds: a6l_chg_ovl.ko (charger C2 runtime overlay), a6l_fuserows (NDK static), charger/cpufreq-F2a overlays + merge
# checks on the V74 base, copies the v67 charger modules, runs the offline tests, assembles firmware/extracted/power-20260926.
# pwr27 (27 Sep 2026): charger overlays modify the existing a6l_battery/pm660_* nodes by label (the runtime overlay
# failed on the phone: /battery re-declared with a phandle -> -EINVAL), new runtime-apply check against the V74 base
# (check-runtime-overlay.py, also proves the 26 Sep dtbo fails), A6L-patched qcom_smbx.ko (fcc_max_ua, jeita_hard,
# float clamp 4.40 V) built out-of-tree from the v67 tree. Output goes to firmware/extracted/power-20260927 (the
# 26 Sep set is kept untouched); build dir /home/a6l/pwr27/build.
set -uo pipefail
R=/mnt/c/Users/Pierre/Desktop/A6L; B=/home/a6l/pwr27/build; K=/home/a6l/kernel/a6l-baseline-7.2; O=/home/a6l/kernel/out-a6l-phone-v67
L=/home/a6l/android/a6l-lineage24; CL=$L/prebuilts/clang/host/linux-x86/clang-r584948/bin
export PATH=$CL:$PATH KBUILD_BUILD_USER=a6l KBUILD_BUILD_HOST=a6l-build PYTHONDONTWRITEBYTECODE=1
NDK=/home/a6l/ndk/android-ndk-r27c/toolchains/llvm/prebuilt/linux-x86_64/bin
DTC=$O/scripts/dtc/dtc; [ -x $DTC ] || DTC=dtc
BASE=$R/firmware/extracted/recovery-v74-candidate-20260923/base.dtb
KA=$R/device/hisense/a6l/kernel; PW=$R/device/hisense/a6l/power
rm -rf $B; mkdir -p $B; FAIL=""
f() { FAIL="$FAIL $1"; echo "STEP_FAIL $1"; }
stripcp() { llvm-strip --strip-debug -o $B/.s.ko "$1" && cp $B/.s.ko "$2"; }
hdr() { { echo "/* generated from $(basename $1) by tools/build-power-v75.sh */"; echo "static const unsigned char $2[] __aligned(8) = {"; xxd -i < $1; echo "};"; } > $3; }
kmod() { make -C $K O=$O ARCH=arm64 LLVM=1 W=1 M=$1 modules > $1/build.log 2>&1 || { grep -a -B2 -A8 "error" $1/build.log | head -30; return 1; }; echo "warnings: $(grep -a -c 'warning:' $1/build.log)"; grep -a "warning:" $1/build.log | head -5; return 0; }
dtbo() { cpp -nostdinc -undef -D__DTS__ -x assembler-with-cpp -I $K/include -I $K/arch/arm64/boot/dts $1 -o $2.pp && $DTC -@ -q -I dts -O dtb -o $2 $2.pp; }
st() { fdtget -t s "$1" "$2" status 2>/dev/null || echo "(no status)"; }
echo "== 0. base $(sha256sum $BASE | cut -c1-16)"
dtc -I dtb -O dts $BASE 2>/dev/null > $B/base.dts
grep -n "simple-battery" $B/base.dts | head -3 || true
echo "== 1. charger overlays"
C=$B/chg; mkdir -p $C/ovl
sed 's/\r$//' $KA/a6l-charger-v75.dtso > $C/a6l-charger-v75.dtso; dtbo $C/a6l-charger-v75.dtso $C/a6l-charger-v75.dtbo || f chg-dtbo
sed 's/\r$//' $KA/power/ovl/a6l-charger-test.dtso > $C/a6l-charger-test.dtso; dtbo $C/a6l-charger-test.dtso $C/a6l-charger-test.dtbo || f chg-test-dtbo
for o in a6l-charger-v75 a6l-charger-test; do
  fdtoverlay -i $BASE -o $C/$o-merged.dtb $C/$o.dtbo || { f $o-merge; continue; }
  P=/soc@0/spmi@800f000/pmic@0
  s="rradc=$(st $C/$o-merged.dtb $P/adc@4500) fg=$(st $C/$o-merged.dtb $P/battery@4000) charger=$(st $C/$o-merged.dtb $P/charger@1000) ccc=$(fdtget $C/$o-merged.dtb /battery constant-charge-current-max-microamp 2>/dev/null)"
  echo "$o: $s"
  case "$s" in *"rradc=okay fg=okay charger=okay"*) echo "A6L_${o}_MERGE_PASS";; *) f $o-status;; esac
done
python3 $PW/tools/check-runtime-overlay.py $BASE $C/a6l-charger-test.dtbo $C/a6l-charger-v75.dtbo || f runtime-overlay
OLD=$R/firmware/extracted/power-20260926/a6l-charger-test.dtbo
[ -f $OLD ] && { python3 $PW/tools/check-runtime-overlay.py $BASE $OLD > $C/old-check.txt 2>&1 && f old-dtbo-should-fail || echo "26 Sep test dtbo correctly flagged: $(grep -c 'kernel -EINVAL' $C/old-check.txt) kernel -EINVAL reasons"; }
fdtget -t x $C/a6l-charger-v75-merged.dtb /soc@0/spmi@800f000/pmic@0/battery@4000 power-supplies >/dev/null 2>&1 && echo "fg power-supplies ok" || f fg-power-supplies
echo "== 2. a6l_chg_ovl.ko"
cp $KA/power/ovl/a6l_chg_ovl.c $KA/power/ovl/Kbuild $C/ovl/; sed -i 's/\r$//' $C/ovl/*
hdr $C/a6l-charger-test.dtbo a6l_chg_dtbo $C/ovl/a6l_chg_dtbo.h
kmod $C/ovl || f chg-ovl
echo "== 2b. qcom_smbx.ko (A6L patch: fcc_max_ua / jeita_hard / float clamp)"
SM=$B/smbx; mkdir -p $SM/a; cp $K/drivers/power/supply/qcom_smbx.c $SM/a/; tr -d '\r' < $KA/power/smbx/qcom_smbx-a6l-fcc-jeita.patch > $SM/p.patch
(cd $SM/a && patch -p4 --no-backup-if-mismatch < $SM/p.patch > /dev/null) || f smbx-patch
printf 'obj-m += qcom_smbx.o\n' > $SM/a/Kbuild; kmod $SM/a || f smbx-build
modinfo -F parm $SM/a/qcom_smbx.ko | grep -q fcc_max_ua || f smbx-parm
echo "== 3. cpufreq F2a overlay (3+2 corners)"
F=$B/cpu; mkdir -p $F
for o in a6l-cpufreq-f2a a6l-cpufreq-v75; do
  sed 's/\r$//' $KA/$o.dtso > $F/$o.dtso; dtbo $F/$o.dtso $F/$o.dtbo || { f $o-dtbo; continue; }
  fdtoverlay -i $BASE -o $F/$o-merged.dtb $F/$o.dtbo && echo "A6L_${o}_MERGE_PASS opp: pwr=$(dtc -I dtb -O dts $F/$o-merged.dtb 2>/dev/null | awk '/opp-table-pwrcl/,/^\t};/' | grep -c opp-hz) perf=$(dtc -I dtb -O dts $F/$o-merged.dtb 2>/dev/null | awk '/opp-table-perfcl/,/^\t};/' | grep -c opp-hz)" || f $o-merge
done
echo "== 4. a6l_fuserows (NDK static)"
$NDK/aarch64-linux-android34-clang -O2 -Wall -Wextra -Werror -static $PW/tools/a6l_fuserows.c -o $B/a6l_fuserows && $NDK/llvm-strip $B/a6l_fuserows || f fuserows
gcc -O2 -Wall -Wextra -Werror $PW/tools/a6l_fuserows.c -o $B/fr-host || f fuserows-host
python3 -c "open('$B/fakenv','wb').write(bytes((i*7)&0xff for i in range(0x621c)))"
$B/fr-host $B/fr.bin $B/fakenv | tail -1
python3 - $B/fr.bin <<'PY' || f fuserows-layout
import sys
b=open(sys.argv[1],'rb').read(); assert len(b)==576
for r in range(72):
    exp = bytes(((0x4000+r*8+i)*7)&0xff for i in range(8)) if r in (38,65,66,67,68,69,70,71) else bytes(8)
    assert b[r*8:r*8+8]==exp, r
print("A6L_FUSEROWS_LAYOUT_PASS")
PY
echo "== 5. offline tests"
T=$B/t; mkdir -p $T; cp $KA/cpr-sdm660/a6l_cpr_openloop.py $KA/cpr-sdm660/test_openloop_stock.py $T/; sed -i 's/\r$//' $T/*.py
python3 $T/test_openloop_stock.py | tail -1 | tee $T/r1; grep -q PASS $T/r1 || f openloop-selftest
python3 $T/a6l_cpr_openloop.py $B/fr.bin --offset 0 | tail -1
sed 's/\r$//' $PW/bundle/run-power.sh > $T/run-power.sh; sed 's/\r$//' $PW/rom/a6l-chg-guard.sh > $T/a6l-chg-guard.sh; sed 's/\r$//' $PW/stock/a6l-stock-cpr-read.sh > $T/a6l-stock-cpr-read.sh
for s in run-power.sh a6l-chg-guard.sh a6l-stock-cpr-read.sh; do dash -n $T/$s && bash -n $T/$s || f syntax-$s; done
mkdir -p $T/p/bundle $T/p/rom; cp $T/run-power.sh $T/p/bundle/; cp $T/a6l-chg-guard.sh $T/p/rom/
sed 's/\r$//' $PW/test/sim-power.sh > $T/sim-power.sh; bash $T/sim-power.sh $T/p | tail -1 | tee $T/r2; grep -q PASS $T/r2 || f power-sim
echo "== 6. assemble"
A=$R/firmware/extracted/power-20260927; mkdir -p $A/stock
stripcp $C/ovl/a6l_chg_ovl.ko $A/a6l_chg_ovl.ko
stripcp $SM/a/qcom_smbx.ko $A/qcom_smbx.ko
for m in drivers/power/supply/pmi8998_fg.ko drivers/iio/adc/qcom-spmi-rradc.ko; do stripcp $O/modinst/$m $A/$(basename $m); done
cp $B/a6l_fuserows $T/run-power.sh $A/; cp $C/a6l-charger-v75.dtbo $C/a6l-charger-test.dtbo $F/a6l-cpufreq-f2a.dtbo $A/
cp $T/a6l_cpr_openloop.py $T/test_openloop_stock.py $A/; cp $T/a6l-stock-cpr-read.sh $A/stock/
for m in $A/*.ko; do echo "$(basename $m): $(modinfo -F vermagic $m) | depends=$(modinfo -F depends $m)"; done
grep -q "7.2.3-a6l-probe+ SMP preempt mod_unload aarch64" <(modinfo -F vermagic $A/a6l_chg_ovl.ko) || echo "NOTE vermagic: $(modinfo -F vermagic $A/a6l_chg_ovl.ko)"
(cd $A && sha256sum *.ko *.dtbo a6l_fuserows run-power.sh a6l_cpr_openloop.py test_openloop_stock.py stock/a6l-stock-cpr-read.sh > SHA256SUMS; cat SHA256SUMS)
echo "RESULT: ${FAIL:-A6L_POWER_BUILD_PASS}"
