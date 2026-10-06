#!/bin/bash
# diag-r5 LAPTOP: stream the phone's kernel log (dmesg -w over adb) into logs/<name>-<UTC>.log, each line prefixed with
# the laptop time (s.ms). Survives a phone freeze (the file keeps everything received) and reconnects by itself when the
# phone comes back (next boot: the whole 4 MiB ring from 0 s is streamed again). Stop with Ctrl-C (or kill the pid).
# usage: host/diag-stream.sh <name>      (run it BEFORE each step; `tail -f` the file in another terminal)
S=${ADB_SERIAL:-HLTE730T-PROBE}
cd "$(dirname "$0")/.." || exit 1; mkdir -p logs
L=logs/${1:-stream}-$(date -u +%Y%m%dT%H%M%SZ).log; echo "stream -> $L"
stamp() { while IFS= read -r l; do printf '%s %s\n' "${EPOCHREALTIME:0:14}" "${l%$'\r'}"; done; }
while :; do
    echo "HOST ${EPOCHREALTIME:0:14} waiting for $S (recovery/device state)" >> "$L"
    until adb devices 2>/dev/null | grep -qE "^$S[[:space:]]+(recovery|device)"; do sleep 0.5; done
    echo "HOST ${EPOCHREALTIME:0:14} connected $(date -u +%FT%TZ)" >> "$L"
    adb -s "$S" shell '/system/bin/toybox dmesg -w' 2>&1 | stamp >> "$L"
    echo "HOST ${EPOCHREALTIME:0:14} STREAM ENDED (adb lost) $(date -u +%FT%TZ)" >> "$L"
    sleep 1
done
