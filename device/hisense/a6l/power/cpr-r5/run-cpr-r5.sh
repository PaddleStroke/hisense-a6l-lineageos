#!/system/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# run-cpr-r5.sh (cpufreq-watchdog agent, 29 Sep 2026) - ATTENDED CPU frequency scaling test on the r5 ROM kernel + CPRh/OSM
# (tools/build-cpr-r5.sh, docs/cpufreq-watchdog-20260929.md). Run as root from `adb shell` (userdebug ROM, adb root) or the
# recovery shell. POSIX sh (mksh/toybox). Everything is logged to $LOG (synced after every line: evidence survives a hang).
#   MODE=pre      READ-ONLY, any kernel: battery/thermal/cpufreq state + GO/NOGO for the voltage steps
#   MODE=check    READ-ONLY, CPR kernel: DT marker, cpr3/cpufreq-hw probe, policies, OPPs within the caps, CPR debugfs
#   MODE=step     VOLTAGE (Pierre present, I_AM_ATTENDED=1): userspace governor, each OPP <= cap for HOLD s with ALL CPUs of
#                 the cluster busy; thermal/battery polled every second; abort -> lowest OPP + governor restored, exit 3
#   MODE=soak     VOLTAGE (after step PASS): schedutil + bursty load for MIN minutes with the same aborts (F4)
#   MODE=restore  kill the loads, governor back to the saved one (or schedutil), print the state
# Env: PMAX_PWR/PMAX_PERF (kHz caps; default F2a 902400/1113600), HOLD (s, 5), TMAX (mC any zone, 65000), TBAT (0.1 C, 450),
#      CAPMIN (battery %, 40), MIN (soak minutes, 10), A6L_SYSROOT (tests only), LOG (/data/local/tmp/a6l-cpr-r5.log).
MODE=${MODE:-pre}; SR=${A6L_SYSROOT:-}; SYS=$SR/sys; PROC=$SR/proc
LOG=${LOG:-/data/local/tmp/a6l-cpr-r5.log}; [ -d "$(dirname $LOG)" ] || LOG=/tmp/a6l-cpr-r5.log
PMAX_PWR=${PMAX_PWR:-902400}; PMAX_PERF=${PMAX_PERF:-1113600}; HOLD=${HOLD:-5}; TMAX=${TMAX:-65000}; TBAT=${TBAT:-450}
DMESG=${A6L_DMESG:-dmesg}; CAPMIN=${CAPMIN:-40}; MIN=${MIN:-10}; STATE=$(dirname $LOG)/a6l-cpr-r5.state
BAT=$SYS/class/power_supply/qcom-battery; [ -d $BAT ] || BAT=$SYS/class/power_supply/battery
say() { echo "A6L_CPR $*"; echo "$(cut -d' ' -f1 $PROC/uptime 2>/dev/null) $*" >> $LOG; [ -z "$SR" ] && sync 2>/dev/null; return 0; }   # sync: log survives a hang (skipped in host tests)
v() { cat "$1" 2>/dev/null || echo NA; }
# helpers use _-prefixed variables: sh variables are global and the main loops use p/f/c/lo
maxtemp() { _m=0; _mz=none; for _z in $SYS/class/thermal/thermal_zone*; do _t=$(cat $_z/temp 2>/dev/null); case "$_t" in ''|*[!0-9-]*) continue;; esac
    [ "$_t" -gt "$_m" ] && { _m=$_t; _mz=$(cat $_z/type 2>/dev/null); }; done; echo "$_m $_mz"; }
policies() { for _pp in $SYS/devices/system/cpu/cpufreq/policy*; do [ -d $_pp ] && echo $_pp; done; }
# cluster of a policy from the MPIDR of its first CPU: 0x1xx = perf (gold, apc1), 0x0xx = pwr (silver, apc0)
# (DT node name via of_node: cpu@100..103 = perf, cpu@0..3 = pwr; logical CPU numbers and cluster_id are NOT reliable here)
cluster() { _cc=$(v $1/related_cpus | cut -d' ' -f1); _n=$(readlink $SYS/devices/system/cpu/cpu$_cc/of_node 2>/dev/null)
    case "${_n##*/}" in cpu@1??) echo perf;; cpu@*) echo pwr;; *) echo unknown;; esac; }
cap() { case "$(cluster $1)" in perf) echo $PMAX_PERF;; pwr) echo $PMAX_PWR;; *) echo 300000;; esac; }   # unknown -> lowest OPP only
LOADS=""
load_on() { for _c in $(v $1/related_cpus); do
      ( while :; do _i=0; while [ $_i -lt 20000 ]; do _i=$((_i+1)); done; done ) &   # no TERM trap: dash would defer it forever
      _pid=$!; LOADS="$LOADS $_pid"; taskset -p $((1 << _c)) $_pid > /dev/null 2>&1; done; }
