#!/system/bin/sh
# misc2 agent (25 Sep 2026): VIBRATOR test for the V74 recovery RAM session (bundle v75/rest3). Offline-prepared.
# Root cause of "no vibration": the A6L motor is NOT on the PM660 haptics block. The stock kernel has no qpnp-haptic driver
# at all; the stock DT drives it with timed-gpio on TLMM GPIO79 (vib-gpio = <&tlmm 79 0>, pinctrl output-low, high = on).
# So PLAY (0x70) never mattered. This test: a6l_vib_ovl.ko adds /a6l-vibrator (gpio79, active high) to the live DT,
# a6l_gpio_vib.ko binds it (input FF device "a6l_gpio_vibrator" + debugfs pulse).
#   MODE=vib   : unload the PM660 haptics module, load overlay + driver, 3 bounded debugfs pulses (100/300/600 ms), then
#                the input-FF path (a6l_vib 65535 400 a6l_gpio_vibrator) - ASK PIERRE after each: felt? (y/n)
#   MODE=gpio  : read-only: gpio79 state in /sys/kernel/debug/gpio + pinmux-pins (before loading anything)
#   MODE=off   : force the motor off (pulse 1 ms) and unload the driver
# usage: adb shell 'export PATH=/tmp/bin:$PATH; export D=/tmp/rest3 MODE=vib; sh /tmp/rest3/run-rest3.sh'
export PATH=/tmp/bin:$PATH
D=${D:-/tmp/rest3}
say() { echo "A6L_REST3 $*"; }
mount -t debugfs none /sys/kernel/debug 2>/dev/null
P=/sys/kernel/debug/a6l_gpio_vib/pulse
gpio79() { grep -E "gpio79[ :]|-79 |\(gpio79\)| 79 " /sys/kernel/debug/gpio 2>/dev/null | head -n 2; grep -E "^pin 79 " /sys/kernel/debug/pinctrl/*/pinmux-pins 2>/dev/null | head -n 1; }
case "$MODE" in
gpio)
  say "GPIO79: $(gpio79 | tr '\n' ' ')"; say GPIO_DONE ;;
vib)
  rmmod a6l_pm660_haptics 2>/dev/null; rmmod qcom_spmi_haptics 2>/dev/null
  say "before: $(gpio79 | tr '\n' ' ')"
  grep -q '^a6l_vib_ovl ' /proc/modules || insmod $D/a6l_vib_ovl.ko || { say "VIB_FAIL overlay insmod"; dmesg | tail -5; exit 1; }
  grep -q '^a6l_gpio_vib ' /proc/modules || insmod $D/a6l_gpio_vib.ko || { say "VIB_FAIL driver insmod"; dmesg | tail -5; exit 1; }
  sleep 1; dmesg | grep -E "A6L_VIB|a6l-vibrator|gpio-vib" | tail -5
  [ -e $P ] || { say "VIB_FAIL no $P (driver did not bind: see dmesg above)"; exit 1; }
  say "after probe: $(gpio79 | tr '\n' ' ')"
  for ms in 100 300 600; do
    say "VIB_PULSE $ms ms - ASK PIERRE: felt? (y/n)"
    echo $ms > $P || say "pulse write failed"
    sleep 1; say "  during/after: $(gpio79 | tr '\n' ' ')"; sleep 2
  done
  EV=$(grep -l a6l_gpio_vibrator /sys/class/input/event*/device/name 2>/dev/null | head -1 | sed 's#.*/\(event[0-9]*\)/device/name#\1#')
  [ -n "$EV" ] && [ ! -e /dev/input/$EV ] && { mkdir -p /dev/input; mknod /dev/input/$EV c 13 $((64 + ${EV#event})); }
  say "VIB_FF 400 ms via input FF ($EV, as the Android VibratorOL HAL does) - ASK PIERRE: felt?"
  $D/a6l_vib 65535 400 a6l_gpio_vibrator; sleep 1
  say "on_count=$(cat /sys/kernel/debug/a6l_gpio_vib/on_count 2>/dev/null) final: $(gpio79 | tr '\n' ' ')"
  dmesg | grep -E "A6L_VIB" | tail -12
  say "VIB_DONE (pass = Pierre felt the pulses; gpio79 reads 'out lo' after each)" ;;
off)
  [ -e $P ] && echo 1 > $P; sleep 1; rmmod a6l_gpio_vib 2>/dev/null; say "OFF $(gpio79 | tr '\n' ' ')" ;;
*) sed -n '2,12p' "$0"; exit 1 ;;
esac
