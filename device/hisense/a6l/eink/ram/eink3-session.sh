#!/system/bin/sh
# eink3-session.sh — ATTENDED ONLY. The eink3 stack (a6l_epdd v4 + a6l_eink_mirror v2) in the LineageOS-from-RAM
# session (V74 recovery + framework payload). Runs in the RECOVERY shell (outside the framework namespace), like the M1
# eink-mirror-session.sh it replaces. The RAM payload's composer is NOT patched, so this keeps M1's two tricks:
# connector forced "off" before the framework ("pre") and a lease taken from the composer with pidfd_getfd ("start").
# The ROM path (patched composer lease server, --no-master) is tested in the installed ROM (docs/eink3-20260924.md §7).
# Files in /tmp/epd: a6l_epdd (v4), a6l_eink_mirror (v2), epd-nor.bin, lib64/libtcon_eink.so (+ its libs as for r155).
#
#   sh eink3-session.sh pre                 BEFORE the framework: save mode, force the e-ink connector off
#   sh eink3-session.sh start [screencap|drm]   AFTER the UI is up: epdd (lease) + mirror in MIRROR mode, key + touch live
#   sh eink3-session.sh status | cmd "<epdd command>" | stop | restore
# Environment: MIRROR_ARGS, EPDD_ARGS (extra options).
export PATH=/tmp/bin:/system/bin:$PATH
E=${E:-/tmp/epd}
MODE=$E/eink-mode.bin; LOGE=$E/epdd-eink3.log; LOGM=$E/mirror-eink3.log; SOCK=$E/sock
eink_conn() {
    for c in /sys/class/drm/card*-*; do [ -f "$c/modes" ] || continue
        case "$(cat $c/modes 2>/dev/null)" in *384x725*) echo "$c"; return 0;; esac; done
    [ -f $E/eink-conn.txt ] && cat $E/eink-conn.txt && return 0; return 1
}
mk_nodes() {
    mkdir -p /dev/dri /dev/input
    for d in /sys/class/drm/card[0-9] /sys/class/drm/renderD* /sys/bus/gpio/devices/gpiochip* /sys/class/input/event* /sys/class/misc/uinput; do
        [ -f $d/dev ] || continue; case $d in */drm/*) n=/dev/dri/${d##*/};; */input/*) n=/dev/input/${d##*/};; */misc/*) n=/dev/${d##*/};; *) n=/dev/${d##*/};; esac
        [ -e $n ] && continue; mm=$(cat $d/dev); mknod $n c ${mm%%:*} ${mm##*:} && echo "node $n $mm"; done
}
pid_of() { for p in /proc/[0-9]*; do [ "$(cat $p/comm 2>/dev/null)" = "$1" ] && { echo ${p##*/}; return 0; }; done; return 1; }
send() { $E/a6l_eink_mirror --epd-socket $SOCK --send "$1"; }	# one command to epdd, prints its reply line
case "${1:-}" in
pre)
    [ "$(id -u)" = 0 ] || { echo EINK3_FAIL root; exit 2; }
    for f in a6l_epdd a6l_eink_mirror epd-nor.bin lib64/libtcon_eink.so; do [ -e $E/$f ] || { echo "EINK3_FAIL missing $E/$f"; exit 3; }; done
    chmod 755 $E/a6l_epdd $E/a6l_eink_mirror
    pid_of surfaceflinger >/dev/null && { echo "EINK3_FAIL surfaceflinger already running: run 'pre' before the framework"; exit 4; }
    mk_nodes
    c=$(eink_conn) || { echo "EINK3_FAIL no connector with a 384x725 mode (panel module loaded? bridge_ok?)"; exit 5; }
    echo "$c" > $E/eink-conn.txt
    echo "e-ink connector: $c status=$(cat $c/status) id=$(cat $c/connector_id 2>/dev/null || echo '?')"
    LD_LIBRARY_PATH=$E/lib64 $E/a6l_epdd --save-mode $MODE || { echo EINK3_FAIL save-mode; exit 6; }
    echo off > $c/status || { echo EINK3_FAIL cannot force connector off; exit 7; }
    sleep 1; st=$(cat $c/status); echo "after force-off: status=$st"
    [ "$st" = disconnected ] || { echo "EINK3_FAIL connector still $st"; exit 8; }
    echo EINK3_PRE_PASS ;;
start)
    SRC=${2:-screencap}
    [ -f $MODE ] || { echo "EINK3_FAIL run 'pre' first"; exit 2; }
    i=0; until pid_of surfaceflinger >/dev/null; do i=$((i+1)); [ $i -gt 180 ] && { echo "EINK3_FAIL surfaceflinger not running"; exit 3; }; sleep 1; done
    sleep ${START_DELAY:-10}; mk_nodes
    rm -f $SOCK; : > $LOGE; : > $LOGM
    ( cd $E; LD_LIBRARY_PATH=$E/lib64 setsid $E/a6l_epdd --waveform $E/epd-nor.bin --lib $E/lib64/libtcon_eink.so --xon-line -1 \
        --lease auto --mode-file $MODE --listen $SOCK --idle-off ${IDLE_OFF:-0} ${EPDD_ARGS:-} >> $LOGE 2>&1 < /dev/null & )
    i=0; until grep -q -e "serving" -e "FAIL" $LOGE; do i=$((i+1)); [ $i -gt 60 ] && break; sleep 1; done
    grep -E "waveform|lease|DRM|bring-up|update|FAIL|WARN|serving" $LOGE
    grep -q "serving" $LOGE || { echo "EINK3_FAIL a6l_epdd did not start (see $LOGE)"; exit 4; }
    ( setsid $E/a6l_eink_mirror --source $SRC --epd-socket $SOCK --no-props --mode mirror --touch-debug ${MIRROR_ARGS:-} >> $LOGM 2>&1 < /dev/null & )
    sleep 6; grep -v TOUCH_OUT $LOGM | head -30
    echo EINK3_STARTED ;;
status)
    echo "== epdd"; grep -E "lease|bring-up|shown in|FAIL|WARN|CRTC" $LOGE | tail -n 15
    echo "== mirror"; grep -v -e "TOUCH_OUT" -e "frame [0-9]*:" $LOGM | tail -n 25
    echo "touch events forwarded: $(grep -c 'TOUCH_OUT 3 57 [0-9]' $LOGM) contacts"
    echo "updates: $(grep -c 'shown in' $LOGE) ok=$(grep 'shown in' $LOGE | grep -c ': ok') missed-vblank-lines=$(grep 'shown in' $LOGE | grep -vc 'drive=0')" ;;
cmd) send "$2" ;;
stop)
    p=$(pid_of a6l_eink_mirror) && kill $p; sleep 1
    send quit; sleep 3; p=$(pid_of a6l_epdd) && kill $p
    tail -n 3 $LOGE; echo EINK3_STOPPED ;;
restore)
    pid_of surfaceflinger >/dev/null && { echo "EINK3_FAIL framework still running"; exit 2; }
    c=$(eink_conn) && echo detect > $c/status && sleep 1 && echo "status=$(cat $c/status)" ;;
*) sed -n 2,14p "$0"; exit 1 ;;
esac
