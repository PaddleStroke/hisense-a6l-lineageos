#!/system/bin/sh
# A6L "rest" attended tests (V74 recovery or later). MODE=fuses|haptics|hall|flash|camera|thermal|power
# Each mode is bounded and prints A6L_REST_<MODE>_... markers. D = bundle dir (default /tmp/rest).
export PATH=/tmp/bin:$PATH
D=${D:-/tmp/rest}
MODE=${MODE:-fuses}
say() { echo "A6L_REST $*"; }
dt_status() { cat "$1/status" 2>/dev/null | tr -d '\0'; }
SPMI=$(ls -d /proc/device-tree/soc@0/spmi@* 2>/dev/null | head -1)
case "$MODE" in
fuses)
  # READ-ONLY: CPR/speed-bin fuse rows 38..71 (qfprom @0x784000) for a6l_cpr_openloop.py --offset 0
  N=$(ls -d /sys/bus/nvmem/devices/qfprom* 2>/dev/null | head -1)
  [ -n "$N" ] || { say FUSES_FAIL no qfprom nvmem; exit 1; }
  dd if=$N/nvmem of=/tmp/a6l-qfprom-rows.bin bs=64 skip=256 count=9 2>/dev/null
  ls -l /tmp/a6l-qfprom-rows.bin; od -A x -t x1 /tmp/a6l-qfprom-rows.bin | head -40
  say FUSES_DONE pull /tmp/a6l-qfprom-rows.bin and run: python3 a6l_cpr_openloop.py a6l-qfprom-rows.bin --offset 0
  ;;
haptics)
  V=$SPMI/pmic@1/vibrator@c000
  say "vibrator node status=$(dt_status $V) brake=$(od -A n -t x1 $V/qcom,brake-pattern 2>/dev/null)"
  [ "$(dt_status $V)" = okay ] || { say HAPTICS_FAIL vibrator node not enabled in this DT; exit 1; }
  rmmod qcom_spmi_haptics 2>/dev/null
  mount -t debugfs none /sys/kernel/debug 2>/dev/null
  insmod $D/a6l_pm660_haptics.ko || { say HAPTICS_FAIL insmod; exit 1; }
  sleep 1; dmesg | grep -E "A6L_HAP|haptic" | tail -5
  cat /sys/kernel/debug/a6l_haptics/regs
  for s in ${LEVELS:-16384 32768 65535}; do
    say "HAPTICS_PULSE strength=$s (vmax ~ $((3200 * s / 65535)) mV) 300 ms - ASK PIERRE: felt? (y/n)"
    $D/a6l_vib $s 300; sleep 2
  done
  cat /sys/kernel/debug/a6l_haptics/regs
  say HAPTICS_DONE
  ;;
hall)
  # read-only GPIO75 level (works without the V75 overlay); with the overlay also watch SW_LID events
  mount -t debugfs none /sys/kernel/debug 2>/dev/null
  for i in 1 2 3 4 5 6 7 8 9 10; do
    L=$(grep -E "gpio75[ :]|-75 " /sys/kernel/debug/gpio 2>/dev/null | head -1)
    P=$(grep -A0 "pin 75 " /sys/kernel/debug/pinctrl/*tlmm*/pins 2>/dev/null | head -1)
    say "HALL_SAMPLE $i gpio='$L' pin='$P' (move a magnet over/away from the phone)"; sleep 1
  done
  say HALL_DONE
  ;;
flash)
  F=$SPMI/pmic@3/led-controller@d300
  [ "$(dt_status $F)" = okay ] || { say FLASH_FAIL led-controller@d300 not enabled - needs a6l-flash-v75 DT; exit 1; }
  insmod $D/led-class-flash.ko 2>/dev/null; insmod $D/leds-qcom-flash.ko 2>/dev/null; sleep 1
  L=$(ls -d /sys/class/leds/*flash* 2>/dev/null | head -1); say "led=$L max=$(cat $L/max_brightness 2>/dev/null)"
  [ -n "$L" ] || { say FLASH_FAIL no led class device; dmesg | tail -5; exit 1; }
  say "FLASH_TORCH 2 s at the lowest step - ASK PIERRE: rear LED lit?"; echo 1 > $L/brightness; sleep 2; echo 0 > $L/brightness
  say FLASH_DONE
  ;;
camera)
  insmod $D/i2c-qcom-cci.ko; insmod $D/qcom-camss.ko; insmod $D/hi846.ko; sleep 3
  dmesg | grep -iE "cci|camss|hi846|csiphy" | tail -20
  ls -l /dev/video* /dev/v4l-subdev* /dev/media* 2>&1 | head -30
  say CAMERA_DONE look for "hi846" chip-id success / -ENXIO at 0x20
  ;;
thermal)
  for z in /sys/class/thermal/thermal_zone*; do echo "$z $(cat $z/type) $(cat $z/temp)"; done
  say THERMAL_DONE
  ;;
power)
  for p in /sys/class/power_supply/*; do echo "== $p"; cat $p/uevent 2>/dev/null; done
  say POWER_DONE
  ;;
*) say "unknown MODE=$MODE"; exit 2;;
esac
