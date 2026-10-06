#!/usr/bin/env bash
# test_check_cpr_dt.sh (cpufreq-watchdog agent, 29 Sep 2026): WSL host test for check_cpr_dt.py.
# Merges a6l-cpufreq-f2a and a6l-cpufreq-v75 (+ a6l-thermal-cooling-v75) onto the r5 boot DTB, checks both with their caps,
# then 5 mutations that must be caught. Prints A6L_CPR_DT_TEST PASS.
set -uo pipefail
R=/mnt/c/Users/Pierre/Desktop/A6L; KD=$R/device/hisense/a6l/kernel; C=$KD/cpr-sdm660/check_cpr_dt.py
KB=/home/a6l/kernel/a6l-baseline-7.2; DTC=/home/a6l/kernel/out-a6l-phone-v67/scripts/dtc/dtc
BASE=${A6L_CPR_TEST_BASE:-/home/a6l/rom-v2/boot-r5/rom-v2.dtb}; ST=$R/firmware/extracted/device-trees/stock-00.dts
T=$(mktemp -d); fail=0
ovl() { tr -d '\r' < $KD/$1.dtso > $T/$1.dtso
  cpp -nostdinc -undef -D__DTS__ -x assembler-with-cpp -I $KB/include -I $KB/arch/arm64/boot/dts $T/$1.dtso -o $T/$1.pp
  $DTC -@ -q -I dts -O dtb -o $T/$1.dtbo $T/$1.pp; }
for o in a6l-cpufreq-f2a a6l-cpufreq-v75 a6l-thermal-cooling-v75; do ovl $o; done
fdtoverlay -i $BASE -o $T/f2a0.dtb $T/a6l-cpufreq-f2a.dtbo && fdtoverlay -i $T/f2a0.dtb -o $T/f2a.dtb $T/a6l-thermal-cooling-v75.dtbo || { echo "FAIL merge f2a"; exit 1; }
fdtoverlay -i $BASE -o $T/v750.dtb $T/a6l-cpufreq-v75.dtbo && fdtoverlay -i $T/v750.dtb -o $T/v75.dtb $T/a6l-thermal-cooling-v75.dtbo || { echo "FAIL merge v75"; exit 1; }
exp() { local want=$1 name=$2; shift 2; out=$(python3 $C --stock $ST "$@" 2>&1); rc=$?
  if { [ $want = pass ] && [ $rc = 0 ]; } || { [ $want = fail ] && [ $rc != 0 ]; }; then echo "ok   $name ($(echo "$out" | tail -1))"
  else echo "BAD  $name rc=$rc"; echo "$out" | tail -15; fail=1; fi; }
exp pass f2a --dtb $T/f2a.dtb --max-pwr 902400 --max-perf 1113600
python3 $C --stock $ST --dtb $T/f2a.dtb --max-pwr 902400 --max-perf 1113600 | grep -E 'kHz|level|cpu@|cells' | sed 's/^/     /'
exp pass v75 --dtb $T/v75.dtb --max-pwr 1536000 --max-perf 1747200
exp fail "v75 over the f2a cap" --dtb $T/v75.dtb --max-pwr 902400 --max-perf 1113600
m() { cp $T/f2a.dtb $T/m.dtb; "$@"; }
P=$(fdtget $T/f2a.dtb /__symbols__ pwrcl_opp_table); Q=$(fdtget $T/f2a.dtb /__symbols__ cprh_opp_table); F=$(fdtget $T/f2a.dtb /__symbols__ qfprom)
m fdtput -t x $T/m.dtb $P/opp-633600000 qcom,pll-override 0x3200021; exp fail "wrong pll-override" --dtb $T/m.dtb --max-pwr 902400 --max-perf 1113600
m fdtput -t u $T/m.dtb $Q/opp-3 qcom,opp-fuse-level 3 2; exp fail "wrong fuse level" --dtb $T/m.dtb --max-pwr 902400 --max-perf 1113600
m fdtput -t x $T/m.dtb /cpus/cpu@0 qcom,freq-domain $(fdtget -t x $T/f2a.dtb /cpus/cpu@0 qcom,freq-domain | awk '{print $1" 1"}'); exp fail "cpu@0 on the perf domain" --dtb $T/m.dtb --max-pwr 902400 --max-perf 1113600
m fdtput -t x $T/m.dtb $F/cpr-speedbin@4133 reg 0x40f0 1; exp fail "nvmem cell in row 30" --dtb $T/m.dtb --max-pwr 902400 --max-perf 1113600
m fdtput -t x $T/m.dtb $P/opp-902400000 required-opps $(fdtget -t x $T/f2a.dtb $Q/opp-2 phandle); exp fail "wrong corner" --dtb $T/m.dtb --max-pwr 902400 --max-perf 1113600
rm -rf $T
[ $fail = 0 ] && echo "A6L_CPR_DT_TEST PASS" || { echo "A6L_CPR_DT_TEST FAIL"; exit 1; }
