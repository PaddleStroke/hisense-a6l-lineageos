#!/usr/bin/env bash
# build-cpr-r5.sh (cpufreq-watchdog agent, 29 Sep 2026) - r5 kernel + CPRh/OSM cpufreq (SDM660), for an ATTENDED RAM-boot
# test only. Never flashed by any tool; the ROM keeps the plain r5/V67 kernel. docs/cpufreq-watchdog-20260929.md.
# WSL only. Separate source copy + out dir (the r5 tree/out and other workers' dirs are never written):
#   src  /home/a6l/kernel/a6l-rom-r5-cpr-src = copy of a6l-rom-r5-src + device/hisense/a6l/kernel/cpr-sdm660/r5/*.patch
#   out  /home/a6l/kernel/out-a6l-rom-r5-cpr (r5 config + CONFIG_QCOM_CPR3=y + kernel/configs/a6l-thermal.config)
#   work /home/a6l/kernel/cpr-r5-work        (dtbs, boot images, bundle) -> copied to firmware/extracted/cpr-r5-20260929
# usage: build-cpr-r5.sh <phase...>   phases: src config build check dt boot bundle   (long ones: run under setsid + poll)
set -euo pipefail
R=/mnt/c/Users/Pierre/Desktop/A6L; KD=$R/device/hisense/a6l/kernel; PD=$KD/cpr-sdm660/r5
S5=/home/a6l/kernel/a6l-rom-r5-src; K5=$R/firmware/extracted/kernel-r5-20260930
S=/home/a6l/kernel/a6l-rom-r5-cpr-src; O=/home/a6l/kernel/out-a6l-rom-r5-cpr; W=/home/a6l/kernel/cpr-r5-work
X=$R/firmware/extracted/cpr-r5-20260929; B5=${A6L_CPR_BOOT:-/home/a6l/rom-v2/boot-r5}   # A6L_CPR_BOOT=/home/a6l/rom-v2/boot-r6 once r6 (A6L_KERNEL=r5) is built
LT=/home/a6l/android/a6l-lineage24; PACK=$LT/system/tools/mkbootimg
export PATH=$LT/prebuilts/clang/host/linux-x86/clang-r584948/bin:$PATH
export LOCALVERSION=+ KBUILD_BUILD_USER=a6l KBUILD_BUILD_HOST=a6l-build KBUILD_BUILD_TIMESTAMP='2026-09-30 00:00:00 UTC'
MK="make -C $S O=$O ARCH=arm64 LLVM=1"
mkdir -p $W
for ph in "$@"; do case $ph in
src)
  [ -f $S/.a6l-cpr-r5 ] && { echo "src exists ($S), skipped"; continue; }
  [ -e $S ] && { echo "refusing: $S exists without marker"; exit 2; }
  cp -a $S5 $S.tmp
  for p in $PD/0*.patch; do patch -p1 -s -d $S.tmp < $p || { echo "A6L_CPR_R5_PATCH_FAIL $p"; exit 3; }; echo "applied ${p##*/}"; done
  install -m 644 $PD/cpr.h $S.tmp/include/soc/qcom/cpr.h
  sha256sum $PD/0*.patch $PD/cpr.h | sed "s|$PD/||" > $S.tmp/.a6l-cpr-r5; mv $S.tmp $S; echo A6L_CPR_R5_SRC_PASS ;;
config)
  mkdir -p $O; cp $K5/config $O/.config
  $S/scripts/config --file $O/.config -e QCOM_CPR3 -e QCOM_CPR_COMMON --set-val THERMAL_EMERGENCY_POWEROFF_DELAY_MS 100
  $MK olddefconfig > $W/olddefconfig.log 2>&1
  for c in CONFIG_QCOM_CPR3=y CONFIG_QCOM_CPR_COMMON=y CONFIG_ARM_QCOM_CPUFREQ_HW=y CONFIG_CPUFREQ_DT=y CONFIG_MODVERSIONS=y \
           CONFIG_THERMAL_EMERGENCY_POWEROFF_DELAY_MS=100 CONFIG_CPU_FREQ_GOV_USERSPACE=m CONFIG_QCOM_WDT=m CONFIG_LOCALVERSION=\"-a6l-probe\"; do
    grep -qx "$c" $O/.config || { echo "A6L_CPR_R5_CONFIG_FAIL missing $c"; exit 4; }; done
  diff <(grep '^CONFIG' $K5/config | sort) <(grep '^CONFIG' $O/.config | sort) > $W/config-diff-r5.txt || true
  cat $W/config-diff-r5.txt; echo A6L_CPR_R5_CONFIG_PASS ;;
