#!/vendor/bin/sh
# A6L rom-v1/rom-v2 radio bring-up (agent flash; rom-v2 IPA hook by agent merge). Modem + Wi-Fi + Bluetooth, the radio2 sequence proven on 23 Sep
# (diag-router over QRTR before the modem, 300 s crashes=0, Wi-Fi scan, real MAC from persist; BT hci0).
# RF: runs only when persist.vendor.a6l.radio=1 (set by Pierre). Logged to kmsg with prefix A6L_ROM_RADIO.
# usage: a6l-radio.sh start | rmtfs | diag | stop
# r5 review fix F8 (28 Sep 2026): the modem is only started when every prerequisite is met. With IPA required (default),
# the IPA module group must load, ipa2-lite must be bound and rmnet_ipa0 must exist BEFORE any modem module (qcom_q6v5_mss)
# is inserted; module insertion errors are fatal for the modem; rmtfs, tqftpserv and diag-router must be running. Any
# unmet prerequisite => the modem is NOT started, vendor.a6l.radio.state=failed:<reason>, exit 1 (init logs the status).
# IPA-less diagnostic mode is explicit only: persist.vendor.a6l.radio.ipa_less_diag=1 (never the result of a failure).
# The start is written only when the MSS remoteproc is 'offline' (never a second start of a running/crashed modem).
# A6L_ROOT (host tests only, rom/tests/test-a6l-radio-start.sh) prefixes every file path; empty on the phone.
R=${A6L_ROOT:-}
M=$R/vendor/lib/modules
L=$R/vendor/etc/a6l/modules
B=/vendor/a6l/radio/bin      # radio2 binaries as proven (rmtfs, tqftpserv, diag-router, qrtr-lookup + libqrtr/libc++)
IPA_DEV=14780000.ipa
IPA_WAIT=${A6L_IPA_WAIT:-50}  # x0.2 s (deferred probe: ipa2-lite binds once its suppliers are up)
log() { echo "A6L_ROM_RADIO $*"; }
state() { setprop vendor.a6l.radio.state "$1" 2>/dev/null; }
loaded() { grep -q "^$(echo "$1" | tr - _) " $R/proc/modules; }
# load_list <group> [fatal_upto.ko]: returns 0 ok, 2 = a module failed at/before fatal_upto (the rest is NOT loaded),
# 1 = a later module failed (degraded). With no fatal_upto every failure is fatal.
load_list() {
    [ -f "$L/$1.txt" ] || { log "$1: no module list"; return 2; }
    fatal=1; bad=0
    while read -r ko params; do
        case "$ko" in ''|'#'*) continue;; esac
        if ! loaded "${ko%.ko}"; then
            if insmod "$M/$ko" $params; then log "insmod $ko ok"
            else
                rc=$?; log "insmod $ko FAILED rc=$rc"
                [ $fatal = 1 ] && return 2
                bad=1
            fi
        fi
        [ -n "$2" ] && [ "$ko" = "$2" ] && fatal=0
    done < "$L/$1.txt"
    return $bad
}
# r5 review fix F39: Bluetooth (WCN3990 BT over UART) does not depend on the modem. Its COMPLETE bring-up (modules +
# public BD address: the controller comes up UNCONFIGURED until a6l_macs sets it) runs on every start path that got
# past the RF gate: success, early fail() and the late "modem did not reach running" exit. a6l_macs is idempotent
# ("bt already set" on a configured controller), so a repeated start does not disrupt a working controller.
bt_up() {
    [ "${BT_TRIED:-0}" = 1 ] && return 0
    BT_TRIED=1
    load_list bt || log "bt: module(s) failed"
    if [ -x $R/vendor/bin/a6l_macs ]; then setprop ctl.start vendor.a6l-bt-addr; else log "bt: no a6l_macs (public address not set)"; fi
}
fail() {
    log "MODEM NOT STARTED: $1"; state "failed:$1"
    bt_up
    exit 1
}
mss() { for r in $R/sys/class/remoteproc/remoteproc*; do case "$(cat $r/name 2>/dev/null)" in *4080000*|mss|modem) echo $r; return;; esac; done; }
svc_wait() {   # all named init services running within ~5 s
    i=0
    while [ $i -lt 25 ]; do
        ok=1; for s in "$@"; do [ "$(getprop init.svc.$s)" = running ] || { ok=0; missing=$s; }; done
        [ $ok = 1 ] && return 0
        sleep 0.2; i=$((i+1))
    done
    return 1
}
case "$1" in
rmtfs)
    # REAL modem EFS partitions (/dev/block/by-name/modemst1, modemst2, fsg, fsc). -r = read-only (default in rom-v1:
    # EFS writes stay in RAM until reboot). persist.vendor.a6l.rmtfs_rw=1 = write-through like stock.
    ro="-r"; [ "$(getprop persist.vendor.a6l.rmtfs_rw)" = 1 ] && ro=""
    log "rmtfs partitions mode=${ro:-read-write}"
    export LD_LIBRARY_PATH=$B
    # rmtfs defaults to Linux /dev/disk/by-partlabel. Android's ueventd
    # publishes the same partition labels under /dev/block/by-name.
    exec $B/rmtfs -P -o /dev/block/by-name $ro -v
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
    [ "$(getprop persist.vendor.a6l.radio)" = 1 ] || { log "radio disabled (persist.vendor.a6l.radio != 1)"; state disabled; exit 0; }
    state starting
    # Bluetooth is independent of the modem and WLAN. Load it before their
    # potentially long waits so Android can discover a configured HCI device.
    bt_up
    # r5 review fix F38: new startup generation: a 'ready' from an earlier start must not open the Wi-Fi HAL gate
    setprop vendor.a6l.wlan.state starting 2>/dev/null
    # radio2 order: qrtr + daemons (rmtfs, tqftpserv, diag-router) BEFORE the modem stack is loaded/started
    for m in qrtr qrtr-smd; do
        loaded $m || insmod $M/$m.ko || fail "insmod-$m"
    done
    i=0; while [ $i -lt 50 ] && [ ! -e $R/dev/qcom_rmtfs_mem1 ]; do sleep 0.2; i=$((i+1)); done
    [ -e $R/dev/qcom_rmtfs_mem1 ] || fail "no-rmtfs_mem1"
    log "rmtfs_mem1=yes"
    setprop ctl.start vendor.rmtfs
    setprop ctl.start vendor.tqftpserv
    setprop ctl.start vendor.diag_router
    sleep 2
    # the modem needs EFS (rmtfs), its config files (tqftpserv) and DIAG (it crashes on diag starvation)
    svc_wait vendor.rmtfs vendor.tqftpserv vendor.diag_router || fail "daemon-$missing"
    log "rmtfs, tqftpserv, diag-router running"
    # rom-v2: IPA (mobile data) must be up BEFORE the modem stack.
    # merge2 (25 Sep): ipa2b probe proven (walk 0-16, modem handshake OK). RULE: nothing may read IPA registers (a6l_diag,
    # debugfs, register dumps) while the block is runtime-suspended: that reset the phone twice on 25 Sep. The ROM keeps the
    # IPA runtime-active (power/control=on, the configuration that ran without reset) and never reads a6l_diag.
    # merge r4 (28 Sep 2026): ORDER RULE - the IPA driver must be loaded (and bound) BEFORE the modem remoteproc starts:
    # a modem started without the AP IPA asserts as soon as its IMS stack comes up. ipa4 (data3) is proven (27 Sep: data
    # call + ping/DNS/HTTP), so persist.vendor.a6l.ipa defaults to 1 (rom.mk); qcom_q6v5_mss (radio.txt) is only
    # inserted below, after the IPA probe finished, and the modem is started after that.
    if [ "$(getprop persist.vendor.a6l.radio.ipa_less_diag)" = 1 ]; then
        log "ipa: IPA-less DIAGNOSTIC mode (persist.vendor.a6l.radio.ipa_less_diag=1): IPA not loaded, no mobile data; WARNING the modem asserts if IMS comes up without IPA"
    else
        if [ "$(getprop persist.vendor.a6l.ipa)" = 0 ]; then
            log "persist.vendor.a6l.ipa=0 without persist.vendor.a6l.radio.ipa_less_diag=1: IPA is required for the modem"
            fail ipa-disabled
        fi
        load_list ipa || fail "ipa-module"
        i=0; while [ $i -lt $IPA_WAIT ] && { [ ! -e $R/sys/bus/platform/drivers/ipa2-lite/$IPA_DEV ] || [ ! -e $R/sys/class/net/rmnet_ipa0 ]; }; do sleep 0.2; i=$((i+1)); done
        [ -e $R/sys/bus/platform/devices/$IPA_DEV/power/control ] && echo on > $R/sys/bus/platform/devices/$IPA_DEV/power/control
        [ -e $R/sys/bus/platform/drivers/ipa2-lite/$IPA_DEV ] || fail "ipa-not-bound"
        [ -e $R/sys/class/net/rmnet_ipa0 ] || fail "ipa-no-rmnet_ipa0"
        log "ipa: bound + rmnet_ipa0 after ${i}x0.2s pm=$(cat $R/sys/bus/platform/devices/$IPA_DEV/power/control 2>/dev/null)"
    fi
    # modem stack: any failure up to qcom_q6v5_mss is fatal; Wi-Fi module failures after it are reported (degraded)
    load_list radio qcom_q6v5_mss.ko; rc=$?
    [ $rc = 2 ] && fail "modem-module"
    [ $rc = 1 ] && { log "radio: Wi-Fi module(s) failed, modem continues"; wifi_bad=1; }
    r=""; i=0; while [ $i -lt 20 ] && [ -z "$r" ]; do r=$(mss); [ -n "$r" ] || { sleep 1; i=$((i+1)); }; done
    [ -n "$r" ] || fail "no-mss-remoteproc"
    st=$(cat $r/state)
    case "$st" in
    offline) echo start > $r/state || fail "mss-start-write" ;;
    running) log "modem ${r##*/} already running: not started again" ;;
    *) fail "mss-state-$st" ;;       # crashed / recovery in progress / unknown: never a second start
    esac
    i=0; while [ $i -lt 40 ] && [ "$(cat $r/state)" != running ]; do sleep 1; i=$((i+1)); done
    st=$(cat $r/state)
    log "modem ${r##*/} state=$st after ${i}s"
    [ "$st" = running ] || { log "MODEM NOT RUNNING: state=$st"; state "failed:mss-$st"; bt_up; exit 1; }
    state "up${wifi_bad:+:wifi-degraded}"
    # Wi-Fi MAC: stock /persist/wlan_mac.bin "Intf0MacAddress=XXXXXXXXXXXX" (persist is mounted read-only)
    i=0; while [ $i -lt 90 ] && [ ! -e $R/sys/class/net/wlan0 ]; do sleep 1; i=$((i+1)); done
    if [ -e $R/sys/class/net/wlan0 ]; then
        mac=$(grep -a -o 'Intf0MacAddress=[0-9A-Fa-f]\{12\}' $R/mnt/vendor/persist/wlan_mac.bin 2>/dev/null | head -n 1 | cut -d= -f2 | sed 's/\(..\)/\1:/g; s/:$//' | tr 'A-F' 'a-f')
        # r5 review fix F38: never take down a live interface (repeated start while Wi-Fi is up: IFF_UP set), and
        # publish the MAC outcome explicitly: ready (factory MAC set) | ready:mac-failed | ready:no-persist-mac |
        # ready:link-up (untouched). libwifi-hal-a6l accepts any ready* value: the script no longer touches the link.
        fl=$(cat $R/sys/class/net/wlan0/flags 2>/dev/null); case "$fl" in 0x[0-9a-fA-F]*) ;; *) fl=0;; esac
        if [ $((fl & 1)) = 1 ]; then
            log "wlan0 already up (flags=$fl): MAC step skipped, link not disturbed"; wl=ready:link-up
        elif [ -z "$mac" ]; then
            log "wlan0: no factory MAC in persist"; wl=ready:no-persist-mac
        elif ip link set wlan0 down && ip link set wlan0 address "$mac"; then
            log "wlan0 mac $mac (persist)"; wl=ready
        else
            log "wlan0 mac set failed"; wl=ready:mac-failed
        fi
        log "wlan0 present after ${i}s addr=$(cat $R/sys/class/net/wlan0/address)"
        # r5 review fix F5: libwifi-hal-a6l (wifi_wait_for_driver_ready) waits for this before bringing wlan0 up,
        # so the Android Wi-Fi HAL never races the MAC write above (which needs the link down).
        setprop vendor.a6l.wlan.state $wl 2>/dev/null
    else
        log "wlan0 missing after 90 s"
        setprop vendor.a6l.wlan.state missing 2>/dev/null
    fi
    bt_up    # full variant: public BD address for hci0 (a6l_macs, agent hals)
    ;;
