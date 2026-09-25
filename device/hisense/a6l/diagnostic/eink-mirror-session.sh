#!/system/bin/sh
# eink-mirror-session.sh — ATTENDED ONLY. Milestone M1: the rear e-ink mirrors the LineageOS front screen in the
# LineageOS-from-RAM session (V71 recovery + framework-phone-v71.sh). Runs in the RECOVERY shell (adb shell), i.e.
# OUTSIDE the framework's private mount namespace, where /sys is writable and the panel rails/XON can be switched.
# Files expected in /tmp/epd (as for the r149/r155 e-ink runs): lib64/libtcon_eink.so, epd-nor.bin, plus
# a6l_epdd_v3, a6l_eink_mirror (this milestone). Needs the bundle-r16 modules loaded (V73 panel driver, bridge_ok).
#
#   sh eink-mirror-session.sh pre     BEFORE starting framework-phone-v71.sh: save the e-ink mode, force the e-ink
#                                     connector "off" so drm_hwcomposer does not attach it as a 2nd display
#   sh eink-mirror-session.sh start   AFTER the Lineage UI is up: a6l_epdd_v3 (DRM lease from the composer) + mirror
#   sh eink-mirror-session.sh status  logs summary;   ... clear | refresh   manual commands through the FIFO
#   sh eink-mirror-session.sh stop    stop mirror + epdd;   ... restore   (after the framework ended) connector "detect"
# Environment: MIRROR_ARGS (extra a6l_eink_mirror options, e.g. "--source drm" or "--clear-every 5"), EPDD_ARGS.
export PATH=/tmp/bin:/system/bin:$PATH
E=${E:-/tmp/epd}
MODE=$E/eink-mode.bin
LOGE=$E/epdd-mirror.log
LOGM=$E/mirror.log
FIFO=$E/cmd

