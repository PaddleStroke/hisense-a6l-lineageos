#!/bin/bash
# diag-r5 LAPTOP: copy the phone-side results (snapshots, kit log, pstore of the PREVIOUS boot) to logs/<tag>/.
# usage: host/diag-pull.sh <tag>
S=${ADB_SERIAL:-HLTE730T-PROBE}; cd "$(dirname "$0")/.." || exit 1
O=logs/${1:-pull}-$(date -u +%Y%m%dT%H%M%SZ); mkdir -p "$O"
for f in $(adb -s "$S" shell '/system/bin/toybox ls /tmp/diag/log.txt /tmp/diag/snap-*.txt 2>/dev/null' 2>/dev/null | tr -d '\r' | grep '^/tmp/'); do adb -s "$S" pull "$f" "$O/" >/dev/null; done
adb -s "$S" pull /tmp/diag/pstore-prev "$O/" >/dev/null 2>&1
adb -s "$S" shell '/system/bin/toybox dmesg' 2>/dev/null > "$O/dmesg.txt"
ls -la "$O" "$O/pstore-prev" 2>/dev/null
