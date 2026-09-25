#!/system/bin/sh
# misc agent (24 Sep 2026): haptics fix test + Hall test for the V74 recovery RAM session. Offline-prepared.
#   MODE=haptics  : reload a6l_pm660_haptics.ko (stock parity: ilim 800 mA, SC debounce 8, INT_PWM 505 kHz),
#                   then debugfs test pulses with register snapshots DURING play (kernel log "A6L_HAP during"),
#                   then the normal input-FF path (a6l_vib) with auto-res on. Ask Pierre after EACH pulse.
#   MODE=hall     : a6l_hall_ovl.ko (L13 vote + gpio-keys SW_LID, pull-up) then 20 s of gpio75/IRQ/SW_LID samples
#                   (read-only; the only change is the L13 enable vote = a 1.8 V rail stock keeps on: Pierre's go).
# usage: adb shell 'export PATH=/tmp/bin:$PATH; export D=/tmp/rest2 MODE=haptics; sh /tmp/rest2/run-rest2.sh'
export PATH=/tmp/bin:$PATH
D=${D:-/tmp/rest2}
say() { echo "A6L_REST2 $*"; }
mount -t debugfs none /sys/kernel/debug 2>/dev/null
case "$MODE" in
haptics)
  T=/sys/kernel/debug/a6l_haptics/test
  rmmod a6l_pm660_haptics 2>/dev/null; rmmod qcom_spmi_haptics 2>/dev/null
  insmod $D/a6l_pm660_haptics.ko ${HAP_ARGS:-} || { say HAPTICS_FAIL insmod; exit 1; }
  sleep 1; dmesg | grep "A6L_HAP" | tail -3
  [ -e $T ] || { say HAPTICS_FAIL no $T; exit 1; }
  pulse() {  # ms mv rate_us autores erm  + question
    say "HAPTICS_TEST pulse $* - ASK PIERRE: felt? (y/n)"
    echo "pulse $*" > $T || say "HAPTICS_TEST write failed (EINVAL = out of bounds, EBUSY = FF playing)"
    dmesg | grep "A6L_HAP" | tail -${SNAPS:-12}; sleep 2
  }
  pulse 300 3200 0 0 0          # DT drive period (6667 us, 150 Hz), no auto-res, stock vmax
  pulse 300 3200 0 1 0          # + PM660 auto-res (0x4B bit7) after 20 ms (stock QWD behaviour)
  for r in 5900 5000 4350; do   # LRA resonance sweep: 170 / 200 / 230 Hz
    pulse 300 3200 $r 1 0
  done
  [ "$HAP_ERM" = 1 ] && pulse 100 2000 0 0 1   # optional: ERM (DC) 100 ms 2 V - a single "click" proves the output stage
  echo 1 > /sys/module/a6l_pm660_haptics/parameters/pm660_autores 2>/dev/null
  EV=$(grep -l spmi_haptics /sys/class/input/event*/device/name 2>/dev/null | head -1 | sed 's#.*/\(event[0-9]*\)/device/name#\1#')
  [ -n "$EV" ] && [ ! -e /dev/input/$EV ] && { mkdir -p /dev/input; mknod /dev/input/$EV c 13 $((64 + ${EV#event})); }
  say "HAPTICS_FF 65535 300 ms via input FF (auto-res on) - ASK PIERRE: felt?"
  $D/a6l_vib 65535 300; sleep 1
  cat /sys/kernel/debug/a6l_haptics/regs
  dmesg | grep -E "A6L_HAP|short-circuit|haptic" | tail -40 > /tmp/rest2-haptics-klog.txt
  say "HAPTICS_DONE (klog /tmp/rest2-haptics-klog.txt)"
  ;;
hall)
  grep -q "gpio_keys\|gpio-keys" /proc/modules /sys/bus/platform/drivers 2>/dev/null; ls /sys/bus/platform/drivers | grep -q gpio-keys || insmod $D/gpio_keys.ko 2>/dev/null
  insmod $D/a6l_hall_ovl.ko; sleep 2
  dmesg | grep -E "A6L_HALL|hall|gpio-keys" | tail -5
  grep -E "hall|l13" /sys/kernel/debug/regulator/regulator_summary 2>/dev/null
  EV=$(grep -l "hall" /sys/class/input/event*/device/name 2>/dev/null | head -1 | sed 's#.*/\(event[0-9]*\)/device/name#\1#')
  say "input=$EV"
  for i in $(seq 1 20); do
    L=$(grep -E "gpio75[ :]|-75 |Hall" /sys/kernel/debug/gpio 2>/dev/null | head -1)
    I=$(grep -i "hall" /proc/interrupts | awk '{s=0; for(i=2;i<=NF;i++) if ($i ~ /^[0-9]+$/) s+=$i; print s; exit}')
    S=$( [ -n "$EV" ] && cat /sys/class/input/$EV/device/capabilities/sw 2>/dev/null)
    say "HALL_SAMPLE $i gpio='$L' irqs=$I (move the flip cover / a strong magnet over the phone's top half)"; sleep 1
  done
  say "HALL_DONE: pass = the gpio line toggles hi->lo and irqs increase when the magnet is close"
  ;;
*) sed -n '2,9p' "$0"; exit 1 ;;
esac
