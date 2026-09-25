# v74 als2: tmd3702.ko v2 (stock-equivalent proximity set-up) on the TMD3702 at 0x49 (bus c176000). ATTENDED ONLY.
# The sensor faces the E-INK side (stock DT als/ps,position = "back"). No rails or GPIOs are touched.
# usage (run twice, Pierre holds the state for the whole run, ~40 s each; nothing to time):
#   D=/tmp/als2 PHASE=open  sh run.sh    # nothing within 20 cm of the e-ink-side sensor window
#   D=/tmp/als2 PHASE=cover sh run.sh    # finger/palm flat ON the sensor window (touching) for the whole run
#   D=/tmp/als2 PHASE=near  sh run.sh    # optional: hand held ~3 cm above the window
# Each run binds the driver (probe measures the crosstalk; do the FIRST run with PHASE=open), then cycles live through
# register variants (debug_write=1 -> reg_write attribute) and prints 5 PDATA samples per variant:
#   stock  : PCFG1=0x09 CFG6=0x7F (APC off), CFG4/TEST3 at reset (0x3F/0x44)   <- what the stock Hisense driver programs
#   ds     : stock + CFG4=0x3D TEST3=0xC4 (datasheet "must" values)
#   apc    : stock but CFG6=0x3F (APC on)
#   v1     : CFG4=0x3D TEST3=0xC4 CFG6=0x3F PCFG1=0x04  (= yesterday's v1 driver, expected flat ~90)
#   drv4   : stock with PCFG1=0x04 (10 mA)
#   gain4  : stock with PCFG1=0x89 (PGAIN 4x)
# Pass = in at least one variant (expected: stock) P(cover) >> P(open) (stock near threshold = crosstalk + 58).
PHASE=${PHASE:-open}; case "$PHASE" in open|cover|near) ;; *) echo "A6L_HW_FAIL PHASE must be open|cover|near"; exit 7;; esac
B=""; for b in /sys/bus/i2c/devices/i2c-*; do readlink -f "$b" | grep -q "c176000" && B=${b##*/i2c-}; done
[ -n "$B" ] || { echo "A6L_HW_FAIL bus c176000 not found (load the i2c/geni driver first: e-ink pmic bundle)"; ls /sys/bus/i2c/devices/; exit 5; }
echo "A6L_ALS2 bus=i2c-$B phase=$PHASE"
if grep -q "^tmd3702 " /proc/modules; then
  [ -e /sys/bus/i2c/devices/$B-0049 ] && echo 0x49 > /sys/bus/i2c/devices/i2c-$B/delete_device
  rmmod tmd3702 || { echo "A6L_HW_FAIL old tmd3702 still loaded"; exit 4; }
fi
insmod "$D/modules/tmd3702.ko" debug_write=1 ${A6L_TMD_PARAMS:-} || { echo "A6L_HW_FAIL insmod tmd3702"; klog | tail -n 10; exit 4; }
echo "tmd3702 0x49" > /sys/bus/i2c/devices/i2c-$B/new_device
sleep 1; klog | grep -i "tmd3702\|0049" | tail -n 6
I=""; for d in /sys/bus/iio/devices/iio:device*; do [ "$(cat $d/name 2>/dev/null)" = tmd3702 ] && I=$d; done
[ -n "$I" ] || { echo "A6L_ALS_NOT_BOUND"; echo 0x49 > /sys/bus/i2c/devices/i2c-$B/delete_device 2>/dev/null; rmmod tmd3702; exit 6; }
[ -w "$I/reg_write" ] || { echo "A6L_HW_FAIL old module without reg_write (check SHA256SUMS)"; exit 6; }
echo "A6L_ALS2 crosstalk=$(cat $I/prox_crosstalk) (measured at probe; meaningful only in PHASE=open)"
w() { echo "$1 $2" > $I/reg_write || echo "  WRITE_FAIL $1 $2"; }
variant() { # name pcfg1 cfg6 cfg4 test3
  w 80 01; w 8f $2; w ae $3; w ac $4; w f2 $5; w 93 ff; w 80 0f; sleep 1
  s=""; i=0; while [ $i -lt 5 ]; do s="$s $(cat $I/in_proximity_raw 2>&1)"; sleep 0.3; i=$((i+1)); done
  echo "A6L_PROX phase=$PHASE variant=$1 P=[$s ] near=$(cat $I/prox_near) C=$(cat $I/in_intensity_clear_raw) regs: $(cat $I/regs | tr ' ' '\n' | grep '^\(8f\|93\|9c\|9d\|ac\|ae\|f2\)=' | tr '\n' ' ')"; }
echo "A6L_ALS2 regs at probe: $(cat $I/regs)"
variant stock 09 7f 3f 44
variant ds    09 7f 3d c4
variant apc   09 3f 3f 44
variant v1    04 3f 3d c4
variant drv4  04 7f 3f 44
variant gain4 89 7f 3f 44
variant stock 09 7f 3f 44
echo "A6L_ALS2 lux~$(cat $I/in_illuminance_input) C=$(cat $I/in_intensity_clear_raw) (ALS must still follow the light)"
klog | grep -i tmd3702 | tail -n 4
echo 0x49 > /sys/bus/i2c/devices/i2c-$B/delete_device; rmmod tmd3702; echo "A6L_ALS2_DONE phase=$PHASE"
