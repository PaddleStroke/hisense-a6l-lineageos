#!/usr/bin/env bash
# agent power28 (28 Sep 2026). OFFLINE build + checks for the Quick Charge (HVDCP) test bundle. Nothing touches the phone,
# no ROM image. Run in WSL (relay). Output: firmware/extracted/power-20260928 (= power-20260927 bundle + new qcom_smbx.ko
# (fcc-jeita + hvdcp patches) + run-power.sh with MODE=qc). Build dir /home/a6l/pwr28/build.
set -uo pipefail
R=/mnt/c/Users/Pierre/Desktop/A6L; B=/home/a6l/pwr28/build; K=/home/a6l/kernel/a6l-baseline-7.2; O=/home/a6l/kernel/out-a6l-phone-v67
L=/home/a6l/android/a6l-lineage24; CL=$L/prebuilts/clang/host/linux-x86/clang-r584948/bin
export PATH=$CL:$PATH KBUILD_BUILD_USER=a6l KBUILD_BUILD_HOST=a6l-build PYTHONDONTWRITEBYTECODE=1
D=$R/device/hisense/a6l; SM=$D/kernel/power/smbx; A=$R/firmware/extracted/power-20260928; P27=$R/firmware/extracted/power-20260927
rm -rf $B; mkdir -p $B/a $B/p/bundle $B/p/rom $B/p/test/hvdcp/linux/iio; FAIL=""
f() { FAIL="$FAIL $1"; echo "STEP_FAIL $1"; }

echo "== 1. qcom_smbx.ko = upstream + fcc-jeita + hvdcp (W=1)"
cp $K/drivers/power/supply/qcom_smbx.c $B/a/ && cp $B/a/qcom_smbx.c $B/qcom_smbx.c.orig
for p in qcom_smbx-a6l-fcc-jeita.patch qcom_smbx-a6l-hvdcp.patch; do tr -d '\r' < $SM/$p > $B/$p; (cd $B/a && patch -p4 --no-backup-if-mismatch < $B/$p) || f patch-$p; done
printf 'obj-m += qcom_smbx.o\n' > $B/a/Kbuild
make -C $K O=$O ARCH=arm64 LLVM=1 W=1 M=$B/a modules > $B/build.log 2>&1 || { grep -a -B2 -A8 error $B/build.log | head -30; f smbx-build; }
w=$(grep -a -c 'warning:' $B/build.log); grep -a 'warning:' $B/build.log | grep -v 'compiler differs' | head; echo "W=1 warnings (incl. the 'compiler differs' note): $w"
grep -a 'warning:' $B/build.log | grep -vq 'compiler differs' && f smbx-warnings
mkdir -p $A
if [ -f $B/a/qcom_smbx.ko ]; then
  llvm-strip --strip-debug -o $B/qcom_smbx.ko $B/a/qcom_smbx.ko
  modinfo -F parm $B/qcom_smbx.ko
  for prm in fcc_max_ua jeita_hard hvdcp_enable hvdcp_max_uv hvdcp_icl_ua hvdcp_status; do modinfo -F parm $B/qcom_smbx.ko | grep -q "^$prm:" || f parm-$prm; done
  case "$(modinfo -F vermagic $B/qcom_smbx.ko)" in "7.2.3-a6l-probe+ "*) echo "vermagic OK";; *) f smbx-vermagic;; esac
  for s in $(llvm-nm -u $B/qcom_smbx.ko | awk '{print $2}'); do grep -q " $s$" $O/System.map || grep -qP "\t$s\t" $O/Module.symvers || { echo "  UNRESOLVED $s"; f unresolved-$s; }; done
  llvm-nm -u $B/qcom_smbx.ko | awk '{print $2}' | tr '\n' ' '; echo
fi

echo "== 2. host register-level HVDCP test (gcc ASan/UBSan) + hvdcp_enable=0 trace == r4 driver"
for x in shim.h sim-smb2.c run-hvdcp-sim.sh; do tr -d '\r' < $D/power/test/hvdcp/$x > $B/p/test/hvdcp/$x; done
(cd $D/power/test/hvdcp/linux && for h in *.h; do tr -d '\r' < $h > $B/p/test/hvdcp/linux/$h; done; tr -d '\r' < iio/consumer.h > $B/p/test/hvdcp/linux/iio/consumer.h)
mkdir -p $B/smbx; tr -d '\r' < $SM/qcom_smbx-a6l-fcc-jeita.patch > $B/smbx/qcom_smbx-a6l-fcc-jeita.patch; tr -d '\r' < $SM/qcom_smbx-a6l-hvdcp.patch > $B/smbx/qcom_smbx-a6l-hvdcp.patch
bash $B/p/test/hvdcp/run-hvdcp-sim.sh $B/qcom_smbx.c.orig $B/smbx > $B/hvdcp-sim.log 2>&1 || f hvdcp-sim
grep -E '^(FAIL|A6L_)' $B/hvdcp-sim.log

echo "== 3. power sim (dash): run-power.sh monitors (chg + qc) + ROM guard"
tr -d '\r' < $D/power/bundle/run-power.sh > $B/p/bundle/run-power.sh; tr -d '\r' < $D/power/rom/a6l-chg-guard.sh > $B/p/rom/a6l-chg-guard.sh
tr -d '\r' < $D/power/test/sim-power.sh > $B/p/test/sim-power.sh
(cd $B && bash $B/p/test/sim-power.sh $B/p > $B/sim-power.log 2>&1) || f power-sim
grep -E 'FAIL|A6L_POWER_SIM' $B/sim-power.log
dash -n $B/p/bundle/run-power.sh && bash -n $B/p/bundle/run-power.sh && echo "run-power.sh syntax ok" || f syntax

echo "== 4. bundle power-20260928 (power-20260927 + new qcom_smbx.ko + run-power.sh)"
for x in a6l_chg_ovl.ko pmi8998_fg.ko qcom-spmi-rradc.ko a6l-charger-test.dtbo a6l-charger-v75.dtbo a6l-cpufreq-f2a.dtbo a6l_fuserows a6l_cpr_openloop.py test_openloop_stock.py; do cp $P27/$x $A/$x; done
mkdir -p $A/stock; cp $P27/stock/a6l-stock-cpr-read.sh $A/stock/
cp $B/qcom_smbx.ko $A/qcom_smbx.ko; cp $B/p/bundle/run-power.sh $A/run-power.sh
cp $B/hvdcp-sim.log $B/sim-power.log $A/ ; grep -a -E 'warning|error' $B/build.log > $A/build-warnings.txt || true
(cd $A && sha256sum a6l_chg_ovl.ko pmi8998_fg.ko qcom-spmi-rradc.ko qcom_smbx.ko a6l-charger-test.dtbo a6l-charger-v75.dtbo a6l-cpufreq-f2a.dtbo a6l_fuserows run-power.sh a6l_cpr_openloop.py test_openloop_stock.py stock/a6l-stock-cpr-read.sh > SHA256SUMS; cat SHA256SUMS)
echo "RESULT: $([ -z "$FAIL" ] && echo A6L_POWER28_BUILD_PASS || echo "A6L_POWER28_BUILD_FAIL:$FAIL")"
