#!/vendor/bin/sh
# Hisense A6L sensors prep (agent hals): make sure the ADSP runs (SMGR over QRTR -> IIO accel/gyro/mag), wait for the
# IIO devices, then hand their sysfs attributes to uid system (the multihal runs as system; SELinux permissive does
# not bypass DAC). Runs blocking (exec_start) before class hal so the IIO sub-HAL sees every sensor on first probe.
# Needs (loaded by the flash agent's module list): qcom_q6v5_pas (adsp), qcom_sns_reg (+ this phone's sns.reg as
# firmware), qcom_smgr + qcom_smgr_{accel,gyro,mag}, tmd3702.
log_() { log -t a6l-sensors "$*"; echo "A6L_SENSORS $*" > /dev/kmsg 2>/dev/null; }
WAIT=${1:-20}
for r in /sys/class/remoteproc/remoteproc*; do
    case "$(cat $r/name 2>/dev/null)" in *adsp*|*15700000*) A=$r;; esac
done
if [ -n "$A" ]; then
    [ "$(cat $A/state)" = running ] || echo start > $A/state
    log_ "adsp $A state=$(cat $A/state)"
else
    log_ "no adsp remoteproc"
fi
i=0; n=0
while [ $i -lt $WAIT ]; do
    n=$(ls -d /sys/bus/iio/devices/iio:device* 2>/dev/null | wc -l)
    names=$(cat /sys/bus/iio/devices/iio:device*/name 2>/dev/null | tr '\n' ' ')
    case "$names" in *accel*gyro*|*gyro*accel*) break;; esac
    sleep 1; i=$((i+1))
done
for d in /sys/bus/iio/devices/iio:device* /sys/bus/iio/devices/trigger*; do
    [ -e "$d" ] || continue
    for f in $d/sampling_frequency $d/buffer/enable $d/buffer/length $d/buffer/watermark $d/trigger/current_trigger $d/scan_elements/*_en $d/*_raw $d/*_input $d/*_scale; do
        [ -e "$f" ] && chown system:system "$f" && chmod 0664 "$f" 2>/dev/null
    done
    dev=/dev/$(basename $d); [ -c "$dev" ] && chown system:system $dev && chmod 0660 $dev
    log_ "$(basename $d) name=$(cat $d/name 2>/dev/null)"
done
log_ "iio devices=$n after ${i}s: $names"
setprop vendor.a6l.sensors.ready 1
exit 0