load_off() { for _lp in $LOADS; do kill -9 $_lp 2>/dev/null; done; for _lp in $LOADS; do wait $_lp 2>/dev/null; done; LOADS=""; }
restore_all() { load_off
    for _rp in $(policies); do _g=$(grep "^${_rp##*/} " $STATE 2>/dev/null | cut -d' ' -f2); [ -n "$_g" ] || _g=schedutil
      _lo=$(v $_rp/cpuinfo_min_freq); [ "$(v $_rp/scaling_governor)" = userspace ] && echo $_lo > $_rp/scaling_setspeed 2>/dev/null
      echo $_g > $_rp/scaling_governor 2>/dev/null; say "restore ${_rp##*/} gov=$(v $_rp/scaling_governor) cur=$(v $_rp/scaling_cur_freq)"; done; }
# returns 0 = ok, 1 = abort (reason in $WHY)
guard() { set -- $(maxtemp); WHY=""
    [ "$1" -ge "$TMAX" ] && WHY="thermal zone $2 at $1 mC >= $TMAX"
    _bt=$(v $BAT/temp); case "$_bt" in NA|'') ;; *) [ "$_bt" -ge "$TBAT" ] && WHY="battery $_bt (0.1 C) >= $TBAT";; esac
    [ -z "$WHY" ]; }
preflight() { ok=1; set -- $(maxtemp); cap_=$(v $BAT/capacity); t=$(v $BAT/temp)
    say "kernel $(uname -r) dt=$(tr -d '\0' < $PROC/device-tree/chosen/hisense,a6l-image 2>/dev/null) battery ${cap_}% temp=$t status=$(v $BAT/status) maxzone $1 mC ($2)"
    [ "$1" -ge 50000 ] && { say "NOGO hottest zone $2 at $1 mC (>= 50 C): let the phone cool"; ok=0; }
    case "$t" in NA|'') say "WARN no battery temperature (fuel gauge not up?)";; *) [ "$t" -ge 400 ] && { say "NOGO battery at $t (>= 40.0 C)"; ok=0; };; esac
    case "$cap_" in NA|'') say "WARN no battery capacity";; *) [ "$cap_" -lt "$CAPMIN" ] && { say "NOGO battery ${cap_}% < ${CAPMIN}%: charge first (USB stays plugged in during the test)"; ok=0; };; esac
    [ -z "$(pidof watchdogd 2>/dev/null)" ] && say "WARN watchdogd not running: a hard hang needs a long power-key press (setprop persist.vendor.a6l.watchdog 1 + reboot arms it)"
    [ $ok = 1 ]; }
case $MODE in
pre)
    : > $LOG; say "BEGIN pre (read-only)"; for z in $SYS/class/thermal/thermal_zone*; do [ -e $z/temp ] && say "zone $(v $z/type) $(v $z/temp)"; done
    n=0; for p in $(policies); do n=$((n+1)); say "CPUFREQ ${p##*/} cpus=$(v $p/related_cpus) driver=$(v $p/scaling_driver) cur=$(v $p/scaling_cur_freq) avail=$(v $p/scaling_available_frequencies) gov=$(v $p/scaling_governor)"; done
    say "cpufreq policies: $n (0 = plain r5/V67 kernel, no DVFS)"
    if preflight; then say "PRE GO"; else say "PRE NOGO"; exit 1; fi ;;
