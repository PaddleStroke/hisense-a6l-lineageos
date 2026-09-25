#!/system/bin/sh
# A6L V75-usb: USB re-attach watchdog for the RAM diagnostic recovery.
# RAM-only. Touches nothing but USB sysfs/configfs attributes of the phone's own
# USB controller (soft_connect, gadget UDC, dwc3 / dwc3-qcom driver bind), and
# runtime-PM "control" of those devices. No storage, no rails, no RF.
#
# Why: the V71/V74 recovery sometimes boots with the UDC in "not attached"
# forever (the laptop sees no device at all, not even a failed enumeration).
# a6l_android_probe binds the gadget once (t=4 s) and soft-connects once (t=8 s)
# and never retries. This service watches /sys/class/udc/<udc>/state and escalates:
#   L1 soft_connect disconnect/connect  (udc_stop/udc_start + dwc3 core soft reset + pullup)
#   L2 gadget UDC unbind/rebind         (+ connect, a6l_manual_usb re-arms the hold at bind)
#   L3 dwc3 core driver unbind/bind     (phy_exit/phy_init: QUSB2 PHY BCR reset + PLL relock)
#   L4 dwc3-qcom glue unbind/bind       (USB30 BCR reset, clocks, QSCRATCH VBUS override)
# then starts again at L1 with a longer pause. Everything is logged to /dev/kmsg
# with the prefix A6L_USBWD (visible on the LCD console and in dmesg).
#
# Test hooks (offline mock test): A6L_USBWD_ROOT prefixes every path,
# A6L_USBWD_GETPROP replaces getprop, A6L_USBWD_FAST=1 shortens the timings,
# A6L_USBWD_MAX_S stops the loop after that many seconds.

T=${A6L_USBWD_TOYBOX:-/system/bin/toybox}
P=${A6L_USBWD_ROOT:-}
KMSG=$P/dev/kmsg
GETPROP=${A6L_USBWD_GETPROP:-/system/bin/getprop}
G=$P/sys/kernel/config/usb_gadget/a6lprobe
UDCCLASS=$P/sys/class/udc
DWC3_DRV=$P/sys/bus/platform/drivers/dwc3
MAX_S=${A6L_USBWD_MAX_S:-0}

if [ "${A6L_USBWD_FAST:-0}" = 1 ]; then
    GRACE=3; POLL=1; STEP=3; STEP_ATTACHED=6; SETTLE=1; CYCLE_PAUSE=5; WAIT_UDC=5
else
    # probe: config t=0, bind t=4, connect t=8 (after its own start). Host needs
    # ~1 s to enumerate. Give the probe's own attempt 25 s before touching it.
    GRACE=25; POLL=2; STEP=12; STEP_ATTACHED=30; SETTLE=2; CYCLE_PAUSE=60; WAIT_UDC=15
fi

log() {
    echo "A6L_USBWD $*" >> "$KMSG" 2>/dev/null
}

rd() { # rd <file> -> first line, or "-"
    v=-
    [ -r "$1" ] && read -r v < "$1" 2>/dev/null
    [ -n "$v" ] || v=-
    echo "$v"
}

wr() { # wr <file> <value> -> 0/1, logged
    if echo "$2" > "$1" 2>/dev/null; then
        return 0
    fi
    log "write_fail path=${1#$P} value=$2"
    return 1
}

find_udc() {
    for u in "$UDCCLASS"/*; do
        [ -e "$u" ] || continue
        echo "${u##*/}"
        return 0
    done
    echo ""
}

