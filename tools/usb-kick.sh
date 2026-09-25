#!/usr/bin/env bash
# usb-kick.sh — laptop-side helper when the A6L recovery USB shows up as "nothing at all".
# Runs on the laptop (system76-pc) in ~/A6L-usb-20260915. NEVER runs adb, never reboots anything.
#
#   tools/usb-kick.sh status            read-only: where is the phone, what did the kernel log (default)
#   tools/usb-kick.sh wait [SEC]        read-only: wait up to SEC s (default 60) for the recovery (1d6b:0104)
#   tools/usb-kick.sh reset             usbreset of the phone's device on PORT (only if something is enumerated)
#   tools/usb-kick.sh port-cycle        logical disable/enable of the hub port (sudo; the hub re-detects the line)
#   tools/usb-kick.sh uhubctl           VBUS power-cycle with uhubctl, if installed AND the port supports power switching
#   tools/usb-kick.sh kick [SEC]        wait; if nothing, port-cycle, wait again, then uhubctl if available
#   tools/usb-kick.sh xhci-rebind --force   rebind the whole xHCI controller (drops every device on it; last resort)
#
# PORT defaults to the established phone port 3-2 (root hub bus 3, port 2); override with PORT=x-y.
# Honest limits: in the "not attached" failure the PHONE never pulls D+ up, so a host-side reset only helps if it
# really removes VBUS (uhubctl on a hub with per-port power switching, or unplugging the cable). The logical
# port-cycle is harmless and cheap, so it is tried first. On this laptop the ports sit on AMD xHCI root hubs,
# which usually do NOT switch VBUS; an external USB2 hub with per-port power switching (uhubctl-compatible) makes
# the uhubctl mode real.
set -u
PORT=${PORT:-3-2}
BUS=${PORT%%-*}; PNUM=${PORT##*-}
HUBIF=/sys/bus/usb/devices/$BUS-0:1.0
[ "${PORT#*.}" != "$PORT" ] && HUBIF=/sys/bus/usb/devices/${PORT%.*}:1.0   # port behind an external hub, e.g. 3-2.4
PORTDIR=$HUBIF/usb$BUS-port$PNUM
[ "${PORT#*.}" != "$PORT" ] && PORTDIR=$HUBIF/${PORT%.*}-port${PORT##*.}
KNOWN='109b:911f 109b:f001 18d1:d00d 18d1:4ee7 1d6b:0104 05c6:9008'

say() { echo "[usb-kick $(date +%T)] $*"; }

dev_on_port() { # prints "vid:pid product" of what is enumerated on PORT
    d=/sys/bus/usb/devices/$PORT
    [ -e "$d/idVendor" ] || { echo none; return; }
    echo "$(cat $d/idVendor):$(cat $d/idProduct) $(cat $d/product 2>/dev/null) serial=$(cat $d/serial 2>/dev/null) speed=$(cat $d/speed)"
}

mode_name() {
    case "$1" in
    1d6b:0104*) echo "A6L RECOVERY (adb serial HLTE730T-PROBE)";;
    18d1:d00d*) echo "fastboot/ABL";;
    109b:*) echo "stock Android";;
    05c6:9008*) echo "EDL 9008";;
    none) echo "NOTHING enumerated";;
    *) echo "unknown";;
    esac
}

status() {
    say "port $PORT: $(dev_on_port) -> $(mode_name "$(dev_on_port)")"
    if [ -d "$PORTDIR" ]; then
        for f in state connect_type disable over_current_count quirks; do
            [ -e "$PORTDIR/$f" ] && echo "  $PORTDIR/$f = $(cat $PORTDIR/$f 2>&1)"
        done
    else
        echo "  port dir $PORTDIR not found"
    fi
    echo "  controller: $(readlink -f /sys/bus/usb/devices/usb$BUS 2>/dev/null)"
    echo "  other phone-like devices anywhere:"
    for d in /sys/bus/usb/devices/*; do
        [ -e "$d/idVendor" ] || continue
        id="$(cat $d/idVendor):$(cat $d/idProduct)"
        case " $KNOWN " in *" $id "*) echo "    ${d##*/} $id $(mode_name $id)";; esac
    done
    echo "  last kernel USB lines for $PORT:"
    journalctl -k --no-pager -n 400 2>/dev/null | grep -E "usb $PORT:|usb$BUS-port$PNUM|hub $BUS-0" | tail -8 | sed 's/^/    /'
    command -v uhubctl >/dev/null && { echo "  uhubctl view:"; uhubctl 2>&1 | sed 's/^/    /' | head -20; } || echo "  uhubctl: not installed (sudo apt install uhubctl)"
}

