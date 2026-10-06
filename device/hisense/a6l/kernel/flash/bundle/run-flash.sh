#!/system/bin/sh
# run-flash.sh (flash/microSD agent, 28 Sep 2026) - ATTENDED rear flash/torch test, V75-usb recovery (RAM only).
#   MODE=check : read-only: kernel, bundle checksums, DT node status, modules, LED class devices.
#   MODE=load  : insmod a6l_flash_ovl.ko (runtime DT overlay: PM660L led-controller@d300 okay, led-0 = channel 1,
#                torch max 500 mA, flash max 500 mA / 100 ms) + led-class-flash.ko + leds-qcom-flash.ko,
#                list /sys/class/leds -> A6L_FLASH_LOAD_PASS / A6L_FLASH_LOAD_FAIL. The LED stays OFF.
#   MODE=torch : torch ON for TORCH_S (default 2, max 10) s then OFF. Default LOW: TORCH_MA=25 (5..100 mA allowed);
#                LEVEL=max is the only way above 100 mA and is capped at the stock torch max, 500 mA.
#   MODE=flash : ONE software strobe, FLASH_MA (default 200, max 500) mA, FLASH_MS (default 50, max 100) ms.
#   MODE=off   : force the LED off (also done by the EXIT trap of every mode).
# usage: adb -s HLTE730T-PROBE shell 'export PATH=/tmp/bin:$PATH TMPDIR=/tmp; D=/tmp/flash MODE=load sh /tmp/flash/run-flash.sh'
export PATH=/tmp/bin:$PATH TMPDIR=/tmp
D=${D:-/tmp/flash}; MODE=${MODE:-check}
SYS=${A6L_SYS:-/sys}; DT=${A6L_DT:-/proc/device-tree}   # A6L_SYS/A6L_DT/A6L_MOCK: offline mock tests only
TORCH_CAP_MA=500; FLASH_CAP_MA=500; FLASH_CAP_MS=100
say() { echo "A6L_FLASH_$*"; }
isnum() { case "$1" in ''|*[!0-9]*) return 1;; esac; return 0; }
find_led() {
  for l in $SYS/class/leds/*; do [ -e "$l/flash_strobe" ] && { echo "$l"; return; }; done
  for l in $SYS/class/leds/*flash*; do [ -e "$l/brightness" ] && { echo "$l"; return; }; done
}
L=$(find_led)
led_off() {
  [ -n "$L" ] || return 0
  [ -e "$L/flash_strobe" ] && echo 0 > "$L/flash_strobe" 2>/dev/null
  echo 0 > "$L/brightness" 2>/dev/null
  say "OFF brightness=$(cat "$L/brightness" 2>/dev/null)"
}
trap led_off EXIT
trap 'led_off; trap - EXIT; exit 130' INT TERM HUP
kmsg() { [ -n "$A6L_MOCK" ] || dmesg | grep -iE "A6L_FLASH_OVL|qcom-flash|flash-led|d300|led-class" | tail -${1:-15}; }
dtnode=$DT/soc@0/spmi@800f000/pmic@3/led-controller@d300
show_led() {
  [ -n "$L" ] || return 1
  say "LED dev=$L max_brightness=$(cat $L/max_brightness 2>/dev/null) brightness=$(cat $L/brightness 2>/dev/null) max_flash_brightness_uA=$(cat $L/max_flash_brightness 2>/dev/null) max_flash_timeout_us=$(cat $L/max_flash_timeout 2>/dev/null) flash_fault=$(cat $L/flash_fault 2>/dev/null | tr '\n' ' ')"
}

case "$MODE" in
check)
  say "CHECK kernel=$(uname -r) D=$D"
  [ -f $D/SHA256SUMS ] && (cd $D && sha256sum -c SHA256SUMS) 2>&1 | sed 's/^/A6L_FLASH_SUM /'
  say "CHECK dt led-controller@d300 status=$(cat $dtnode/status 2>/dev/null | tr -d '\0') led-0=$([ -d $dtnode/led-0 ] && echo present || echo absent)"
  say "CHECK modules: $(grep -E '^(a6l_flash_ovl|led_class_flash|leds_qcom_flash) ' /proc/modules 2>/dev/null | cut -d' ' -f1 | tr '\n' ' ')"
  ls $SYS/class/leds 2>/dev/null | sed 's/^/A6L_FLASH_LEDS /'
  show_led || say "CHECK no flash LED class device yet (run MODE=load)"
  say CHECK_DONE
  ;;
load)
  if [ -z "$A6L_MOCK" ]; then
    grep -q '^a6l_flash_ovl ' /proc/modules || insmod $D/a6l_flash_ovl.ko || { kmsg; say "LOAD_FAIL insmod a6l_flash_ovl.ko"; exit 1; }
    grep -q '^led_class_flash ' /proc/modules || insmod $D/led-class-flash.ko || say "LOAD_WARN insmod led-class-flash.ko failed"
    grep -q '^leds_qcom_flash ' /proc/modules || insmod $D/leds-qcom-flash.ko || say "LOAD_WARN insmod leds-qcom-flash.ko failed"
    sleep 1
  fi
  kmsg 20
  say "LOAD dt status=$(cat $dtnode/status 2>/dev/null | tr -d '\0')"
  for d in $SYS/bus/platform/drivers/*flash*; do ls $d 2>/dev/null | grep -i d300 | sed "s#^#A6L_FLASH_BOUND ${d##*/} #"; done
  ls $SYS/class/leds 2>/dev/null | sed 's/^/A6L_FLASH_LEDS /'
  L=$(find_led)
  if [ -n "$L" ] && [ -e "$L/flash_strobe" ]; then
    echo 0 > $L/brightness 2>/dev/null; show_led
    MF=$(cat $L/max_flash_brightness 2>/dev/null)
    isnum "$MF" && [ "$MF" -le $((FLASH_CAP_MA*1000)) ] || { say "LOAD_FAIL max_flash_brightness=$MF above the ${FLASH_CAP_MA} mA test cap"; exit 1; }
    say LOAD_PASS
  else
    say "LOAD_FAIL no flash LED class device (see kernel log above: 'Failed to get parent regmap' = reparent failed)"; exit 1
  fi
  ;;