# sysfs device dirs: resolved once the UDC exists (udc name = dwc3 core device name)
DWC3_DEV=""; GLUE_DEV=""; GLUE_DRV=""
resolve_devs() {
    u=$1
    [ -n "$u" ] || return 1
    d=$P/sys/bus/platform/devices/$u
    [ -e "$d" ] || return 1
    DWC3_DEV=$u
    real=$($T readlink -f "$d" 2>/dev/null)
    [ -n "$real" ] || return 1
    parent=${real%/*}
    GLUE_DEV=${parent##*/}
    if [ -e "$parent/driver" ]; then
        gd=$($T readlink -f "$parent/driver" 2>/dev/null)
        GLUE_DRV=${gd##*/}
    fi
    return 0
}

pm_on() { # keep controller, glue and PHY out of runtime suspend
    for dev in "$@"; do
        [ -n "$dev" ] || continue
        f=$P/sys/bus/platform/devices/$dev/power/control
        [ -e "$f" ] && echo on > "$f" 2>/dev/null
    done
}

snapshot() { # one-shot diagnostics for the bad case (read-only)
    log "snap udc=$UDC state=$(rd $UDCCLASS/$UDC/state) speed=$(rd $UDCCLASS/$UDC/current_speed) gadget_udc=$(rd $G/UDC) ffs_ready=$($GETPROP sys.usb.ffs.ready 2>/dev/null) adbd=$($GETPROP init.svc.adbd 2>/dev/null)"
    log "snap dwc3=$DWC3_DEV rpm=$(rd $P/sys/bus/platform/devices/$DWC3_DEV/power/runtime_status) glue=$GLUE_DEV drv=$GLUE_DRV rpm=$(rd $P/sys/bus/platform/devices/$GLUE_DEV/power/runtime_status)"
    for phy in $P/sys/bus/platform/devices/*.phy; do
        [ -e "$phy" ] || continue
        log "snap phy=${phy##*/} drv=$(if [ -e $phy/driver ]; then r=$($T readlink -f $phy/driver); echo ${r##*/}; else echo none; fi) rpm=$(rd $phy/power/runtime_status)"
    done
    cs=$P/sys/kernel/debug/clk/clk_summary
    if [ -r "$cs" ]; then
        $T grep -E "ln_bb|clkref|usb|xo_board|cxo" "$cs" 2>/dev/null | while read -r line; do
            log "clk $line"
        done
    fi
    rs=$P/sys/kernel/debug/regulator/regulator_summary
    if [ -r "$rs" ]; then
        $T grep -E "qusb|usb|l1b|l10a|l7b|ldo1|ldo10" "$rs" 2>/dev/null | while read -r line; do
            log "reg $line"
        done
    fi
    ex=$P/sys/class/extcon
    for e in "$ex"/*; do
        [ -e "$e/state" ] || continue
        log "extcon ${e##*/} $(rd $e/state)"
    done
}

wait_udc() { # wait until /sys/class/udc/<name> exists again
    n=0
    while [ $n -lt $WAIT_UDC ]; do
        [ -e "$UDCCLASS/$1" ] && return 0
        $T sleep 1; n=$((n + 1))
    done
    return 1
}

bind_gadget() { # bind gadget to UDC (retry while ffs descriptors are rewritten) + connect
    n=0
    while [ $n -lt $WAIT_UDC ]; do
        cur=$(rd $G/UDC)
        [ "$cur" = "$UDC" ] && break
        echo "$UDC" > "$G/UDC" 2>/dev/null && break
        $T sleep 1; n=$((n + 1))
    done
    cur=$(rd $G/UDC)
    log "gadget_bind udc=$UDC result=$cur tries=$n"
    $T sleep 1
    wr "$UDCCLASS/$UDC/soft_connect" connect && log "soft_connect connect ok"
}

level1() {
    log "L1 soft_connect toggle udc=$UDC"
    wr "$UDCCLASS/$UDC/soft_connect" disconnect
    $T sleep $SETTLE
    wr "$UDCCLASS/$UDC/soft_connect" connect
}

level2() {
    log "L2 gadget unbind/rebind udc=$UDC"
    wr "$G/UDC" ""
    $T sleep $SETTLE
    bind_gadget
}

level3() {
    [ -n "$DWC3_DEV" ] || { log "L3 skipped: dwc3 device unknown"; return; }
    log "L3 dwc3 core unbind/bind dev=$DWC3_DEV"
    wr "$G/UDC" ""
    wr "$DWC3_DRV/unbind" "$DWC3_DEV"
    $T sleep $SETTLE
    wr "$DWC3_DRV/bind" "$DWC3_DEV"
    if wait_udc "$UDC"; then
        pm_on "$DWC3_DEV"
        bind_gadget
    else
        log "L3 udc did not come back"
    fi
}

level4() {
    [ -n "$GLUE_DEV" ] && [ -n "$GLUE_DRV" ] || { log "L4 skipped: glue unknown"; return; }
    log "L4 glue unbind/bind dev=$GLUE_DEV drv=$GLUE_DRV"
    wr "$G/UDC" ""
    wr "$P/sys/bus/platform/drivers/$GLUE_DRV/unbind" "$GLUE_DEV"
    $T sleep $SETTLE
    wr "$P/sys/bus/platform/drivers/$GLUE_DRV/bind" "$GLUE_DEV"
    if wait_udc "$UDC"; then
        pm_on "$GLUE_DEV" "$DWC3_DEV"
        bind_gadget
    else
        log "L4 udc did not come back"
    fi
}

# ---------------------------------------------------------------- main
log "START v75-usb grace=$GRACE poll=$POLL step=$STEP"
$T sleep $GRACE
UDC=$(find_udc)
if [ -z "$UDC" ]; then
    # no UDC at all: dwc3 probe failed or deferred. Try binding the core by the
    # DT name once; the loop below keeps retrying.
    log "no UDC after grace; trying dwc3 bind a800000.usb"
    wr "$DWC3_DRV/bind" a800000.usb
    wait_udc a800000.usb
    UDC=$(find_udc)
fi
resolve_devs "$UDC"
pm_on "$GLUE_DEV" "$DWC3_DEV"
for phy in $P/sys/bus/platform/devices/*.phy; do
    [ -e "$phy/power/control" ] && echo on > "$phy/power/control" 2>/dev/null
done
log "udc=$UDC dwc3=$DWC3_DEV glue=$GLUE_DEV glue_drv=$GLUE_DRV"

bad=0          # seconds in a bad state since the last action
level=0        # last escalation level used (0 = none yet)
ever=0         # 1 once state reached configured
snapped=0
last=""
start=$SECONDS
while :; do
    now=$((SECONDS - start))
    if [ "$MAX_S" -gt 0 ] && [ $now -ge "$MAX_S" ]; then
        log "STOP max_s=$MAX_S level=$level ever=$ever"
        exit 0
    fi
    [ -n "$UDC" ] || { UDC=$(find_udc); resolve_devs "$UDC"; }
    st=$(rd "$UDCCLASS/$UDC/state")
    if [ "$st" != "$last" ]; then
        log "state=$st t=$now level=$level"
        last=$st
    fi
    case "$st" in
    configured|suspended)
        if [ $ever = 0 ]; then
            log "CONFIGURED t=$now after_level=$level"
            ever=1
        fi
        bad=0; level=0
        ;;
    *)
        bad=$((bad + POLL))
        lim=$STEP
        # attached but stuck mid-enumeration: give the host more time
        case "$st" in default|addressed|powered) lim=$STEP_ATTACHED;; esac
        if [ $bad -ge $lim ]; then
            if [ $snapped = 0 ]; then snapshot; snapped=1; fi
            level=$((level + 1))
            case $level in
            1) level1 ;;
            2) level1 ;;
            3) level2 ;;
            4) level3 ;;
            5) level4 ;;
            *) log "cycle done; pause ${CYCLE_PAUSE}s"; $T sleep $CYCLE_PAUSE
               level=1; snapped=0; level1 ;;
            esac
            bad=0
        fi
        ;;
    esac
    $T sleep $POLL
done
