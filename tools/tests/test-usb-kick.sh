#!/usr/bin/env bash
# Host test for tools/usb-kick.sh EDL guard (bug hunt round2 install-tools, 29 Sep 2026). Fake sysfs + PATH stubs for
# sudo/usbreset/pgrep/uhubctl/journalctl: nothing touches real USB. usage: bash tools/tests/test-usb-kick.sh [kick script]
set -u
HERE=$(cd "$(dirname "$0")/.." && pwd); K=${1:-$HERE/usb-kick.sh}
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
fail=0; ok() { echo "PASS $1"; }; bad() { echo "FAIL $1"; fail=1; }
mkdir -p $T/bin $T/sys/bus/usb/devices/3-0:1.0/usb3-port2
for c in usbreset uhubctl journalctl; do printf '#!/bin/sh\necho "%s $*" >> %s/actions\n' $c $T > $T/bin/$c; done
printf '#!/bin/sh\n[ -f %s/worker ] && exit 0; exit 1\n' $T > $T/bin/pgrep
printf '#!/bin/sh\nshift 0; if [ "$1" = tee ]; then echo "tee $2" >> %s/actions; cat > /dev/null; else "$@"; fi\n' $T > $T/bin/sudo
chmod +x $T/bin/*
echo 0 > $T/sys/bus/usb/devices/3-0:1.0/usb3-port2/disable
dev() { local d=$T/sys/bus/usb/devices/$1; rm -rf $d; mkdir -p $d; echo $2 > $d/idVendor; echo $3 > $d/idProduct; echo 3 > $d/busnum; echo 7 > $d/devnum; echo 480 > $d/speed; }
run() { rm -f $T/actions; PATH=$T/bin:$PATH A6L_SYSFS=$T/sys timeout 20 bash $K "$@" > $T/out 2>&1; echo $?; }
acted() { [ -s $T/actions ] && grep -qE "usbreset|tee .*disable|uhubctl -l|xhci_hcd" $T/actions; }
# EDL on the phone port: every disruptive action refused, nothing done
dev 3-2 05c6 9008
for a in reset port-cycle uhubctl xhci-rebind "kick 1"; do rc=$(run $a --force)
  if [ "$rc" = 3 ] && ! acted && grep -q REFUSED $T/out; then ok "edl_refuses_$a"; else bad "edl_refuses_$a rc=$rc $(cat $T/actions 2>/dev/null | tr '\n' ';')"; fi; done
# status still works in EDL
rc=$(run status); [ "$rc" = 0 ] && grep -q "EDL 9008" $T/out && ok status_in_edl || bad "status_in_edl rc=$rc"
# EDL on another port also blocks the whole-controller rebind
rm -rf $T/sys/bus/usb/devices/3-2; dev 3-1 05c6 9008; rc=$(run xhci-rebind --force)
[ "$rc" = 3 ] && ! acted && ok edl_other_port_blocks_rebind || bad "edl_other_port_blocks_rebind rc=$rc"
rm -rf $T/sys/bus/usb/devices/3-1
# worker running, device in stock Android: refused
dev 3-2 109b 911f; touch $T/worker; rc=$(run reset)
[ "$rc" = 3 ] && ! acted && ok worker_refuses_reset || bad "worker_refuses_reset rc=$rc"
rm -f $T/worker
# no EDL, no worker: reset and port-cycle act as before
rc=$(run reset); grep -q "usbreset /dev/bus/usb/003/007" $T/actions 2>/dev/null && ok reset_acts || bad "reset_acts rc=$rc"
rc=$(run port-cycle); grep -q "tee .*usb3-port2/disable" $T/actions 2>/dev/null && ok port_cycle_acts || bad "port_cycle_acts rc=$rc"
# explicit override when no flash runs
dev 3-2 05c6 9008; rc=$(A6L_KICK_EDL_OK=1 run reset); grep -q usbreset $T/actions 2>/dev/null && ok edl_override || bad "edl_override rc=$rc"
touch $T/worker; rc=$(A6L_KICK_EDL_OK=1 run reset); [ "$rc" = 3 ] && ! acted && ok override_never_with_worker || bad "override_never_with_worker rc=$rc"
echo "USB_KICK_TEST $([ $fail = 0 ] && echo PASS || echo FAIL)"; exit $fail
