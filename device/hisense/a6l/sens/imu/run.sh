#!/system/bin/sh
# ATTENDED ONLY, V74 diagnostic recovery (V71 controls image). v75/sens (sens agent, 26 Sep 2026): motion sensors + COMPASS
# through the ADSP Sensor Manager (SMGR): BMI160 accel/gyro + AKM AK09918 magnetometer live on the ADSP's own I2C bus (stock
# sensor_def_qcomdev.conf "SSI SMGR Cfg 2 AK09911_FIFO", i2c_bus 3; there is NO AK node in the stock AP device tree), so the
# path is qcom_sns_reg (registry server with this phone's sns.reg) -> ADSP -> qcom_smgr -> IIO qcom-smgr-accel/-gyro/-mag.
# Same modules as the V71 sensors-adsp bundle (proven 21 Sep: IIO accel, gyro, mag). Works in the same boot as audio6/speaker.
# usage: export PATH=/tmp/bin:$PATH; D=/tmp/sens MODE=<mode> sh /tmp/sens/run.sh
#   pre   : FRESH BOOT, BEFORE the ADSP bundle (next to audio6/speaker MODE=ovl): registry server.  PASS = A6L_SENS_PRE_PASS
#   load  : after the ADSP bundle: QRTR/SMGR modules, waits <= 20 s for the 3 IIO devices.      PASS = A6L_SENS_LOAD_PASS
#   flat  : 10 s, phone FLAT on the table SCREEN UP, nobody touches it.   -> ACCEL_PASS, GYRO_REST_PASS, gravity axis
#   turn  : 30 s, Pierre turns the phone SLOWLY, still flat on the table, one FULL turn (~20 s per turn) -> GYRO_TURN, MAG
#   tilt  : 15 s, tilt left/right then forward/back (~60 deg)             -> TILT_PASS
#   info  : names, scales, rates, mount matrix, SMGR klog. No data.
#   Options: MAP=+y+x-z (raw SMGR -> Android axes, default), SECS=n, UNIT=auto|gauss|ut.
set -u
D=${D:-$(dirname "$0")}
[ "$(tr -d '\0' < /proc/device-tree/chosen/hisense,a6l-controls 2>/dev/null)" = v71 ] || { echo A6L_HW_FAIL wrong image; exit 2; }
hashchk() { ( cd "$D" || exit 1; n=0; while read -r h f; do [ -n "$f" ] || continue; f=${f#\*}
    [ -f "$f" ] || { echo "A6L_HASH missing $f"; exit 1; }
    set -- $(sha256sum "$f"); [ "$1" = "$h" ] || { echo "A6L_HASH mismatch $f"; exit 1; }; n=$((n+1)); done < SHA256SUMS
    [ $n -gt 0 ] || exit 1; echo "A6L_HASH_OK $n files" ); }
hashchk || { echo A6L_HW_FAIL payload hash; exit 3; }
MARK="A6L_SENS_$$"; echo "$MARK" > /dev/kmsg
klog() { dmesg | sed -n "/$MARK/,\$p"; }
MODE=${MODE:-info}; MAP=${MAP:-+y+x-z}; UNIT=${UNIT:-auto}; T=$D/bin; chmod 755 "$T"/* 2>/dev/null
adsp_rproc() { for r in /sys/class/remoteproc/remoteproc*; do [ "$(cat "$r/name" 2>/dev/null)" = adsp ] && { echo "$r"; return 0; }; done
  for r in /sys/class/remoteproc/remoteproc*; do case "$(readlink -f "$r/device" 2>/dev/null)" in *15700000*) echo "$r"; return 0;; esac; done; return 1; }
adsp_state() { r=$(adsp_rproc) || return 0; cat "$r/state" 2>/dev/null; }
loaded() { grep -q "^$(echo "${1%.ko}" | tr - _) " /proc/modules; }
ins() { loaded "$1" && return 0; insmod "$D/modules/$1" || { echo "A6L_HW_FAIL insmod $1"; klog | tail -n 15; exit 4; }; }
iio_list() { for d in /sys/bus/iio/devices/iio:device*; do [ -e "$d/name" ] && echo "${d##*/} $(cat "$d/name")"; done; }
have() { iio_list | grep -q " $1\$"; }
mkiio() { for d in /sys/bus/iio/devices/iio:device*; do n=${d##*/}; [ -r "$d/dev" ] || continue; [ -e "/dev/$n" ] && continue
  mm=$(cat "$d/dev"); mknod "/dev/$n" c "${mm%%:*}" "${mm##*:}"; done; }
echo "A6L_SENS adsp=$(adsp_rproc) state=$(adsp_state) mode=$MODE"

case "$MODE" in
pre)
  mkdir -p /lib/firmware/qcom/sensors && cp "$D/firmware/qcom/sensors/sns.reg" /lib/firmware/qcom/sensors/ || { echo "A6L_HW_FAIL cannot copy sns.reg to /lib/firmware"; exit 4; }
  [ "$(adsp_state)" = running ] && echo "A6L_NOTE adsp already running: the registry arrives late (SMGR may not come up; reboot if load fails)"
  while read -r ko; do [ -n "$ko" ] || continue; ins "$ko"; [ "$ko" = qcom_sns_reg.ko ] && break; done < "$D/modules/order.txt"
  loaded qcom_sns_reg.ko && echo "A6L_SENS_PRE_PASS registry server loaded (sns.reg $(wc -c < /lib/firmware/qcom/sensors/sns.reg) bytes); now the ADSP bundle, then MODE=load" || echo A6L_SENS_PRE_FAIL;;