wait_recovery() { # wait_recovery SEC -> 0 when 1d6b:0104 appears on any port
    t=${1:-60}; i=0
    while [ $i -lt $t ]; do
        for d in /sys/bus/usb/devices/*; do
            [ -e "$d/idVendor" ] || continue
            [ "$(cat $d/idVendor):$(cat $d/idProduct)" = 1d6b:0104 ] && { say "recovery enumerated on ${d##*/} after ${i}s"; return 0; }
        done
        sleep 1; i=$((i + 1))
    done
    say "no recovery device after ${t}s (port $PORT: $(mode_name "$(dev_on_port)"))"
    return 1
}

do_reset() {
    d=/sys/bus/usb/devices/$PORT
    if [ ! -e "$d/busnum" ]; then
        say "nothing enumerated on $PORT: usbreset needs a device; use port-cycle / replug instead"; return 1
    fi
    b=$(printf %03d $(cat $d/busnum)); n=$(printf %03d $(cat $d/devnum))
    say "usbreset /dev/bus/usb/$b/$n ($(dev_on_port))"
    if [ -w /dev/bus/usb/$b/$n ]; then usbreset /dev/bus/usb/$b/$n; else sudo usbreset /dev/bus/usb/$b/$n; fi
}

port_cycle() {
    [ -e "$PORTDIR/disable" ] || { say "no $PORTDIR/disable attribute (kernel too old?)"; return 1; }
    say "logical port-cycle $PORTDIR (disable 1 -> 2 s -> 0); sudo may ask for the password"
    echo 1 | sudo tee "$PORTDIR/disable" >/dev/null || return 1
    sleep 2
    echo 0 | sudo tee "$PORTDIR/disable" >/dev/null || return 1
    say "port re-enabled; state=$(cat $PORTDIR/state 2>/dev/null)"
}

uhub() {
    command -v uhubctl >/dev/null || { say "uhubctl not installed"; return 1; }
    loc=$BUS; p=$PNUM
    if [ "${PORT#*.}" != "$PORT" ]; then loc=${PORT%.*}; p=${PORT##*.}; fi
    say "uhubctl -l $loc -p $p -a cycle -d 3 (sudo)"
    sudo uhubctl -l "$loc" -p "$p" -a cycle -d 3
}

xhci_rebind() {
    [ "${1:-}" = --force ] || { say "xhci-rebind drops EVERY device on this controller; re-run with --force"; return 1; }
    pci=$(basename "$(readlink -f /sys/bus/usb/devices/usb$BUS/..)")
    say "devices that will drop with controller $pci:"
    for u in /sys/bus/pci/devices/$pci/usb*; do lsusb -s "$(cat $u/busnum):" 2>/dev/null; done
    printf 'type YES to rebind %s: ' "$pci"; read -r a; [ "$a" = YES ] || return 1
    echo "$pci" | sudo tee /sys/bus/pci/drivers/xhci_hcd/unbind >/dev/null
    sleep 3
    echo "$pci" | sudo tee /sys/bus/pci/drivers/xhci_hcd/bind >/dev/null
    say "rebound $pci"
}

cmd=${1:-status}; shift 2>/dev/null || true
case "$cmd" in
status) status ;;
wait) wait_recovery "${1:-60}" ;;
reset) do_reset ;;
port-cycle) port_cycle && wait_recovery 30 ;;
uhubctl) uhub && wait_recovery 40 ;;
kick)
    wait_recovery "${1:-40}" && exit 0
    port_cycle && wait_recovery 30 && exit 0
    command -v uhubctl >/dev/null && uhub && wait_recovery 40 && exit 0
    say "host side cannot fix it: unplug the USB-C cable AT THE PHONE, wait 5 s, plug it back, then: $0 wait 60"
    say "(the V75-usb recovery re-attaches by itself within ~2 min; on V74 a long-press Power restart is the fallback)"
    exit 1 ;;
xhci-rebind) xhci_rebind "${1:-}" ;;
*) sed -n '2,20p' "$0"; exit 2 ;;
esac