stop)
    # r5 review fix F40: the supporting daemons (rmtfs/EFS, tqftpserv, diag-router: the modem crashes on DIAG
    # starvation) are only stopped once the MSS remoteproc is confirmed offline (or absent). A rejected stop write, a
    # modem that stays up, or a crashed/recovering remoteproc => state failed:stop-<why>, daemons kept, exit 1.
    r=$(mss)
    if [ -n "$r" ]; then
        st=$(cat $r/state 2>/dev/null)
        case "$st" in
        offline) log "modem ${r##*/} already offline" ;;
        running)
            state stopping
            if ! echo stop > $r/state; then
                log "modem ${r##*/} stop write rejected: daemons kept"; state "failed:stop-write"; exit 1
            fi
            i=0; while [ $i -lt 20 ] && [ "$(cat $r/state 2>/dev/null)" != offline ]; do sleep 0.5; i=$((i+1)); done
            st=$(cat $r/state 2>/dev/null)
            [ "$st" = offline ] || { log "modem ${r##*/} state=$st after stop: daemons kept"; state "failed:stop-$st"; exit 1; }
            ;;
        *) log "modem ${r##*/} state=$st: not stopping (daemons kept)"; state "failed:stop-${st:-unknown}"; exit 1 ;;
        esac
    else
        log "no MSS remoteproc: modem not loaded"
    fi
    state stopped
    setprop vendor.a6l.wlan.state stopped 2>/dev/null
    for s in vendor.diag_router vendor.tqftpserv vendor.rmtfs; do setprop ctl.stop $s; done
    log "stopped"
    ;;
esac
exit 0