load)
  loaded qcom_sns_reg.ko || echo "A6L_WARN MODE=pre was not run before the ADSP: SMGR may never appear"
  [ "$(adsp_state)" = running ] || { echo "A6L_HW_FAIL adsp not running (run the adsp bundle first)"; exit 5; }
  while read -r ko; do [ -n "$ko" ] || continue; ins "$ko"; done < "$D/modules/order.txt"
  n=0; while [ $n -lt 20 ]; do have qcom-smgr-accel && have qcom-smgr-gyro && have qcom-smgr-mag && break; sleep 1; n=$((n+1)); done
  mkiio; iio_list | sed 's/^/  IIO /'
  klog | grep -iE "smgr|sns|registry|qrtr" | tail -n 15 | sed 's/^/  KLOG: /'
  ok=1; for s in accel gyro mag; do have qcom-smgr-$s || { echo "  MISSING qcom-smgr-$s"; ok=0; }; done
  [ $ok = 1 ] && echo "A6L_SENS_LOAD_PASS accel gyro mag after ${n}s" || echo "A6L_SENS_LOAD_FAIL (see KLOG; mag missing alone = AK09918 not in the SMGR sensor list)";;
info)
  mkiio; for d in /sys/bus/iio/devices/iio:device*; do case "$(cat $d/name 2>/dev/null)" in qcom-smgr-*) ;; *) continue;; esac
    echo "  ${d##*/} $(cat $d/name) buffer=$(cat $d/buffer/enable 2>/dev/null)"
    for f in $d/in_*_scale $d/in_*_sampling_frequency $d/in_*_sampling_frequency_available $d/in_*mount_matrix $d/scan_elements/*_type; do
      [ -e "$f" ] && echo "    ${f#$d/} = $(cat $f 2>/dev/null)"; done; done
  dmesg | grep -iE "smgr|sns_reg|registry" | tail -n 20 | sed 's/^/  KLOG: /';;
flat|turn|tilt)
  for s in accel gyro mag; do have qcom-smgr-$s || echo "  WARNING qcom-smgr-$s missing (run MODE=load)"; done
  mkiio
  case "$MODE" in flat) S=${SECS:-10}; MSG="phone FLAT on the table, SCREEN UP, top edge pointing away from you; do NOT touch it";;
    turn) S=${SECS:-30}; MSG="keep it flat on the table and turn it SLOWLY one FULL turn (about 20 s), then a bit more, counter-clockwise seen from above";;
    tilt) S=${SECS:-15}; MSG="pick it up: tilt LEFT and RIGHT (~60 deg), then FORWARD and BACK (~60 deg)";; esac
  echo "ASK PIERRE NOW: $MSG  (starts in 5 s, lasts $S s)"; sleep 5
  "$T/a6l_imu" -n "$MODE" -s "$S" -c "$MODE" -m "$MAP" -u "$UNIT" -p 1000; rc=$?
  echo "A6L_SENS_${MODE}_RC $rc";;
*) echo "A6L_HW_FAIL unknown MODE"; exit 7;;
esac
echo "A6L_SENS_DONE mode=$MODE"
