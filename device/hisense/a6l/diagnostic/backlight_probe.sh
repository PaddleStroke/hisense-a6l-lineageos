#!/system/bin/sh
# Future attended diagnostic only. Two low levels for two seconds each.
# Never call this on stock Android; no automatic discovery of another driver.
set -eu
test "$(/system/bin/toybox uname -r)" = '7.2.3-a6l-probe+'
test "$(getprop ro.a6l.ramdiag)" = 'v38'
test "$#" = 1 && test "$1" = '--test'
path=/sys/class/backlight/backlight
test -r "$path/device/of_node/compatible" || { echo 'A6L_BACKLIGHT_REFUSED no-compatible-device'; exit 1; }
/system/bin/toybox tr '\000' '\n' < "$path/device/of_node/compatible" | /system/bin/toybox grep -qx 'qcom,pm660l-wled'
test "$(/system/bin/toybox cat "$path/max_brightness")" = '4095'
old=$(/system/bin/toybox cat "$path/brightness")
case "$old" in ''|*[!0-9]*) exit 2;; esac
test "$old" -le 4095
restore() { printf '%s\n' "$old" > "$path/brightness"; }
trap restore EXIT
trap 'exit 1' HUP INT TERM
printf 'A6L_BACKLIGHT_BEGIN original=%s\n' "$old"
printf '64\n' > "$path/brightness"
/system/bin/toybox sleep 2
printf '256\n' > "$path/brightness"
/system/bin/toybox sleep 2
restore
test "$(/system/bin/toybox cat "$path/brightness")" = "$old"
echo 'A6L_BACKLIGHT_COMMANDS_PASS physical_confirmation_required=1'
