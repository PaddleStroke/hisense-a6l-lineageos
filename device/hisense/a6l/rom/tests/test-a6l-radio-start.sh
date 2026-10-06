#!/usr/bin/env bash
# r5 review fix F8 (28 Sep 2026): offline host test of a6l-radio.sh start against a fake root (A6L_ROOT): fake sysfs,
# /proc/modules, module files, and shims for getprop/setprop/insmod/sleep/ip. Runs the real script with dash.
# Asserts that in every failure case (missing module, deferred IPA probe that never binds, missing rmnet_ipa0, failed
# daemon, ...) the modem driver (qcom_q6v5_mss) is never inserted after a failed IPA prerequisite and the MSS start is
# never written; that IPA is always loaded before qcom_q6v5_mss; and that a running modem is never started twice.
# usage: bash rom/tests/test-a6l-radio-start.sh [<repo>/device/hisense/a6l/rom]  -> A6L_RADIO_START_TEST PASS|FAIL n
D=${1:-$(cd "$(dirname "$0")/.." && pwd)}; W=$(mktemp -d); fails=0
exp() { [ "$2" = "$3" ] && echo "ok   $1 ($3)" || { echo "FAIL $1: got '$2' want '$3'"; fails=$((fails+1)); }; }
tr -d '\r' < $D/bin/a6l-radio.sh > $W/a6l-radio.sh
mkdir -p $W/bin
cat > $W/bin/getprop <<'SH'
#!/bin/sh
cat "$FAKE/props/$1" 2>/dev/null
SH
cat > $W/bin/setprop <<'SH'
#!/bin/sh
echo "$1 $2" >> "$FAKE/setprop.log"
case "$1" in
ctl.start) case " $FAIL_SVCS " in *" $2 "*) echo restarting;; *) echo running;; esac > "$FAKE/props/init.svc.$2";;
ctl.stop) echo stopped > "$FAKE/props/init.svc.$2";;
*) printf '%s' "$2" > "$FAKE/props/$1";;
esac
SH
cat > $W/bin/insmod <<'SH'
#!/bin/sh
n=$(basename "$1" .ko | tr - _)
echo "$n" >> "$FAKE/insmod.log"
[ -f "$1" ] || { echo "insmod: $1: No such file" >&2; exit 1; }
case " $FAIL_MODS " in *" $n "*) echo "insmod: $n: Invalid argument" >&2; exit 1;; esac
echo "$n 16384 0 - Live 0x0" >> "$FAKE/root/proc/modules"
S=$FAKE/root/sys
if [ "$n" = ipa2_lite ]; then
  case "$IPA_MODE" in
  ok) mkdir -p $S/bus/platform/drivers/ipa2-lite/14780000.ipa $S/class/net/rmnet_ipa0;;
  nonetdev) mkdir -p $S/bus/platform/drivers/ipa2-lite/14780000.ipa;;
  deferred) echo 3 > $FAKE/ipa_defer;;       # binds after 3 sleeps (deferred probe)
  never) ;;
  esac
fi
if [ "$n" = ath10k_snoc ]; then mkdir -p $S/class/net/wlan0; echo 00:00:00:00:00:00 > $S/class/net/wlan0/address; fi
exit 0
SH
cat > $W/bin/sleep <<'SH'
#!/bin/sh
S=$FAKE/root/sys
if [ -f $FAKE/ipa_defer ]; then
  c=$(($(cat $FAKE/ipa_defer) - 1)); echo $c > $FAKE/ipa_defer
  [ $c -le 0 ] && { rm -f $FAKE/ipa_defer; mkdir -p $S/bus/platform/drivers/ipa2-lite/14780000.ipa $S/class/net/rmnet_ipa0; }
