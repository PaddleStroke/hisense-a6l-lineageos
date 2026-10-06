#!/usr/bin/env bash
# r6e (1 Oct 2026, docs/rom-r6e-20261001.md): LAPTOP side live capture of a booting A6L ROM over adb, from the moment adbd
# answers until Ctrl-C. Read-only on the phone (adb shell dmesg/logcat/cat only). Reconnects by itself after a drop/reboot.
# usage (in the kit dir, phone booting or booted):  bash extra/live-capture.sh [outdir]      (default logs/live-<time>)
#   dmesg.txt     `adb shell dmesg -w` (r6e debug builds: dmesg_restrict=0 + /dev/kmsg 0644 from post-fs-data on)
#   logcat.txt    `adb logcat -b main,system,crash,events -v threadtime`
#   pressure.txt  every 5 s: uptime, PSI io/memory/cpu, MemAvailable/Dirty/Writeback, eMMC inflight
# A6L_IOSTALL / A6L_IOWATCH lines (the r6e IO-stall detector) are echoed on this terminal as they arrive.
O=${1:-logs/live-$(date +%H%M%S)}; mkdir -p "$O"; echo "live capture -> $O (Ctrl-C to stop)"; date > "$O/start.txt"
stream() {
    n=$1; shift
    while :; do
        adb wait-for-device 2>/dev/null
        echo "=== $(date '+%F %T') connected ($(adb get-state 2>/dev/null))" >> "$O/$n.txt"
        "$@" >> "$O/$n.txt" 2>&1
        echo "=== $(date '+%F %T') stream ended rc=$?" >> "$O/$n.txt"; sleep 1
    done
}
stream dmesg adb shell dmesg -w &
stream logcat adb logcat -b main,system,crash,events -v threadtime &
stream pressure adb shell 'while :; do echo "== $(cat /proc/uptime)"; for p in io memory cpu; do echo "$p $(cat /proc/pressure/$p | tr "\n" " ")"; done; grep -E "MemAvailable|^Dirty|^Writeback:" /proc/meminfo | tr -s " " | tr "\n" " "; echo; echo "inflight $(cat /sys/block/mmcblk1/inflight)"; sleep 5; done' &
touch "$O/dmesg.txt"; tail -n 0 -F "$O/dmesg.txt" 2>/dev/null | grep --line-buffered -E 'A6L_IOSTALL|A6L_IOWATCH (start|alive)|PM: suspend|hard LOCKUP|Kernel panic|Watchdog' &
trap 'kill $(jobs -p) 2>/dev/null; echo; echo "stopped; logs in $O"; exit 0' INT TERM
wait