build)
  # Image only: the ROM modules are the r5 set (kernel-r5-20260930/modules); `check` proves their CRCs still match.
  nice -n 10 $MK -j12 Image.gz > $W/image-build.log 2>&1 || { tail -30 $W/image-build.log; echo A6L_CPR_R5_BUILD_FAIL; exit 5; }
  echo "Image build warnings: $(grep -c 'warning:' $W/image-build.log || true)"
  # W=1 on the CPR objects only (the whole tree is not W=1 clean upstream)
  touch $S/drivers/pmdomain/qcom/cpr3.c $S/drivers/pmdomain/qcom/cpr-common.c $S/drivers/cpufreq/qcom-cpufreq-hw.c $S/drivers/cpufreq/cpufreq-dt-platdev.c
  nice -n 10 $MK -j12 W=1 drivers/pmdomain/qcom/cpr3.o drivers/pmdomain/qcom/cpr-common.o drivers/cpufreq/qcom-cpufreq-hw.o \
     drivers/cpufreq/cpufreq-dt-platdev.o drivers/opp/debugfs.o > $W/w1.log 2>&1 || { tail -30 $W/w1.log; exit 5; }
  echo "W=1 CPR objects: $(grep -c 'warning:' $W/w1.log || true) warnings"; grep 'warning:' $W/w1.log | head -20 || true
  nice -n 10 $MK -j12 Image.gz > $W/image-build2.log 2>&1   # relink after the touch (same config, no-op for other objects)
  echo A6L_CPR_R5_BUILD_PASS ;;
check)
  ver=$(grep -a -o -m1 'Linux version [^ ]*' $O/arch/arm64/boot/Image | cut -d' ' -f3); echo "kernel release $ver"
  [ "$ver" = 7.2.3-a6l-probe+ ] || { echo "A6L_CPR_R5_CHECK_FAIL release $ver"; exit 6; }
  # every vmlinux export imported by the 124 ROM modules (and every export r5 had) must keep its CRC
  python3 - $K5/Module.symvers $O/vmlinux.symvers $K5/modules <<'PY'
import sys, os, subprocess
old = {l.split('\t')[1]: l.split('\t')[0] for l in open(sys.argv[1]) if l.split('\t')[2] == 'vmlinux'}
new = {l.split('\t')[1]: l.split('\t')[0] for l in open(sys.argv[2])}
missing = [s for s in old if s not in new]; diff = [s for s in old if s in new and new[s] != old[s]]
added = sorted(s for s in new if s not in old)
print(f"r5 vmlinux exports {len(old)}; cpr exports {len(new)}; missing {len(missing)}; crc changed {len(diff)}; added {len(added)}: {' '.join(added)[:400]}")
for s in (missing + diff)[:20]: print('  BAD', s, old.get(s), new.get(s))
sys.exit(1 if missing or diff else 0)
PY
  echo A6L_CPR_R5_CRC_PASS
  for sy in cpr_get_fuses sdm660_cpr_desc; do grep -q " $sy\$" $O/System.map && echo "vmlinux has $sy" || { echo "A6L_CPR_R5_CHECK_FAIL $sy not in vmlinux"; exit 6; }; done
  grep -a -q 'qcom,sdm660-cpufreq-hw' $O/drivers/cpufreq/qcom-cpufreq-hw.o && echo "cpufreq-hw matches qcom,sdm660-cpufreq-hw"
  grep -a -q 'qcom,sdm660-cprh' $O/drivers/pmdomain/qcom/cpr3.o && echo "cpr3 matches qcom,sdm660-cprh"
  grep -a -q 'qcom,sdm660' $O/drivers/cpufreq/cpufreq-dt-platdev.o && echo "cpufreq-dt-platdev blocklists qcom,sdm660"
  echo A6L_CPR_R5_CHECK_PASS ;;
