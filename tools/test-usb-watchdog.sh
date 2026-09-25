#!/usr/bin/env bash
# Offline mock test of a6l_usb_watchdog.sh: a fake sysfs/configfs tree plus a
# "fake kernel" loop that reacts to the watchdog's writes like dwc3/udc-core do.
# usage: test-usb-watchdog.sh <watchdog.sh> [shell-cmd-to-run-it] [toybox-wrapper]
# scenarios: good l1 l2 l3 l4 never noudc
set -u
WD=$(readlink -f "$1"); SHELLCMD=${2:-sh}; TB=${3:-}
HERE=$(mktemp -d)
if [ -z "$TB" ]; then
    TB=$HERE/toybox
    printf '#!/bin/sh\nexec "$@"\n' > "$TB"; chmod +x "$TB"
fi
pass=0; fail=0
UDC=a800000.usb; GLUE=a8f8800.usb; GDRV=dwc3-qcom-legacy

mkroot() {
    R=$1
    mkdir -p $R/dev $R/sys/kernel/config/usb_gadget/a6lprobe $R/sys/class/udc \
        $R/sys/devices/platform/soc@0/$GLUE/$UDC/power $R/sys/devices/platform/soc@0/$GLUE/power \
        $R/sys/devices/platform/soc@0/c012000.phy/power \
        $R/sys/bus/platform/devices $R/sys/bus/platform/drivers/dwc3 $R/sys/bus/platform/drivers/$GDRV \
        $R/sys/kernel/debug/clk
    : > $R/dev/kmsg
    ln -s ../../../devices/platform/soc@0/$GLUE/$UDC $R/sys/bus/platform/devices/$UDC
    ln -s ../../../devices/platform/soc@0/$GLUE $R/sys/bus/platform/devices/$GLUE
    ln -s ../../../devices/platform/soc@0/c012000.phy $R/sys/bus/platform/devices/c012000.phy
    ln -s ../../../../bus/platform/drivers/$GDRV $R/sys/devices/platform/soc@0/$GLUE/driver
    for d in $GLUE/$UDC $GLUE c012000.phy; do echo auto > $R/sys/devices/platform/soc@0/$d/power/control; echo active > $R/sys/devices/platform/soc@0/$d/power/runtime_status; done
    : > $R/sys/bus/platform/drivers/dwc3/bind; : > $R/sys/bus/platform/drivers/dwc3/unbind
    : > $R/sys/bus/platform/drivers/$GDRV/bind; : > $R/sys/bus/platform/drivers/$GDRV/unbind
    printf '   ln_bb_clk1   0 0 0 19200000\n   gcc_rx0_usb2_clkref_clk 1 1 0 19200000\n' > $R/sys/kernel/debug/clk/clk_summary
    echo $UDC > $R/sys/kernel/config/usb_gadget/a6lprobe/UDC
}
mkudc() {
    R=$1
    mkdir -p $R/sys/class/udc/$UDC
    echo "not attached" > $R/sys/class/udc/$UDC/state
    echo UNKNOWN > $R/sys/class/udc/$UDC/current_speed
    : > $R/sys/class/udc/$UDC/soft_connect
}