fi
st=$S/class/remoteproc/remoteproc1/state
case "$(cat $st 2>/dev/null)" in
start) [ -n "$MSS_NEVER_RUNS" ] || echo running > $st;;
stop) if [ -n "$STOP_STUCK" ]; then echo running > $st; else echo offline > $st; fi;;
esac
exit 0
SH
cat > $W/bin/ip <<'SH'
#!/bin/sh
echo "$*" >> "$FAKE/ip.log"
case "$*" in *address*) [ -n "$IP_FAIL" ] && exit 2;; esac
exit 0
SH
# r5 review F40: a state path that rejects the stop write (a directory: EISDIR) while cat still reports 'running'
mkdir -p $W/bin-rejectstop
cat > $W/bin-rejectstop/cat <<'SH'
#!/bin/sh
case "$1" in */remoteproc1/state) echo running;; *) exec /bin/cat "$@";; esac
SH
chmod +x $W/bin/* $W/bin-rejectstop/*
setup() {   # fresh fake root; $1 = initial MSS state
  F=$W/case; rm -rf $F; mkdir -p $F/props $F/root/proc $F/root/dev $F/root/vendor/lib/modules $F/root/vendor/etc/a6l/modules
  : > $F/root/proc/modules; : > $F/insmod.log; : > $F/setprop.log; touch $F/root/dev/qcom_rmtfs_mem1
  for g in ipa radio bt; do tr -d '\r' < $D/modules/$g.txt > $F/root/vendor/etc/a6l/modules/$g.txt; done
  for k in $(cat $F/root/vendor/etc/a6l/modules/*.txt | grep -v '^#' | awk '{print $1}') qrtr.ko qrtr-smd.ko; do touch $F/root/vendor/lib/modules/$k; done
  for r in 0 1; do mkdir -p $F/root/sys/class/remoteproc/remoteproc$r; done
  echo 15700000.remoteproc > $F/root/sys/class/remoteproc/remoteproc0/name; echo offline > $F/root/sys/class/remoteproc/remoteproc0/state
  echo 4080000.remoteproc > $F/root/sys/class/remoteproc/remoteproc1/name; echo "${1:-offline}" > $F/root/sys/class/remoteproc/remoteproc1/state
  echo 1 > $F/props/persist.vendor.a6l.radio; echo 1 > $F/props/persist.vendor.a6l.ipa
  mkdir -p $F/root/vendor/bin $F/root/mnt/vendor/persist; printf '#!/bin/sh\n' > $F/root/vendor/bin/a6l_macs; chmod +x $F/root/vendor/bin/a6l_macs
  printf 'Intf0MacAddress=00AABBCCDDEE\n' > $F/root/mnt/vendor/persist/wlan_mac.bin; : > $F/ip.log
}
run() {
  env PATH="${XPATH:+$XPATH:}$W/bin:$PATH" FAKE=$W/case A6L_ROOT=$W/case/root A6L_IPA_WAIT=10 IPA_MODE=${IPA_MODE:-ok} \
      FAIL_MODS="$FAIL_MODS" FAIL_SVCS="$FAIL_SVCS" dash $W/a6l-radio.sh ${CMD:-start} > $W/case/out.log 2>&1
  rc=$?
  mss=$(grep -c '^qcom_q6v5_mss$' $W/case/insmod.log)
  st=$(cat $W/case/props/vendor.a6l.radio.state 2>/dev/null)
}
# failure cases: modem start never reached
fcase() { # name expected_state   (env: IPA_MODE FAIL_MODS FAIL_SVCS, extra setup already done)
  run
  exp "$1: rc" $rc 1; exp "$1: state" "$st" "$2"; exp "$1: MSS start never written" "$(cat $W/case/root/sys/class/remoteproc/remoteproc1/state)" "${3:-offline}"
}
setup; IPA_MODE=ok run
exp "normal: rc" $rc 0; exp "normal: state" "$st" up; exp "normal: modem running" "$(cat $W/case/root/sys/class/remoteproc/remoteproc1/state)" running
exp "normal: ipa2_lite inserted before qcom_q6v5_mss" "$(grep -n '^ipa2_lite$\|^qcom_q6v5_mss$' $W/case/insmod.log | cut -d: -f2 | tr '\n' ' ')" "ipa2_lite qcom_q6v5_mss "
exp "normal: bt loaded" "$(grep -c . <<< "$(grep -x 'btqca\|hci_uart' $W/case/insmod.log)")" "$(grep -v '^#' $D/modules/bt.txt | grep -c 'btqca\|hci_uart')"
# r5 review fix F5: the Wi-Fi HAL gate (libwifi-hal-a6l waits for it) is published after the wlan0 MAC step
exp "normal: wlan state ready" "$(grep '^vendor.a6l.wlan.state ' $W/case/setprop.log | tail -n 1)" "vendor.a6l.wlan.state ready"
setup; IPA_MODE=deferred run
exp "deferred probe that binds: rc" $rc 0; exp "deferred probe that binds: modem running" "$(cat $W/case/root/sys/class/remoteproc/remoteproc1/state)" running
setup; FAIL_MODS=ipa2_lite fcase "missing IPA module" failed:ipa-module;       exp "missing IPA module: mss not inserted" $mss 0
setup; FAIL_MODS=rmnet fcase "failed rmnet module" failed:ipa-module;          exp "failed rmnet module: ipa2_lite not inserted after it" "$(grep -c '^ipa2_lite$' $W/case/insmod.log)" 0
setup; IPA_MODE=never fcase "deferred IPA probe never binds" failed:ipa-not-bound; exp "deferred never: mss not inserted" $mss 0
setup; IPA_MODE=nonetdev fcase "missing rmnet_ipa0" failed:ipa-no-rmnet_ipa0;  exp "missing netdev: mss not inserted" $mss 0
for s in vendor.rmtfs vendor.tqftpserv vendor.diag_router; do
  setup; FAIL_SVCS=$s fcase "failed daemon $s" failed:daemon-$s; exp "failed daemon $s: mss not inserted" $mss 0
done
setup; rm $W/case/root/dev/qcom_rmtfs_mem1; fcase "no rmtfs_mem1" failed:no-rmtfs_mem1
setup; FAIL_MODS=qcom_q6v5_mss fcase "missing modem module" failed:modem-module
setup; FAIL_MODS=qcom_q6v5 fcase "failed modem dependency" failed:modem-module; exp "failed modem dep: mss not inserted" $mss 0
setup; FAIL_MODS=qrtr_smd fcase "failed qrtr-smd" failed:insmod-qrtr-smd
setup; echo 0 > $W/case/props/persist.vendor.a6l.ipa; fcase "ipa=0 without diagnostic opt-out" failed:ipa-disabled; exp "ipa=0: mss not inserted" $mss 0
setup crashed; fcase "crashed modem not restarted" failed:mss-state-crashed crashed
# explicit IPA-less diagnostic mode
setup; echo 0 > $W/case/props/persist.vendor.a6l.ipa; echo 1 > $W/case/props/persist.vendor.a6l.radio.ipa_less_diag; IPA_MODE=never run
exp "ipa_less_diag: rc" $rc 0; exp "ipa_less_diag: ipa2_lite not loaded" "$(grep -c '^ipa2_lite$' $W/case/insmod.log)" 0
exp "ipa_less_diag: modem running" "$(cat $W/case/root/sys/class/remoteproc/remoteproc1/state)" running
# no double start
setup running; run
exp "already running: rc" $rc 0; exp "already running: no start write" "$(grep -c 'not started again' $W/case/out.log)" 1
exp "already running: state" "$(cat $W/case/root/sys/class/remoteproc/remoteproc1/state)" running
# Wi-Fi module failure after the modem driver: modem still starts, reported degraded
setup; FAIL_MODS=ath10k_snoc run
exp "wifi module failure: rc" $rc 0; exp "wifi module failure: state" "$st" up:wifi-degraded
# radio gate off
setup; echo 0 > $W/case/props/persist.vendor.a6l.radio; run
exp "radio disabled: rc" $rc 0; exp "radio disabled: nothing inserted" "$(grep -c . $W/case/insmod.log)" 0
# ---- r5 review round4 F38/F39/F40 ----
btmods() { grep -c 'btqca\|hci_uart' $W/case/insmod.log; }
btaddr() { grep -c '^ctl.start vendor.a6l-bt-addr$' $W/case/setprop.log; }
wlst() { grep '^vendor.a6l.wlan.state ' $W/case/setprop.log | cut -d' ' -f2 | tr '\n' ' '; }
nbt=$(grep -v '^#' $D/modules/bt.txt | grep -c 'btqca\|hci_uart')
setup; run; exp "F39 normal: bt modules" $(btmods) $nbt; exp "F39 normal: bt address service started" $(btaddr) 1
setup; FAIL_MODS=ipa2_lite run; exp "F39 early fail: rc" $rc 1; exp "F39 early fail: bt modules" $(btmods) $nbt; exp "F39 early fail: bt address service started" $(btaddr) 1
setup; FAIL_SVCS=vendor.rmtfs run; exp "F39 daemon fail: bt address service started" $(btaddr) 1
setup; MSS_NEVER_RUNS=1 run; exp "F39 late fail (modem never running): rc" $rc 1; exp "F39 late fail: state" "$st" failed:mss-start
exp "F39 late fail: bt modules" $(btmods) $nbt; exp "F39 late fail: bt address service started" $(btaddr) 1
setup; rm $W/case/root/vendor/bin/a6l_macs; FAIL_MODS=ipa2_lite run; exp "F39 no a6l_macs: no address start" $(btaddr) 0
setup; echo 0 > $W/case/props/persist.vendor.a6l.radio; run; exp "F39 RF gate off: bt untouched (policy)" "$(btmods) $(btaddr)" "0 0"
# F38: fresh generation, explicit MAC outcome, live link never taken down
setup; run; exp "F38 normal: wlan states" "$(wlst)" "starting ready "
exp "F38 normal: MAC set with link down" "$(tr '\n' '|' < $W/case/ip.log)" "link set wlan0 down|link set wlan0 address 00:aa:bb:cc:dd:ee|"
setup; printf ready > $W/case/props/vendor.a6l.wlan.state; FAIL_MODS=ipa2_lite run
exp "F38 stale ready cleared on a new start" "$(cat $W/case/props/vendor.a6l.wlan.state)" starting
setup; IP_FAIL=1 run; exp "F38 MAC write failure explicit" "$(wlst)" "starting ready:mac-failed "
setup; : > $W/case/root/mnt/vendor/persist/wlan_mac.bin; run; exp "F38 no persist MAC explicit" "$(wlst)" "starting ready:no-persist-mac "
setup running; mkdir -p $W/case/root/sys/class/net/wlan0; echo 0x1003 > $W/case/root/sys/class/net/wlan0/flags; echo ath10k_snoc 1 0 - Live 0 >> $W/case/root/proc/modules
run; exp "F38 repeated start, Wi-Fi up: rc" $rc 0; exp "F38 repeated start: link not touched" "$(grep -c . $W/case/ip.log)" 0
exp "F38 repeated start: state" "$(wlst)" "starting ready:link-up "
# F40: stop only reports stopped (and stops rmtfs/tqftpserv/diag) once the modem is confirmed offline
stops() { grep -c '^ctl.stop ' $W/case/setprop.log; }
setup running; CMD=stop run; exp "F40 stop: rc" $rc 0; exp "F40 stop: state" "$st" stopped; exp "F40 stop: modem offline" "$(cat $W/case/root/sys/class/remoteproc/remoteproc1/state)" offline; exp "F40 stop: daemons stopped" $(stops) 3
setup running; STOP_STUCK=1 CMD=stop run; exp "F40 modem stays up: rc" $rc 1; exp "F40 modem stays up: state" "$st" failed:stop-running; exp "F40 modem stays up: daemons kept" $(stops) 0
setup running; rm $W/case/root/sys/class/remoteproc/remoteproc1/state; mkdir $W/case/root/sys/class/remoteproc/remoteproc1/state
XPATH=$W/bin-rejectstop CMD=stop run; exp "F40 rejected stop write: rc" $rc 1; exp "F40 rejected write: state" "$st" failed:stop-write; exp "F40 rejected write: daemons kept" $(stops) 0
setup crashed; CMD=stop run; exp "F40 crashed modem: rc" $rc 1; exp "F40 crashed: state" "$st" failed:stop-crashed; exp "F40 crashed: daemons kept" $(stops) 0
setup offline; CMD=stop run; exp "F40 already offline: rc" $rc 0; exp "F40 already offline: state" "$st" stopped; exp "F40 already offline: daemons stopped" $(stops) 3
setup; rm -rf $W/case/root/sys/class/remoteproc/remoteproc1; CMD=stop run; exp "F40 no MSS remoteproc: rc/state" "$rc $st" "0 stopped"
[ -n "$KEEP" ] && echo "kept $W" || rm -rf $W
echo "A6L_RADIO_START_TEST $([ $fails = 0 ] && echo PASS || echo FAIL $fails)"; exit $fails
