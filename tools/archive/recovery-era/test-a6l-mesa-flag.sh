#!/usr/bin/env bash
# Attended laptop test, after Android has proved stable. One flag per run.
# Does not reboot Android. Restores properties and restarts Settings on exit.
# Usage: bash extra/test-a6l-mesa-flag.sh <tag> <none|noscis|noblit|sysmem|...> [main|display]
set -euo pipefail
S=1e529013
TAG=${1:?supply a tag}; FLAG=${2:?supply one flag}
VIEW=${3:-main}
case "$VIEW" in
    main) LAUNCH=(-n com.android.settings/.Settings) ;;
    display) LAUNCH=(-a android.settings.DISPLAY_SETTINGS) ;;
    *) echo 'Unsupported Settings view'; exit 2 ;;
esac
[[ $TAG =~ ^[A-Za-z0-9_-]+$ ]] || exit 2
case "$FLAG" in none|noscis|noblit|sysmem|notile|noubwc|nogrow|serialc|inorder|ddraw|dclear) ;; *) echo 'Unsupported test flag'; exit 2;; esac
O=logs/mesa-$TAG
[[ ! -e $O ]] || { echo "$O exists; use a new tag"; exit 2; }
mkdir -p "$O"
PROP=debug.mesa.fd.mesa.debug
PRINT=debug.mesa.gallium.print.options
OLD=$(timeout 10 adb -s "$S" shell getprop "$PROP" | tr -d '\r')
OLDPRINT=$(timeout 10 adb -s "$S" shell getprop "$PRINT" | tr -d '\r')
# Quote restored values for the Android shell; getprop data is not shell code.
quote() { printf "'%s'" "${1//\'/\'\\\'\'}"; }
cleanup() {
    timeout 10 adb -s "$S" shell "setprop $PROP $(quote "$OLD"); setprop $PRINT $(quote "$OLDPRINT"); am force-stop com.android.settings; am start ${LAUNCH[*]}" >> "$O/restore.txt" 2>&1 || true
}
trap cleanup EXIT
[[ $FLAG != none ]] || FLAG=
timeout 10 adb -s "$S" shell "setprop $PROP $(quote "$FLAG"); setprop $PRINT 1; getprop $PROP" > "$O/property.txt"
timeout 15 adb -s "$S" shell am force-stop com.android.settings
timeout 60 adb -s "$S" shell am start -W "${LAUNCH[@]}" > "$O/launch.txt" 2>&1
sleep 8
timeout 15 adb -s "$S" shell dumpsys window > "$O/window.txt"
grep 'mCurrentFocus' "$O/window.txt" > "$O/focus.txt" || true
grep -q 'com.android.settings' "$O/focus.txt" || { echo 'Settings is not focused; this run is inconclusive'; exit 1; }
timeout 20 adb -s "$S" exec-out screencap -p > "$O/settings.png"
[[ -s $O/settings.png ]] || exit 1
timeout 15 adb -s "$S" logcat -d -t 1000 > "$O/logcat.txt" 2>&1 || true
grep 'FD_MESA_DEBUG\|GALLIUM_PRINT_OPTIONS' "$O/logcat.txt" > "$O/mesa-options.txt" || true
echo "Capture: $O/settings.png; check mesa-options.txt before interpreting the result"