# fake kernel: fixes state only once the required level ran.
kernel() {
    R=$1; need=$2   # need: good|l1|l2|l3|l4|never
    ok=0; [ $need = good ] && ok=1
    [ $need = l1 ] && ok=1        # a connect toggle alone fixes it
    s=$R/sys/class/udc/$UDC
    while [ -e $R/.run ]; do
        if [ -d $s ]; then
            sc=$(cat $s/soft_connect 2>/dev/null)
            if [ -n "$sc" ]; then
                : > $s/soft_connect
                echo "$sc" >> $R/.events
                case "$sc" in
                disconnect) echo "not attached" > $s/state ;;
                connect) [ $ok = 1 ] && [ $need != good ] && [ -e $R/.toggled ] && echo configured > $s/state ;;
                esac
                [ "$sc" = disconnect ] && touch $R/.toggled
            fi
        fi
        g=$(cat $R/sys/kernel/config/usb_gadget/a6lprobe/UDC 2>/dev/null)
        if [ -z "$g" ] && [ ! -e $R/.gunbound ]; then touch $R/.gunbound; echo gadget-unbind >> $R/.events; [ $need = l2 ] && ok=1; fi
        [ -n "$g" ] && rm -f $R/.gunbound
        u=$(cat $R/sys/bus/platform/drivers/dwc3/unbind); if [ -n "$u" ]; then : > $R/sys/bus/platform/drivers/dwc3/unbind; echo "dwc3-unbind $u" >> $R/.events; rm -rf $s; : > $R/sys/kernel/config/usb_gadget/a6lprobe/UDC; fi
        b=$(cat $R/sys/bus/platform/drivers/dwc3/bind); if [ -n "$b" ]; then : > $R/sys/bus/platform/drivers/dwc3/bind; echo "dwc3-bind $b" >> $R/.events; sleep 0.5; mkudc $R; [ $need = l3 ] || [ $need = noudc ] && ok=1; touch $R/.toggled; fi
        u=$(cat $R/sys/bus/platform/drivers/$GDRV/unbind); if [ -n "$u" ]; then : > $R/sys/bus/platform/drivers/$GDRV/unbind; echo "glue-unbind $u" >> $R/.events; rm -rf $s; : > $R/sys/kernel/config/usb_gadget/a6lprobe/UDC; fi
        b=$(cat $R/sys/bus/platform/drivers/$GDRV/bind); if [ -n "$b" ]; then : > $R/sys/bus/platform/drivers/$GDRV/bind; echo "glue-bind $b" >> $R/.events; sleep 0.5; mkudc $R; [ $need = l4 ] && ok=1; touch $R/.toggled; fi
        sleep 0.1
    done
}

run() {
    name=$1; need=$2; maxs=$3; expect=$4; forbid=${5:-}
    R=$HERE/$name; mkroot $R
    [ $need = noudc ] || mkudc $R
    [ $need = good ] && echo configured > $R/sys/class/udc/$UDC/state
    [ $need = noudc ] && : > $R/sys/kernel/config/usb_gadget/a6lprobe/UDC
    touch $R/.run; : > $R/.events
    kernel $R $need & kp=$!
    printf '#!/bin/sh\necho 1\n' > $R/getprop; chmod +x $R/getprop
    A6L_USBWD_ROOT=$R A6L_USBWD_FAST=1 A6L_USBWD_MAX_S=$maxs A6L_USBWD_TOYBOX=$TB A6L_USBWD_GETPROP=$R/getprop \
        $SHELLCMD $WD > $R/stdout 2>&1
    rc=$?
    rm -f $R/.run; wait $kp 2>/dev/null
    log=$(cat $R/dev/kmsg)
    ok=1
    for e in $expect; do echo "$log" | grep -q -- "$e" || { ok=0; echo "  missing: $e"; }; done
    for f in $forbid; do echo "$log" | grep -q -- "$f" && { ok=0; echo "  unexpected: $f"; }; done
    [ $rc = 0 ] || { ok=0; echo "  rc=$rc"; cat $R/stdout; }
    [ -s $R/stdout ] && { echo "  stdout/stderr:"; sed 's/^/    /' $R/stdout | head -20; }
    if [ $ok = 1 ]; then pass=$((pass+1)); echo "PASS $name"; else fail=$((fail+1)); echo "FAIL $name"; sed 's/^/    kmsg: /' $R/dev/kmsg | head -60; sed 's/^/    ev: /' $R/.events; fi
    echo "  pm: $(cat $R/sys/devices/platform/soc@0/$GLUE/$UDC/power/control) $(cat $R/sys/devices/platform/soc@0/$GLUE/power/control) $(cat $R/sys/devices/platform/soc@0/c012000.phy/power/control)"
}

run good  good  10 "START CONFIGURED.*after_level=0" "L1 L2 L3 L4 snap"
run l1    l1    15 "L1 soft_connect CONFIGURED.*after_level=1 clk.*ln_bb_clk1" "L2 L3 L4"
run l2    l2    30 "L1 L2.gadget gadget_bind.*result=a800000.usb CONFIGURED.*after_level=3" "L3 L4"
run l3    l3    40 "L3.dwc3 gadget_bind CONFIGURED.*after_level=4 glue=a8f8800.usb" "L4"
run l4    l4    50 "L4.glue.*drv=dwc3-qcom-legacy CONFIGURED.*after_level=5"
run never never 60 "L4 cycle.done STOP"
run noudc noudc 20 "no.UDC CONFIGURED"
echo "RESULT pass=$pass fail=$fail"
[ $fail = 0 ]
