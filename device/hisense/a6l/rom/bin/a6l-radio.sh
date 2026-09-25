#!/vendor/bin/sh
# A6L rom-v1 radio bring-up (agent flash). Modem + Wi-Fi + Bluetooth, the radio2 sequence proven on 23 Sep
# (diag-router over QRTR before the modem, 300 s crashes=0, Wi-Fi scan, real MAC from persist; BT hci0).
# RF: runs only when persist.vendor.a6l.radio=1 (set by Pierre). Logged to kmsg with prefix A6L_ROM_RADIO.
# usage: a6l-radio.sh start | rmtfs | diag | stop
M=/vendor/lib/modules
B=/vendor/a6l/radio/bin      # radio2 binaries as proven (rmtfs, tqftpserv, diag-router, qrtr-lookup + libqrtr/libc++)
log() { echo "A6L_ROM_RADIO $*"; }
loaded() { grep -q "^$(echo "$1" | tr - _) " /proc/modules; }
load_list() {
    while read -r ko params; do
        case "$ko" in ''|'#'*) continue;; esac
        loaded "${ko%.ko}" && continue
        insmod "$M/$ko" $params && log "insmod $ko ok" || log "insmod $ko FAILED"
    done < /vendor/etc/a6l/modules/$1.txt
}
mss() { for r in /sys/class/remoteproc/remoteproc*; do case "$(cat $r/name 2>/dev/null)" in *4080000*|mss|modem) echo $r; return;; esac; done; }
case "$1" in
rmtfs)
    # REAL modem EFS partitions (/dev/block/by-name/modemst1, modemst2, fsg, fsc). -r = read-only (default in rom-v1:
    # EFS writes stay in RAM until reboot). persist.vendor.a6l.rmtfs_rw=1 = write-through like stock.
    ro="-r"; [ "$(getprop persist.vendor.a6l.rmtfs_rw)" = 1 ] && ro=""
    log "rmtfs partitions mode=${ro:-read-write}"
    export LD_LIBRARY_PATH=$B
    exec $B/rmtfs -P $ro -v
    ;;
diag)
    export A6L_DIAG_EDGES=modem A6L_DIAG_QRTR=modem
    exec $B/diag-router
    ;;
tqftpserv)
    export LD_LIBRARY_PATH=$B
    exec $B/tqftpserv
    ;;
start)
    [ "$(getprop persist.vendor.a6l.radio)" = 1 ] || { log "radio disabled (persist.vendor.a6l.radio != 1)"; exit 0; }
    # radio2 order: qrtr + daemons (rmtfs, tqftpserv, diag-router) BEFORE the modem stack is loaded/started
    for m in qrtr qrtr-smd; do loaded $m || insmod $M/$m.ko; done
    i=0; while [ $i -lt 50 ] && [ ! -c /dev/qcom_rmtfs_mem1 ]; do sleep 0.2; i=$((i+1)); done
    log "rmtfs_mem1=$([ -c /dev/qcom_rmtfs_mem1 ] && echo yes || echo no)"
    setprop ctl.start vendor.rmtfs
    setprop ctl.start vendor.tqftpserv
    setprop ctl.start vendor.diag_router
    sleep 2
    [ "$(getprop init.svc.vendor.diag_router)" = running ] || { log "diag-router not running: modem NOT started (it would crash on diag starvation)"; exit 1; }
    load_list radio
    r=""; i=0; while [ $i -lt 20 ] && [ -z "$r" ]; do r=$(mss); [ -n "$r" ] || { sleep 1; i=$((i+1)); }; done
    [ -n "$r" ] || { log "no mss remoteproc"; exit 1; }
    [ "$(cat $r/state)" = running ] || echo start > $r/state
    i=0; while [ $i -lt 40 ] && [ "$(cat $r/state)" != running ]; do sleep 1; i=$((i+1)); done
    log "modem ${r##*/} state=$(cat $r/state) after ${i}s"
    # Wi-Fi MAC: stock /persist/wlan_mac.bin "Intf0MacAddress=XXXXXXXXXXXX" (persist is mounted read-only)
    i=0; while [ $i -lt 90 ] && [ ! -e /sys/class/net/wlan0 ]; do sleep 1; i=$((i+1)); done
    if [ -e /sys/class/net/wlan0 ]; then
        mac=$(grep -a -o 'Intf0MacAddress=[0-9A-Fa-f]\{12\}' /mnt/vendor/persist/wlan_mac.bin 2>/dev/null | head -n 1 | cut -d= -f2 | sed 's/\(..\)/\1:/g; s/:$//' | tr 'A-F' 'a-f')
        if [ -n "$mac" ]; then ip link set wlan0 down; ip link set wlan0 address "$mac" && log "wlan0 mac $mac (persist)" || log "wlan0 mac set failed"; fi
        log "wlan0 present after ${i}s addr=$(cat /sys/class/net/wlan0/address)"
    else
        log "wlan0 missing after 90 s"
    fi
    load_list bt
    # full variant: public BD address for hci0 (a6l_macs, agent hals); harmless no-op when absent
    [ -x /vendor/bin/a6l_macs ] && setprop ctl.start vendor.a6l-bt-addr
    ;;
stop)
    r=$(mss); [ -n "$r" ] && [ "$(cat $r/state)" = running ] && echo stop > $r/state
    for s in vendor.diag_router vendor.tqftpserv vendor.rmtfs; do setprop ctl.stop $s; done
    log "stopped"
    ;;
esac
exit 0
