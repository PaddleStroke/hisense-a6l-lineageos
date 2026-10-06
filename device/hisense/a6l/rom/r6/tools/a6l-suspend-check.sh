#!/system/bin/sh
# A6L suspend / wake / drain checks (r6 prep, completeness audit 29 Sep 2026; H53 H54 H62, F3 proximity wake).
# Run as root from adb shell on the INSTALLED ROM (screen off, USB unplugged where noted). Output: one "key=value" line per
# fact, prefixed A6LSUSP, so a log can be diffed between runs. Nothing is written except the RTC wakealarm (wake-rtc) and
# the log file given to drain.
#   a6l-suspend-check.sh snapshot                 counters + top wakeup sources + sleep mode + battery
#   a6l-suspend-check.sh wake-rtc [sec]           arm an RTC alarm (default 60 s), wait for a real suspend/resume cycle
#   a6l-suspend-check.sh drain <log> [min] [hours]  battery/suspend logger (default every 10 min for 8 h; overnight test)
#   a6l-suspend-check.sh delta <before> <after>   compare two snapshot outputs (suspend success/fail, top wake sources)
# PASS criteria (attended): snapshot after 10 min screen-off, unplugged: success grows, fail=0, no single wakeup source
# with active_count growing every few seconds; wake-rtc prints RESUMED; drain overnight < 1 %/h with radio on.
P=/sys/power; S=$P/suspend_stats; B=/sys/class/power_supply/qcom-battery
say() { echo "A6LSUSP $*"; }
rd() { cat "$1" 2>/dev/null || echo "?"; }
snapshot() {
  say "uptime=$(cut -d' ' -f1 /proc/uptime) boottime_s=$(awk '{print $1}' /proc/uptime)"
  say "mem_sleep=$(rd $P/mem_sleep) state=$(rd $P/state) wakeup_count=$(rd $P/wakeup_count)"
  for k in success fail failed_freeze failed_prepare failed_suspend failed_suspend_late failed_suspend_noirq failed_resume last_failed_dev last_failed_errno last_failed_step last_hw_sleep total_hw_sleep; do
    [ -e $S/$k ] && say "stats.$k=$(rd $S/$k)"
  done
  # top 12 wakeup sources by active_count (debugfs: /sys/kernel/debug/wakeup_sources; sysfs class fallback)
  if [ -r /sys/kernel/debug/wakeup_sources ]; then
    awk 'NR>1 && $2>0 {print $2, $1, $7}' /sys/kernel/debug/wakeup_sources | sort -rn | head -12 | while read c n t; do say "ws.$n active_count=$c total_ms=$t"; done
  else
    for w in /sys/class/wakeup/wakeup*; do echo "$(rd $w/active_count) $(rd $w/name) $(rd $w/total_time_ms)"; done | sort -rn | head -12 | while read c n t; do say "ws.$n active_count=$c total_ms=$t"; done
  fi
  # SoC low-power stats (qcom_stats / rpm stats if the kernel exposes them)
  for f in /sys/kernel/debug/qcom_stats/* /sys/power/rpmh_stats/master_stats /sys/power/system_sleep/stats; do
    [ -r "$f" ] && say "socstats.$(basename $f)=$(tr '\n' ' ' < $f | tr -s ' ' | cut -c1-200)"
  done
  say "batt capacity=$(rd $B/capacity) status=$(rd $B/status) current_now=$(rd $B/current_now) voltage_now=$(rd $B/voltage_now) temp=$(rd $B/temp) charge_counter=$(rd $B/charge_counter)"
  say "wakelocks_held=$(tr '\n' ' ' < $P/wake_lock 2>/dev/null)"
  say "irq_wakeups=$(grep -c . /proc/interrupts 2>/dev/null)"
}
case "$1" in
  snapshot) snapshot ;;
  wake-rtc)
    sec=${2:-60}; R=/sys/class/rtc/rtc0
    [ -w $R/wakealarm ] || { say "ERROR no writable $R/wakealarm"; exit 1; }
    before=$(rd $S/success); echo 0 > $R/wakealarm; echo "+$sec" > $R/wakealarm
    say "armed rtc0 +${sec}s success_before=$before; turn the screen off now and do not touch the phone"
    i=0; while [ $i -lt $((sec + 120)) ]; do sleep 5; i=$((i + 5)); n=$(rd $S/success); [ "$n" != "$before" ] && break; done
    after=$(rd $S/success); last=$(rd $P/pm_wakeup_irq)
    if [ "$after" != "$before" ]; then say "RESUMED success $before->$after wakeup_irq=$last alarm_now=$(rd $R/wakealarm)"; else say "NO_SUSPEND success=$after (a wakelock kept the system awake: see snapshot)"; fi ;;
  drain)
    log=$2; per=${3:-10}; hours=${4:-8}; [ -n "$log" ] || { echo "usage: $0 drain <log> [min] [hours]"; exit 2; }
    n=$((hours * 60 / per)); say "drain start per=${per}min samples=$n" | tee -a "$log"
    while [ $n -ge 0 ]; do
      echo "A6LSUSP t=$(date +%s) cap=$(rd $B/capacity) cc=$(rd $B/charge_counter) i=$(rd $B/current_now) v=$(rd $B/voltage_now) temp=$(rd $B/temp) ok=$(rd $S/success) fail=$(rd $S/fail)" >> "$log"
      n=$((n - 1)); sleep $((per * 60))
    done; say "drain done" | tee -a "$log" ;;
  delta)
    [ -r "$2" ] && [ -r "$3" ] || { echo "usage: $0 delta <before> <after>"; exit 2; }
    for k in success fail; do a=$(sed -n "s/^A6LSUSP stats.$k=//p" "$2"); b=$(sed -n "s/^A6LSUSP stats.$k=//p" "$3"); say "delta.$k=$((b - a))"; done
    grep '^A6LSUSP ws\.' "$3" | while read -r _ n c _; do a=$(grep "^A6LSUSP $n " "$2" | sed 's/.*active_count=\([0-9]*\).*/\1/'); say "delta.$n active_count +$(( ${c#active_count=} - ${a:-0} ))"; done ;;
  *) sed -n '2,15p' "$0"; exit 2 ;;
esac