eink_conn() {	# sysfs dir of the connector whose mode list contains 384x725
    for c in ${SYSDRM:-/sys/class/drm}/card*-*; do
        [ -f "$c/modes" ] || continue
        case "$(cat $c/modes 2>/dev/null)" in *384x725*) echo "$c"; return 0;; esac
    done
    # after "off" the modes list is empty: fall back to the name recorded by "pre"
    [ -f $E/eink-conn.txt ] && cat $E/eink-conn.txt && return 0
    return 1
}
mk_nodes() {	# recovery has no /dev/dri or /dev/gpiochip*: create them from sysfs (as the bundle scripts do)
    mkdir -p /dev/dri
    for d in /sys/class/drm/card[0-9] /sys/class/drm/renderD*; do
        [ -f $d/dev ] || continue; n=/dev/dri/${d##*/}; [ -e $n ] && continue
        mm=$(cat $d/dev); mknod $n c ${mm%%:*} ${mm##*:} && echo "node $n $mm"
    done
    for d in /sys/bus/gpio/devices/gpiochip*; do
        [ -f $d/dev ] || continue; n=/dev/${d##*/}; [ -e $n ] && continue
        mm=$(cat $d/dev); mknod $n c ${mm%%:*} ${mm##*:} && echo "node $n $mm"
    done
}
pid_of() { for p in /proc/[0-9]*; do [ "$(cat $p/comm 2>/dev/null)" = "$1" ] && { echo ${p##*/}; return 0; }; done; return 1; }

case "${1:-}" in
pre)
    [ "$(id -u)" = 0 ] || { echo MIRROR_FAIL root; exit 2; }
    for f in a6l_epdd_v3 a6l_eink_mirror epd-nor.bin lib64/libtcon_eink.so; do [ -e $E/$f ] || { echo "MIRROR_FAIL missing $E/$f"; exit 3; }; done
    chmod 755 $E/a6l_epdd_v3 $E/a6l_eink_mirror
    pid_of surfaceflinger >/dev/null && { echo "MIRROR_FAIL surfaceflinger already running: run 'pre' before the framework"; exit 4; }
    mk_nodes
    c=$(eink_conn) || { echo "MIRROR_FAIL no connector with a 384x725 mode (panel module loaded? bridge_ok?)"; exit 5; }
    echo "$c" > $E/eink-conn.txt
    echo "e-ink connector: $c status=$(cat $c/status) enabled=$(cat $c/enabled)"
    for s in /sys/bus/mipi-dsi/devices/*/bringup_status; do [ -f $s ] && echo "bringup: $(cat $s)"; done
    $E/a6l_epdd_v3 --save-mode $MODE || { echo MIRROR_FAIL save-mode; exit 6; }
    echo off > $c/status || { echo MIRROR_FAIL cannot force connector off; exit 7; }
    sleep 1
    st=$(cat $c/status); echo "after force-off: status=$st enabled=$(cat $c/enabled)"
    [ "$st" = disconnected ] || { echo "MIRROR_FAIL connector still $st"; exit 8; }
    echo MIRROR_PRE_PASS
    ;;
start)
    [ -f $MODE ] || { echo "MIRROR_FAIL run 'pre' first"; exit 2; }
    i=0; until pid_of surfaceflinger >/dev/null; do
        i=$((i+1)); [ $i -gt 180 ] && { echo "MIRROR_FAIL surfaceflinger not running"; exit 3; }; sleep 1; done
    sleep ${START_DELAY:-10}	# let the composer finish its first commits (it must be DRM master before we ask it for a lease)
    echo "surfaceflinger pid $(pid_of surfaceflinger)"
    for p in /proc/[0-9]*; do for f in $p/fd/*; do case "$(readlink $f 2>/dev/null)" in /dev/dri/card*) echo "DRM user: pid ${p##*/} $(cat $p/comm) $f -> $(readlink $f)";; esac; done; done 2>/dev/null
    rm -f $FIFO; : > $LOGE; : > $LOGM
    ( cd $E; LD_LIBRARY_PATH=$E/lib64 setsid $E/a6l_epdd_v3 --waveform $E/epd-nor.bin --lib $E/lib64/libtcon_eink.so \
        --lease auto --mode-file $MODE --fifo $FIFO ${EPDD_ARGS:-} >> $LOGE 2>&1 < /dev/null & )
    i=0; until grep -q -e "serving $FIFO" -e FAIL $LOGE; do i=$((i+1)); [ $i -gt 60 ] && break; sleep 1; done
    grep -E "lease|DRM|bring-up|update|FAIL|WARN|serving" $LOGE
    grep -q "serving $FIFO" $LOGE || { echo "MIRROR_FAIL a6l_epdd_v3 did not start (see $LOGE)"; exit 4; }
    ( setsid $E/a6l_eink_mirror --fifo $FIFO --epdd-log $LOGE --out $E/mirror ${MIRROR_ARGS:-} >> $LOGM 2>&1 < /dev/null & )
    sleep 5; cat $LOGM
    echo MIRROR_STARTED
    ;;
status)
    echo "== epdd"; grep -E "lease|bring-up|shown in|FAIL|WARN" $LOGE | tail -n 15
    echo "== mirror"; grep -v "frame [0-9]*:" $LOGM | tail -n 20
    echo "updates: $(grep -c 'shown in' $LOGE) ok=$(grep 'shown in' $LOGE | grep -c ': ok') missed-vblank-lines=$(grep 'shown in' $LOGE | grep -vc 'drive=0')"
    ;;
clear|refresh) echo "$1" > $FIFO && echo "sent $1" ;;
stop)
    p=$(pid_of a6l_eink_mirror) && kill $p
    sleep 1; [ -p $FIFO ] && echo quit > $FIFO
    sleep 3; p=$(pid_of a6l_epdd_v3) && kill $p
    tail -n 3 $LOGE; echo MIRROR_STOPPED
    ;;
restore)	# only after the framework session has ended (the composer would attach the e-ink as a display)
    pid_of surfaceflinger >/dev/null && { echo "MIRROR_FAIL framework still running"; exit 2; }
    c=$(eink_conn) && echo detect > $c/status && sleep 1 && echo "status=$(cat $c/status)"
    ;;
*) sed -n 2,16p "$0"; exit 1 ;;
esac
