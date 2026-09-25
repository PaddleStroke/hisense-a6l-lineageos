# v74 light/proximity: minimal tmd3702 IIO driver on the TMD3702 at 0x49 (bus c176000). ATTENDED ONLY. No rails touched.
# Stock DT says als,position = "back" / ps,position = "back": the sensor looks out of the E-INK side of the phone.
# usage: D=/tmp/als sh run.sh     (optional: A6L_TMD_PARAMS="pdrive=2 prox=0")
# Proximity uses the module's IR VCSEL at pdrive=4 (10 mA, stock DT asks 9 = beyond the datasheet table); prox=0 disables it.
B=""; for b in /sys/bus/i2c/devices/i2c-*; do readlink -f "$b" | grep -q "c176000" && B=${b##*/i2c-}; done
[ -n "$B" ] || { echo "A6L_HW_FAIL bus c176000 not found (load the i2c/geni driver first: e-ink pmic bundle)"; ls /sys/bus/i2c/devices/; exit 5; }
echo "A6L_ALS bus=i2c-$B"
grep -q "^tmd3702 " /proc/modules || insmod "$D/modules/tmd3702.ko" ${A6L_TMD_PARAMS:-} || { echo "A6L_HW_FAIL insmod tmd3702"; klog | tail -n 10; exit 4; }
[ -e /sys/bus/i2c/devices/$B-0049 ] || echo "tmd3702 0x49" > /sys/bus/i2c/devices/i2c-$B/new_device
sleep 1; klog | grep -i "tmd3702\|0049" | tail -n 6
I=""; for d in /sys/bus/iio/devices/iio:device*; do [ "$(cat $d/name 2>/dev/null)" = tmd3702 ] && I=$d; done
[ -n "$I" ] || { echo "A6L_ALS_NOT_BOUND"; echo 0x49 > /sys/bus/i2c/devices/i2c-$B/delete_device 2>/dev/null; exit 6; }
echo "A6L_ALS iio=$I int_time=$(cat $I/in_intensity_integration_time 2>/dev/null) gain=$(cat $I/in_intensity_hardwaregain 2>/dev/null)"
rd() { echo "A6L_ALS_READ $1 lux~$(cat $I/in_illuminance_input 2>&1) C=$(cat $I/in_intensity_clear_raw 2>&1) R=$(cat $I/in_intensity_red_raw 2>&1) G=$(cat $I/in_intensity_green_raw 2>&1) B=$(cat $I/in_intensity_blue_raw 2>&1) P=$(cat $I/in_proximity_raw 2>&1)"; }
rd open1; sleep 1; rd open2
echo "ASK PIERRE: cover the sensor window on the e-ink side (finger ~1 cm away, not touching) for 5 s"; sleep 5; rd covered1; sleep 1; rd covered2
echo "ASK PIERRE: shine a light (phone torch) on it"; sleep 5; rd bright
C1=$(cat $I/in_intensity_clear_raw 2>/dev/null); [ -n "$C1" ] && echo A6L_ALS_READ_PASS
# leave it bound for the sensors HAL work? No: unbind so the next test starts clean.
echo 0x49 > /sys/bus/i2c/devices/i2c-$B/delete_device; rmmod tmd3702; echo "A6L_ALS_DONE"
