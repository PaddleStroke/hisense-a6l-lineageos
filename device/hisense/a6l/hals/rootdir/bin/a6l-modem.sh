#!/vendor/bin/sh
# Hisense A6L modem helper (agent hals). prep | start | stop | status
# prep : read-only copies of modemst1/modemst2/fsg/fsc into /mnt/vendor/rmtfs (tmpfs), then vendor.a6l.modem.prep=done
# start: refuse unless diag-router is alive and persist.vendor.a6l.radio.enable=1, then start the mss remoteproc
# stop : stop the mss remoteproc, then the companion services
D=/mnt/vendor/rmtfs
log_() { log -t a6l-modem "$*"; echo "A6L_MODEM $*"; echo "A6L_MODEM $*" > /dev/kmsg 2>/dev/null; }
part_dev() {  # PARTNAME -> /dev/block node (by-name first, sysfs fallback)
    [ -e /dev/block/by-name/$1 ] && { echo /dev/block/by-name/$1; return 0; }
    for u in /sys/class/block/*/uevent; do
        if grep -q "^PARTNAME=$1$" $u; then n=$(grep "^DEVNAME=" $u | cut -d= -f2); [ -e /dev/block/$n ] && { echo /dev/block/$n; return 0; }; fi
    done
    return 1
}
mss() { for r in /sys/class/remoteproc/remoteproc*; do case "$(cat $r/name 2>/dev/null)" in *4080000*|mss|modem) echo $r; return 0;; esac; done; return 1; }
case "$1" in
prep)
    grep -q " $D tmpfs " /proc/mounts || { log_ "FAIL $D is not tmpfs; refusing"; setprop vendor.a6l.modem.prep fail; exit 1; }
    for pair in modemst1:modem_fs1 modemst2:modem_fs2 fsg:modem_fsg fsc:modem_fsc; do
        p=${pair%%:*}; f=${pair##*:}
        src=$(part_dev $p) || { log_ "FAIL partition $p not found"; setprop vendor.a6l.modem.prep fail; exit 1; }
        [ -s $D/$f ] || dd if=$src of=$D/$f bs=1048576 2>/dev/null || { log_ "FAIL copy $p"; setprop vendor.a6l.modem.prep fail; exit 1; }
        log_ "copy $p -> $D/$f $(stat -c %s $D/$f) bytes"
    done
    setprop vendor.a6l.modem.prep done ;;
start)
    [ "$(getprop persist.vendor.a6l.radio.enable)" = 1 ] || { log_ "radio not enabled"; exit 0; }
    i=0; while [ $i -lt 10 ] && [ "$(getprop init.svc.vendor.diag-router)" != running ]; do sleep 1; i=$((i+1)); done
    [ "$(getprop init.svc.vendor.diag-router)" = running ] || { log_ "FAIL diag-router not running; modem NOT started"; exit 1; }
    sleep 2
    [ "$(getprop init.svc.vendor.diag-router)" = running ] || { log_ "FAIL diag-router died; modem NOT started"; exit 1; }
    i=0; M=""; while [ $i -lt 20 ] && [ -z "$M" ]; do M=$(mss) || { sleep 1; i=$((i+1)); }; done
    [ -n "$M" ] || { log_ "FAIL no mss remoteproc (qcom_q6v5_mss loaded?)"; exit 1; }
    [ "$(cat $M/state)" = running ] || echo start > $M/state
    i=0; while [ $i -lt 40 ] && [ "$(cat $M/state)" != running ]; do sleep 1; i=$((i+1)); done
    log_ "state=$(cat $M/state) after ${i}s ($M)"
    setprop vendor.a6l.modem.state $(cat $M/state)
    [ "$(cat $M/state)" = running ] && start vendor.a6l-wlan-mac ;;
stop)
    M=$(mss) && [ "$(cat $M/state)" = running ] && echo stop > $M/state
    sleep 2; stop vendor.diag-router; stop vendor.tqftpserv; stop vendor.rmtfs
    setprop vendor.a6l.modem.prep ""
    setprop vendor.a6l.modem.state offline; log_ "stopped" ;;
status)
    M=$(mss); echo "mss=$M state=$(cat $M/state 2>/dev/null) prep=$(getprop vendor.a6l.modem.prep) diag=$(getprop init.svc.vendor.diag-router) rmtfs=$(getprop init.svc.vendor.rmtfs) tqftp=$(getprop init.svc.vendor.tqftpserv)" ;;
*) echo "usage: a6l-modem.sh prep|start|stop|status"; exit 1 ;;
esac
