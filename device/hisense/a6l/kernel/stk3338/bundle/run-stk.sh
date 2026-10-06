#!/system/bin/sh
# stk agent (25 Sep 2026): FRONT STK3338 light/proximity test for the V74 recovery RAM session. Offline-prepared.
#   MODE=probe  : READ-ONLY. I2C register reads (PID 0x3E) at 0x47 and 0x67 (+0x20 a96t346 as a bus reference)
#                 before and after a6l_stk_ovl.ko (always-on votes on pm660l L3 = sensor vdd, pm660 L13 = vio).
#                 The only change is the L3 enable (3.0 V, value already fixed in the V74 DT; stock keeps it on):
#                 Pierre's go.
#   MODE=data   : insmod stk3338_a6l.ko (power, stock address search 0x67/0x47, soft reset, stock presets),
#                 then 30 s of proximity/light samples + IIO near/far events. COVER the front sensor (top edge,
#                 next to the earpiece) when asked, then uncover.
# usage: adb shell 'export PATH=/tmp/bin:$PATH; export D=/tmp/stk MODE=probe; sh /tmp/stk/run-stk.sh'
export PATH=/tmp/bin:$PATH
D=${D:-/tmp/stk}
say() { echo "A6L_STK $*"; }
mount -t debugfs none /sys/kernel/debug 2>/dev/null
chmod 755 $D/a6l_i2cprobe $D/a6l_iio_ev 2>/dev/null
BUS=""
for d in /sys/bus/i2c/devices/i2c-*; do
  case "$(readlink -f $d)" in *c1b6000*) BUS=${d##*/i2c-};; esac
done
say "bus c1b6000 = i2c-$BUS"
[ -n "$BUS" ] || { say "FAIL no i2c adapter for c1b6000 (blsp_i2c6 disabled?)"; exit 1; }
[ -e /dev/i2c-$BUS ] || mknod /dev/i2c-$BUS c 89 $BUS
rails() { grep -E " l3 | l13 |pm660l_l3|stk" /sys/kernel/debug/regulator/regulator_summary 2>/dev/null | head -8; }
scan() {
  for a in 47 67 20; do $D/a6l_i2cprobe /dev/i2c-$BUS $a 3e 2; done
  $D/a6l_i2cprobe /dev/i2c-$BUS 47 00 6; $D/a6l_i2cprobe /dev/i2c-$BUS 67 00 6
}
case "$MODE" in
probe)
  say "PROBE_BEFORE (V74 DT, no vote)"; rails; scan
  ls /sys/bus/i2c/devices/ | grep "^$BUS-" ; cat /sys/bus/i2c/devices/$BUS-0047/name 2>/dev/null
  grep -q a6l_stk_ovl /proc/modules || insmod $D/a6l_stk_ovl.ko; sleep 1
  dmesg | grep -E "A6L_STK_OVL|a6l_stk" | tail -3
  say "PROBE_AFTER (L3+L13 voted)"; rails; scan
  grep -E "gpio71[ :]|-71 " /sys/kernel/debug/gpio 2>/dev/null | head -2
  say "PROBE_DONE: pass = 'OK 0x47 reg 0x3e: <pid> <rid>' or 'OK 0x67 ...' with pid != 00. Record which address answers."
  ;;
data)
  grep -q a6l_stk_ovl /proc/modules || insmod $D/a6l_stk_ovl.ko
  grep -q stk3338_a6l /proc/modules || insmod $D/stk3338_a6l.ko ${STK_ARGS:-}
  sleep 1; dmesg | grep -E "A6L_STK|stk3338|stk3310" | tail -8
  IIO=""
  for n in /sys/bus/iio/devices/iio:device*; do [ "$(cat $n/name 2>/dev/null)" = stk3338 ] && IIO=$n; done
  [ -n "$IIO" ] || { say "DATA_FAIL no stk3338 IIO device (see dmesg above)"; exit 1; }
  say "iio=$IIO"; ls $IIO | tr '\n' ' '; echo
  for f in in_illuminance_scale in_proximity_scale in_illuminance_integration_time in_proximity_integration_time \
           events/in_proximity_thresh_rising_value events/in_proximity_thresh_falling_value; do
    echo "$f=$(cat $IIO/$f 2>/dev/null)"; done
  DEV=/dev/${IIO##*/}; [ -e $DEV ] || mknod $DEV c $(cut -d: -f1 $IIO/dev) $(cut -d: -f2 $IIO/dev)
  $D/a6l_iio_ev $DEV 32 &
  for i in $(seq 1 30); do
    case $i in 8) say ">>> COVER the front sensor now (finger over the top edge, next to the earpiece)";;
               18) say ">>> UNCOVER now";; 24) say ">>> point the screen at a lamp / cover with the palm (light)";; esac
    P=$(cat $IIO/in_proximity_raw 2>/dev/null); L=$(cat $IIO/in_illuminance_raw 2>/dev/null)
    I=$(grep -E "stk3310_event|stk3338" /proc/interrupts | awk '{s=0; for(i=2;i<=NF;i++) if ($i ~ /^[0-9]+$/) s+=$i; print s; exit}')
    say "SAMPLE $i prox=$P light=$L irqs=$I"; sleep 1
  done
  wait
  say "DATA_DONE: pass = prox rises well above ${STK_THD:-120} when covered and drops when uncovered, NEAR/FAR events, light changes"
  ;;
*) sed -n '2,11p' "$0"; exit 1 ;;
esac
