#!/bin/bash
# diag-r5 LAPTOP: copy the phone kit to /tmp/diag (RAM) and check every file's sha256 ON THE PHONE.
# usage: host/diag-push.sh      -> prints DIAG_PUSH_OK <n> files, or DIAG_PUSH_FAIL
S=${ADB_SERIAL:-HLTE730T-PROBE}; cd "$(dirname "$0")/.." || exit 1
adb devices | grep -qE "^$S[[:space:]]+(recovery|device)" || { echo "DIAG_PUSH_FAIL no $S"; exit 2; }
adb -s "$S" shell '/system/bin/toybox mkdir -p /tmp/diag' >/dev/null 2>&1
adb -s "$S" push phone/. /tmp/diag/ | tail -n 1
n=$(wc -l < phone/SHA256SUMS)
out=$(adb -s "$S" shell 'cd /tmp/diag && /system/bin/toybox chmod 755 d.sh && /system/bin/toybox sha256sum -c SHA256SUMS 2>&1 | /system/bin/toybox grep -c ": OK$"; echo RC=$?' 2>/dev/null | tr -d '\r')
ok=$(echo "$out" | grep -E '^[0-9]+$' | tail -n 1)
[ "$ok" = "$n" ] && echo "DIAG_PUSH_OK $ok files" || { echo "DIAG_PUSH_FAIL ok=$ok expected=$n"; echo "$out" | tail -n 5; exit 3; }
adb -s "$S" shell 'echo DIAG=$(/system/bin/toybox cat /proc/cmdline | /system/bin/toybox tr " " "\n" | /system/bin/toybox grep a6l_diag); /system/bin/toybox uname -r' 2>/dev/null | grep -E "DIAG=|a6l"
