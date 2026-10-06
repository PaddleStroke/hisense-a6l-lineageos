#!/usr/bin/env bash
# Run on the laptop for the attended Android test; writes logs on the laptop.
# Usage: bash extra/collect-a6l-r6e-live.sh <tag> [awake|sleep] [minutes]
# Set A6L_EXPECT_BUILD=r6f when using the collector with the r6f kit.
set -euo pipefail
SERIAL=1e529013
EXPECTED_BUILD=${A6L_EXPECT_BUILD:-r6e}
[[ $EXPECTED_BUILD =~ ^r[0-9]+[a-z]*$ ]] || { echo 'Invalid expected ROM build'; exit 2; }
TAG=${1:?supply a log tag}; MODE=${2:-awake}; MINUTES=${3:-20}
[[ $TAG =~ ^[A-Za-z0-9_-]+$ ]] || { echo 'Use a simple log tag'; exit 2; }
[[ $MODE == awake || $MODE == sleep ]] || exit 2
[[ $MINUTES =~ ^[0-9]+$ ]] && (( MINUTES >= 1 && MINUTES <= 60 )) || exit 2
O=logs/live-$TAG
[[ ! -e $O ]] || { echo "$O already exists; use a new tag"; exit 2; }
mkdir -p "$O"
timeout 300 adb -s "$SERIAL" wait-for-device
BUILD=$(timeout 15 adb -s "$SERIAL" shell getprop ro.vendor.a6l.rom.build | tr -d '\r')
[[ $BUILD == "$EXPECTED_BUILD" ]] || { echo "Expected $EXPECTED_BUILD on $SERIAL, got $BUILD"; exit 1; }
SECONDS_LIMIT=$((MINUTES * 60))
# adb appears before the Settings provider during boot. Keep the streams alive
# while waiting for it, then collect the requested full awake interval.
STREAM_LIMIT=$((SECONDS_LIMIT + 600))
timeout "$STREAM_LIMIT" adb -s "$SERIAL" shell dmesg -w > "$O/kmsg-live.txt" 2>&1 & KP=$!
timeout "$STREAM_LIMIT" adb -s "$SERIAL" logcat -b all -v threadtime > "$O/logcat-live.txt" 2>&1 & LP=$!
OLD=
cleanup() {
    kill "$KP" "$LP" 2>/dev/null || true
    wait "$KP" "$LP" 2>/dev/null || true
    if [[ $MODE == awake && -n $OLD ]]; then
        if [[ $OLD == null ]]; then
            timeout 10 adb -s "$SERIAL" shell settings delete global stay_on_while_plugged_in >> "$O/restore-awake.txt" 2>&1 || true
        elif [[ $OLD =~ ^[0-9]+$ ]]; then
            timeout 10 adb -s "$SERIAL" shell settings put global stay_on_while_plugged_in "$OLD" >> "$O/restore-awake.txt" 2>&1 || true
        fi
    fi
}
trap cleanup EXIT
if [[ $MODE == awake ]]; then
    READY=0; WAIT_START=$SECONDS
    while (( SECONDS - WAIT_START < 300 )); do
        if [[ -z $OLD ]]; then
            if VALUE=$(timeout 10 adb -s "$SERIAL" shell settings get global stay_on_while_plugged_in 2>> "$O/framework-wait.txt"); then
                VALUE=${VALUE//$'\r'/}
                if [[ $VALUE == null || $VALUE =~ ^[0-9]+$ ]]; then
                    OLD=$VALUE
                    printf '%s\n' "$OLD" > "$O/previous-stay-awake.txt"
                fi
            fi
        fi
        if [[ -n $OLD ]] && timeout 10 adb -s "$SERIAL" shell settings put global stay_on_while_plugged_in 7 >> "$O/framework-wait.txt" 2>&1; then
            READY=1; break
        fi
        sleep 2
    done
    [[ $READY == 1 ]] || { echo 'Settings provider not ready after 300 s; early boot streams are saved'; exit 1; }
fi
timeout 15 adb -s "$SERIAL" shell getprop > "$O/props-start.txt" 2>&1 || true
START=$SECONDS
while (( SECONDS - START < SECONDS_LIMIT )); do
    {
        date -Is
        timeout 10 adb -s "$SERIAL" shell 'cat /proc/uptime; cat /proc/pressure/io; for b in /sys/block/mmcblk[0-9]; do case "$(readlink -f "$b")" in *c0c4000*) echo "emmc=${b##*/}"; cat "$b/inflight";; esac; done; getprop sys.boot_completed; getprop vendor.a6l.radio.state; getprop vendor.a6l.wlan.state'
    } >> "$O/heartbeat.txt" 2>&1 || true
    kill -0 "$KP" 2>/dev/null || { echo 'Kernel stream ended; check kmsg-live.txt' | tee -a "$O/heartbeat.txt"; break; }
    sleep 10
done
timeout 15 adb -s "$SERIAL" shell dumpsys media.camera > "$O/camera.txt" 2>&1 || true
timeout 15 adb -s "$SERIAL" shell dumpsys power > "$O/power.txt" 2>&1 || true
timeout 15 adb -s "$SERIAL" shell dumpsys bluetooth_manager > "$O/bluetooth.txt" 2>&1 || true
printf 'Logs: %s\n' "$O"