torch)
  [ -n "$L" ] || { say "TORCH_FAIL no flash LED (run MODE=load first)"; exit 1; }
  MB=$(cat $L/max_brightness 2>/dev/null); isnum "$MB" && [ "$MB" -gt 0 ] || { say "TORCH_FAIL bad max_brightness=$MB"; exit 1; }
  # leds-qcom-flash: torch current_mA = brightness * led-max-microamp(500 mA) / 255 (LED_FULL), 5 mA register steps
  if [ "$LEVEL" = max ]; then MA=$TORCH_CAP_MA; else MA=${TORCH_MA:-25}; fi
  isnum "$MA" || { say "TORCH_FAIL TORCH_MA=$MA not a number"; exit 1; }
  [ "$LEVEL" = max ] || [ "$MA" -le 100 ] || { say "TORCH_FAIL TORCH_MA=$MA > 100 needs LEVEL=max (capped ${TORCH_CAP_MA} mA)"; exit 1; }
  [ "$MA" -ge 5 ] && [ "$MA" -le $TORCH_CAP_MA ] || { say "TORCH_FAIL TORCH_MA=$MA outside 5..$TORCH_CAP_MA"; exit 1; }
  B=$((MA * 255 / TORCH_CAP_MA)); [ $B -lt 1 ] && B=1; [ $B -gt $MB ] && B=$MB
  S=${TORCH_S:-2}; isnum "$S" && [ "$S" -ge 1 ] && [ "$S" -le 10 ] || S=2
  say "TORCH_ON brightness=$B/$MB (~$((B * TORCH_CAP_MA / 255)) mA) for $S s - ASK PIERRE: rear LED lit? (y/n)"
  echo $B > $L/brightness || { say "TORCH_FAIL write brightness"; kmsg 5; exit 1; }
  say "TORCH readback brightness=$(cat $L/brightness 2>/dev/null)"
  sleep $S
  led_off; show_led
  say "TORCH_DONE brightness_written=$B"
  ;;
flash)
  [ -n "$L" ] && [ -e "$L/flash_strobe" ] || { say "STROBE_FAIL no flash LED with flash_strobe (run MODE=load first)"; exit 1; }
  MA=${FLASH_MA:-200}; MS=${FLASH_MS:-50}
  isnum "$MA" && [ "$MA" -ge 13 ] && [ "$MA" -le $FLASH_CAP_MA ] || { say "STROBE_FAIL FLASH_MA=$MA outside 13..$FLASH_CAP_MA"; exit 1; }
  isnum "$MS" && [ "$MS" -ge 10 ] && [ "$MS" -le $FLASH_CAP_MS ] || { say "STROBE_FAIL FLASH_MS=$MS outside 10..$FLASH_CAP_MS"; exit 1; }
  echo 0 > $L/brightness 2>/dev/null
  echo $((MA * 1000)) > $L/flash_brightness || { say "STROBE_FAIL write flash_brightness"; exit 1; }
  echo $((MS * 1000)) > $L/flash_timeout || { say "STROBE_FAIL write flash_timeout"; exit 1; }
  RB=$(cat $L/flash_brightness 2>/dev/null); RT=$(cat $L/flash_timeout 2>/dev/null)
  isnum "$RB" && isnum "$RT" && [ "$RB" -le $((FLASH_CAP_MA*1000)) ] && [ "$RT" -le $((FLASH_CAP_MS*1000)) ] \
    || { say "STROBE_FAIL readback flash_brightness=$RB flash_timeout=$RT above the caps - NOT strobing"; exit 1; }
  say "STROBE single strobe flash_brightness=${RB}uA flash_timeout=${RT}us - ASK PIERRE: saw one short flash? (y/n)"
  echo 1 > $L/flash_strobe || { say "STROBE_FAIL write flash_strobe"; kmsg 5; exit 1; }
  sleep 1
  say "STROBE fault=$(cat $L/flash_fault 2>/dev/null | tr '\n' ' ') strobe_state=$(cat $L/flash_strobe 2>/dev/null)"
  led_off
  say "STROBE_DONE"
  ;;
off) led_off; show_led; say OFF_DONE ;;
*) sed -n '2,11p' "$0"; exit 2 ;;
esac