check)
    say "BEGIN check (read-only)"; mount -t debugfs none $SYS/kernel/debug 2>/dev/null
    dtm=$(tr -d '\0' < $PROC/device-tree/chosen/hisense,a6l-image 2>/dev/null); say "dt marker '$dtm'"; bad=0
    case "$dtm" in *cpr*) ;; *) say "FAIL not the CPR DT (expected rom-v2-cpr-f2a)"; bad=1;; esac
    $DMESG 2>/dev/null | grep -iE 'cpr3|cprh|cpufreq|osm|qfprom|apc_cprh|power-controller@179c' | tail -40 | sed 's/^/A6L_CPR_DMESG /'
    $DMESG 2>/dev/null | grep -iE '(cpr|cpufreq).*(fail|error|timeout|invalid)|deferred.*(179c1000|179c8000)' | head -5 | sed 's/^/A6L_CPR ERRLINE /' | grep . && bad=1
    for t in $SYS/kernel/debug/qcom_cpr3/thread*; do [ -e "$t" ] && sed "s|^|A6L_CPR_DBG ${t##*/}: |" $t; done
    n=0; for p in $(policies); do n=$((n+1)); c=$(cap $p); hi=0
      for f in $(v $p/scaling_available_frequencies); do [ "$f" -gt "$hi" ] && hi=$f; done
      say "CPUFREQ ${p##*/} $(cluster $p) cpus=$(v $p/related_cpus) driver=$(v $p/scaling_driver) avail=$(v $p/scaling_available_frequencies) max=$hi cap=$c cur=$(v $p/scaling_cur_freq) gov=$(v $p/scaling_governor)"
      [ "$(v $p/scaling_driver)" = qcom-cpufreq-hw ] || { say "FAIL ${p##*/} driver is not qcom-cpufreq-hw"; bad=1; }
      [ "$hi" -gt "$c" ] && { say "FAIL ${p##*/} offers $hi kHz > cap $c (wrong DT?)"; bad=1; }; done
    [ $n = 2 ] || { say "FAIL expected 2 cpufreq policies, found $n"; bad=1; }
    set -- $(maxtemp); say "hottest zone $2 $1 mC"
    [ $bad = 0 ] && say "CPR_CHECK PASS" || { say "CPR_CHECK FAIL: stop here (reboot to the plain r5 kernel), send $LOG"; exit 1; } ;;
step|soak)
    [ "$I_AM_ATTENDED" = 1 ] || { say "refused: $MODE changes CPU voltage/frequency; run with I_AM_ATTENDED=1 and Pierre present"; exit 2; }
    preflight || { say "refused: preflight NOGO"; exit 1; }
    : > $STATE; for p in $(policies); do echo "${p##*/} $(v $p/scaling_governor)" >> $STATE; done
    trap 'say "interrupted"; restore_all; exit 4' INT TERM
    if [ $MODE = step ]; then
      for p in $(policies); do
        pn=${p##*/}; lim=$(cap $p); lo=$(v $p/cpuinfo_min_freq)
        echo userspace > $p/scaling_governor 2>/dev/null; [ "$(v $p/scaling_governor)" = userspace ] || { say "FAIL no userspace governor on $pn (modprobe cpufreq_userspace?)"; restore_all; exit 1; }
        for f in $(v $p/scaling_available_frequencies | tr ' ' '\n' | sort -n); do
          [ "$f" -gt "$lim" ] && { say "STOP $pn $f > cap $lim"; break; }
          say "SET $pn $(cluster $p) $f kHz"; echo $f > $p/scaling_setspeed; load_on $p; s=0
          while [ $s -lt $HOLD ]; do sleep 1; s=$((s+1))
            if ! guard; then load_off; echo $lo > $p/scaling_setspeed; say "ABORT $WHY -> $pn back to $lo"; restore_all; say CPR_STEP ABORT; exit 3; fi; done
          cur=$(v $p/scaling_cur_freq); load_off; set -- $(maxtemp)
          d=$((cur - f)); [ $d -lt 0 ] && d=$((-d)); ok=ok; [ $((d * 20)) -gt $f ] && ok="MISMATCH(>5%)"
          say "STEP $pn set=$f cur=$cur $ok hottest=$1($2) bat=$(v $BAT/temp)"
        done
        echo $lo > $p/scaling_setspeed; say "$pn done -> $lo"
      done
      restore_all; say "CPR_STEP DONE (any MISMATCH = OSM did not follow: report)"
    else
      for p in $(policies); do echo schedutil > $p/scaling_governor 2>/dev/null; done
      end=$(( $(cut -d. -f1 $PROC/uptime) + MIN * 60 )); k=0
      kmax=$((MIN * 12 + 2))   # 5 s per iteration; bounds the loop even if uptime misbehaves
      while [ "$(cut -d. -f1 $PROC/uptime)" -lt $end ] && [ $k -lt $kmax ]; do
        for p in $(policies); do load_on $p; done; sleep 3; load_off; sleep 2; k=$((k+1))
        guard || { say "ABORT $WHY"; restore_all; say CPR_SOAK ABORT; exit 3; }
        [ $((k % 6)) = 0 ] && { set -- $(maxtemp); say "SOAK t=$(cut -d' ' -f1 $PROC/uptime) hottest=$1($2) bat=$(v $BAT/temp) $(for p in $(policies); do printf '%s=%s ' ${p##*/} $(v $p/scaling_cur_freq); done)"; }
      done
      restore_all; say "CPR_SOAK DONE ${MIN} min"
    fi ;;
restore) restore_all; say RESTORED ;;
*) say "unknown MODE $MODE"; exit 2 ;;
esac
