#!/usr/bin/env bash
# test-run-cpr-r5.sh (cpufreq-watchdog agent, 29 Sep 2026): host simulation of run-cpr-r5.sh against a fake sysfs
# (A6L_SYSROOT) with a fake OSM (scaling_setspeed -> scaling_cur_freq). Runs under every available POSIX shell
# (dash/sh, busybox sh, mksh). Prints A6L_CPR_R5_SIM PASS.
set -u
H=$(cd "$(dirname "$0")/.." && pwd); S=$H/run-cpr-r5.sh; fail=0
mk() { R=$(mktemp -d); C=$R/sys/devices/system/cpu; mkdir -p $R/proc/device-tree/chosen $R/dt/cpus/cpu@0 $R/dt/cpus/cpu@100 $R/tmp
  printf 'rom-v2-cpr-f2a\0' > $R/proc/device-tree/chosen/hisense,a6l-image; echo "123.45 100.00" > $R/proc/uptime
  B=$R/sys/class/power_supply/qcom-battery; mkdir -p $B; echo 300 > $B/temp; echo 80 > $B/capacity; echo Charging > $B/status
  for z in 0 1; do mkdir -p $R/sys/class/thermal/thermal_zone$z; echo "zone$z" > $R/sys/class/thermal/thermal_zone$z/type; echo 38000 > $R/sys/class/thermal/thermal_zone$z/temp; done
  pol() { p=$C/cpufreq/policy$1; mkdir -p $p; echo "$2" > $p/related_cpus; echo "$3" > $p/scaling_available_frequencies
    echo schedutil > $p/scaling_governor; echo qcom-cpufreq-hw > $p/scaling_driver; echo 300000 > $p/cpuinfo_min_freq
    echo 300000 > $p/scaling_cur_freq; : > $p/scaling_setspeed; for c in $2; do mkdir -p $C/cpu$c; ln -s $R/dt/cpus/$4 $C/cpu$c/of_node; done; }
  pol 0 "0 1 2 3" "300000 633600 902400" cpu@0; pol 4 "4 5 6 7" "300000 1113600" cpu@100
  ( while [ -d $R ]; do for p in $C/cpufreq/policy*; do f=$(cat $p/scaling_setspeed 2>/dev/null); [ -n "$f" ] && { echo $f > $p/.cur && mv $p/.cur $p/scaling_cur_freq; }; echo "$f" >> $p/setspeed.hist 2>/dev/null; : > $p/scaling_setspeed; done; sleep 0.2; done ) >/dev/null 2>&1 &
  OSM=$!; }
rm_() { kill $OSM 2>/dev/null; pkill -9 -f "/cpr-r5/run-cpr-r5.sh" 2>/dev/null; rm -rf $R; }   # no load may survive a case
run() { timeout -k 3 60 env A6L_SYSROOT=$R LOG=$R/tmp/log A6L_DMESG=true HOLD=${HOLD:-1} "$@" $SH $S 2>&1; }
t() { local name=$1 want_rc=$2 want=$3; shift 3; out=$(run "$@"); rc=$?
  if [ $rc = $want_rc ] && echo "$out" | grep -q -- "$want"; then echo "ok   [$SHN] $name"; else echo "BAD  [$SHN] $name rc=$rc (want $want_rc, '$want')"; echo "$out" | tail -8; fail=1; fi; }
SHELLS=""; for s in dash "busybox sh" mksh sh; do set -- $s; command -v $1 >/dev/null && SHELLS="$SHELLS|$s"; done
IFS='|'; for SH in $SHELLS; do [ -z "$SH" ] && continue; unset IFS; SHN=$SH
  mk; t "pre GO" 0 "PRE GO" MODE=pre; rm_
  mk; t "check PASS" 0 "CPR_CHECK PASS" MODE=check; rm_
  mk; echo "300000 633600 902400 1401600" > $C/cpufreq/policy0/scaling_available_frequencies; t "check FAIL above cap" 1 "offers 1401600 kHz > cap 902400" MODE=check; rm_
  mk; printf 'rom-v2\0' > $R/proc/device-tree/chosen/hisense,a6l-image; t "check FAIL plain DT" 1 "not the CPR DT" MODE=check; rm_
  mk; t "step refused unattended" 2 "refused" MODE=step; rm_
  mk; t "step DONE" 0 "CPR_STEP DONE" MODE=step I_AM_ATTENDED=1; sleep 0.6   # let the fake OSM (0.2 s poll) record the last write
    h0=$(grep -v '^$' $C/cpufreq/policy0/setspeed.hist | uniq | tr '\n' ' '); h4=$(grep -v '^$' $C/cpufreq/policy4/setspeed.hist | uniq | tr '\n' ' ')
    [ "$h0" = "300000 633600 902400 300000 " ] && [ "$h4" = "300000 1113600 300000 " ] && echo "ok   [$SHN] step sequence pwr: $h0| perf: $h4" || { echo "BAD  [$SHN] step sequence '$h0' '$h4'"; fail=1; }
    [ "$(cat $C/cpufreq/policy0/scaling_governor)$(cat $C/cpufreq/policy4/scaling_governor)" = schedutilschedutil ] && echo "ok   [$SHN] governors restored" || { echo "BAD  [$SHN] governors not restored"; fail=1; }
    grep -qF "MISMATCH(" $R/tmp/log && { echo "BAD  [$SHN] OSM mismatch reported"; grep -E "STEP|SET" $R/tmp/log | head; fail=1; }; rm_
  mk; t "step cap 633600" 0 "STOP policy0 902400 > cap 633600" MODE=step I_AM_ATTENDED=1 PMAX_PWR=633600; rm_
  mk; ( sleep 1.5; echo 71000 > $R/sys/class/thermal/thermal_zone1/temp ) & t "thermal abort" 3 "ABORT thermal zone zone1 at 71000" MODE=step I_AM_ATTENDED=1 HOLD=3
    [ "$(cat $C/cpufreq/policy0/scaling_governor)" = schedutil ] && [ "$(cat $C/cpufreq/policy0/scaling_cur_freq)" = 300000 ] && echo "ok   [$SHN] abort left policy0 at 300000/schedutil" || { echo "BAD  [$SHN] abort state"; fail=1; }; rm_
  mk; ( sleep 1.5; echo 460 > $R/sys/class/power_supply/qcom-battery/temp ) & t "battery abort" 3 "ABORT battery 460" MODE=step I_AM_ATTENDED=1 HOLD=3; rm_
  mk; echo 420 > $R/sys/class/power_supply/qcom-battery/temp; t "battery NOGO" 1 "NOGO battery at 420" MODE=step I_AM_ATTENDED=1; rm_
  mk; echo 30 > $R/sys/class/power_supply/qcom-battery/capacity; t "capacity NOGO" 1 "NOGO battery 30%" MODE=pre; rm_
  mk; rm $C/cpu4/of_node; t "unknown cluster -> 300 MHz only" 0 "STOP policy4 1113600 > cap 300000" MODE=step I_AM_ATTENDED=1; rm_
  mk; ( i=1; while [ -d $R ] && [ $i -lt 60 ]; do sleep 1; echo "$((123 + i * 20)).0 1.0" > $R/proc/uptime 2>/dev/null; i=$((i+1)); done ) >/dev/null 2>&1 &
    t "soak DONE" 0 "CPR_SOAK DONE" MODE=soak I_AM_ATTENDED=1 MIN=1; rm_
  IFS='|'; done; unset IFS
[ $fail = 0 ] && echo "A6L_CPR_R5_SIM PASS" || { echo "A6L_CPR_R5_SIM FAIL"; exit 1; }