dt)
  # C0 = rom-v2.dtb as built for r5 (no CPR node: the CPR kernel must behave exactly like r5)
  # F2a = rom-v2 + a6l-cpufreq-f2a (3 pwrcl + 2 perfcl lowest corners, open loop) + a6l-thermal-cooling-v75
  bash $R/tools/build-rom-v2-dt.sh $W/dt > $W/dt.log 2>&1 || { tail $W/dt.log; exit 7; }
  cmp $W/dt/rom-v2.dtb $B5/rom-v2.dtb && echo "rom-v2.dtb == boot-r5 dtb" || echo "NOTE rom-v2.dtb differs from boot-r5/rom-v2.dtb (DT changed since r5; C0 uses the boot-r5 one)"
  cp $B5/rom-v2.dtb $W/c0.dtb
  KB=/home/a6l/kernel/a6l-baseline-7.2; DTC=/home/a6l/kernel/out-a6l-phone-v67/scripts/dtc/dtc; cur=$W/c0.dtb
  for o in a6l-cpufreq-f2a a6l-thermal-cooling-v75; do
    tr -d '\r' < $KD/$o.dtso > $W/$o.dtso
    cpp -nostdinc -undef -D__DTS__ -x assembler-with-cpp -I $KB/include -I $KB/arch/arm64/boot/dts $W/$o.dtso -o $W/$o.pp
    $DTC -@ -q -I dts -O dtb -o $W/$o.dtbo $W/$o.pp
    fdtoverlay -i $cur -o $W/next.dtb $W/$o.dtbo || { echo "A6L_CPR_R5_DT_FAIL $o"; exit 7; }; cur=$W/f2a.dtb; mv $W/next.dtb $cur
  done
  fdtput -t s $W/f2a.dtb /chosen hisense,a6l-image rom-v2-cpr-f2a
  for p in /soc@0/cpufreq@179c1000 /soc@0/power-controller@179c8000; do echo "$p compatible=$(fdtget $W/f2a.dtb $p compatible | tr '\n' ' ')"; done
  python3 $KD/cpr-sdm660/check_cpr_dt.py --stock $R/firmware/extracted/device-trees/stock-00.dts --dtb $W/f2a.dtb --max-pwr 902400 --max-perf 1113600
  echo A6L_CPR_R5_DT_PASS ;;
boot)
  # same ramdisk/cmdline/header as boot-r5 (ae05419b...), only the kernel (+appended DTB) changes; `fastboot boot` only
  mapfile -d '' -t A < <(python3 $PACK/unpack_bootimg.py --boot_img $B5/boot-body.img --out $W/r5-parts --format mkbootimg -0)
  echo "boot-r5 args: ${#A[@]} fields"
  for v in c0 f2a; do
    { gzip -n -9 -c $O/arch/arm64/boot/Image; cat $W/$v.dtb; } > $W/Image.gz-dtb-$v
    args=(); i=0; while [ $i -lt ${#A[@]} ]; do a=${A[$i]}; if [ "$a" = --kernel ]; then args+=(--kernel $W/Image.gz-dtb-$v); i=$((i+2)); continue; fi; args+=("$a"); i=$((i+1)); done
    python3 $PACK/mkbootimg.py "${args[@]}" --output $W/boot-r5-cpr-$v.img
    python3 - $B5/boot-body.img $W/boot-r5-cpr-$v.img <<'PY'
import sys
a = open(sys.argv[1], 'rb').read(); b = bytearray(open(sys.argv[2], 'rb').read()); b[28:32] = a[28:32]; open(sys.argv[2], 'wb').write(b)
ha, hb = bytearray(a[:1648]), bytearray(b[:1648])
for s, e in [(8, 12), (576, 608)]: ha[s:e] = hb[s:e] = bytes(e - s)   # only the kernel size and the id hash may differ
assert ha == hb, 'boot header differs from boot-r5 beyond the kernel size'
assert len(b) < 64 << 20
PY
    echo "boot-r5-cpr-$v.img $(stat -c %s $W/boot-r5-cpr-$v.img) $(sha256sum < $W/boot-r5-cpr-$v.img | cut -c1-16)"
  done; echo A6L_CPR_R5_BOOT_PASS ;;
bundle)
  mkdir -p $X
  cp $O/arch/arm64/boot/Image.gz $W/c0.dtb $W/f2a.dtb $W/a6l-cpufreq-f2a.dtbo $W/a6l-thermal-cooling-v75.dtbo \
     $W/boot-r5-cpr-c0.img $W/boot-r5-cpr-f2a.img $W/config-diff-r5.txt $X/
  cp $O/.config $X/config; cp $O/vmlinux.symvers $X/; cp $R/device/hisense/a6l/power/cpr-r5/run-cpr-r5.sh $X/
  cp $KD/cpr-sdm660/a6l_cpr_openloop.py $X/; cp $R/device/hisense/a6l/power/stock/a6l-stock-cpr-read.sh $X/
  cp $R/firmware/extracted/power-20260929/a6l_fuserows $R/firmware/extracted/power-20260929/run-power.sh $X/
  (cd $X && sha256sum Image.gz *.dtb *.dtbo *.img run-cpr-r5.sh a6l_cpr_openloop.py a6l-stock-cpr-read.sh a6l_fuserows run-power.sh config > SHA256SUMS)
  cat $X/SHA256SUMS; echo A6L_CPR_R5_BUNDLE_PASS ;;
*) echo "unknown phase $ph"; exit 2 ;;
esac; done
