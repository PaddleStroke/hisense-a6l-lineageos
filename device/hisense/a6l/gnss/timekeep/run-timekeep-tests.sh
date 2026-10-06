#!/usr/bin/env bash
# Host tests for a6l_timekeep (r5 bug hunt round2 gnss-time R1): decision logic (ASan/UBSan) + the real binary in
# --dry-run with a file-backed RTC (restore path, arguments, offset file parsing). Prints A6L_TIMEKEEP_SUITE PASS|FAIL.
set -u
D=$(cd "$(dirname "$0")" && pwd); W=$(mktemp -d); trap 'rm -rf "$W"' EXIT
CXX=${CXX:-g++}; fail=0
$CXX -std=c++17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined "$D/timekeep_test.cpp" -o "$W/t" && "$W/t" || fail=1
$CXX -std=c++17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined "$D/a6l_timekeep.cpp" -o "$W/tk" || fail=1
now=$(date +%s)
echo "$now" > "$W/rtc"                                  # clock == RTC: not set this boot
printf 'a6l_rtc_offset v1 1000\n' > "$W/off"
out=$(timeout 10 "$W/tk" --restore-only --dry-run --min-utc 1 --file "$W/off" --rtc-file "$W/rtc" 2>/dev/null)
exp=$((now + 1000)); got=${out#A6L_TIMEKEEP RESTORE }
[ -n "$out" ] && [ $((got - exp)) -ge -2 ] && [ $((got - exp)) -le 2 ] && echo "restore ok ($out)" || { echo "FAIL restore: '$out' want ~$exp"; fail=1; }
echo $((now - 100000)) > "$W/rtc"                       # clock already far from RTC (set this boot): nothing
out=$(timeout 10 "$W/tk" --restore-only --dry-run --min-utc 1 --file "$W/off" --rtc-file "$W/rtc" 2>/dev/null)
[ -z "$out" ] && echo "already-set ok" || { echo "FAIL already-set: '$out'"; fail=1; }
printf 'a6l_rtc_offset v1 1000' > "$W/off"; echo "$now" > "$W/rtc"   # torn file (no newline): ignored
out=$(timeout 10 "$W/tk" --restore-only --dry-run --min-utc 1 --file "$W/off" --rtc-file "$W/rtc" 2>/dev/null)
[ -z "$out" ] && echo "torn-file ok" || { echo "FAIL torn file: '$out'"; fail=1; }
echo $((now - 5000)) > "$W/rtc"; rm -f "$W/off"            # watcher: stores clock - RTC, then blocks on the timerfd
timeout 3 "$W/tk" --dry-run --min-utc 1 --file "$W/off" --rtc-file "$W/rtc" >/dev/null 2>&1
st=$(cat "$W/off" 2>/dev/null); v=${st#a6l_rtc_offset v1 }
case "$st" in "a6l_rtc_offset v1 "*) [ $((v - 5000)) -ge -2 ] && [ $((v - 5000)) -le 2 ] && echo "store ok ($v)" || { echo "FAIL store: '$st'"; fail=1; };; *) echo "FAIL store: '$st'"; fail=1;; esac
[ ! -e "$W/off.tmp" ] || { echo "FAIL tmp left"; fail=1; }
timeout 10 "$W/tk" --bogus >/dev/null 2>&1; [ $? = 2 ] && echo "bad-arg ok" || { echo "FAIL bad arg"; fail=1; }
[ $fail = 0 ] && echo "A6L_TIMEKEEP_SUITE PASS" || echo "A6L_TIMEKEEP_SUITE FAIL"
exit $fail
