#!/usr/bin/env bash
# agent power (26 Sep 2026; r5 28 Sep: review F1/F2 guard cases; 28 Sep: new thresholds T_ABORT 50.0 / T_START_MAX 45.0, power28 QC monitor; power29 29 Sep: gadget pull-up / APSD rerun / watchdog cases): offline simulation of the charger monitor (run-power.sh MODE=chg-mon) and of the ROM
# software-JEITA guard (a6l-chg-guard.sh) against a fake power_supply tree, with dash (POSIX sh like toybox/mksh).
# usage: sim-power.sh <repo>/device/hisense/a6l/power   -> prints A6L_POWER_SIM PASS|FAIL n
P=${1:-$(dirname "$0")/..}; W=$(mktemp -d); fails=0
exp() { [ "$2" = "$3" ] && echo "ok   $1 ($3)" || { echo "FAIL $1: got '$2' want '$3'"; fails=$((fails+1)); }; }
await_value() { # Wait for an asynchronous guard write, including on a busy build host.
    local i=0
    while [ $i -lt 100 ]; do
        [ "$(cat "$1" 2>/dev/null)" = "$2" ] && return 0
        sleep 0.1; i=$((i+1))
    done
    return 1
}
# ---- monitor
F=$W/m; mkdir -p $F/b $F/c
echo Charging > $F/b/status; echo 50 > $F/b/capacity; echo 3900000 > $F/b/voltage_now; echo -800000 > $F/b/current_now; echo 300 > $F/b/temp
echo 1 > $F/c/online; echo Good > $F/c/health; echo 500000 > $F/c/current_max; echo 1 > $F/c/status
sed 's/sleep 10; n=\$((n+10))/sleep 0.3; n=$((n+10))/' $P/bundle/run-power.sh > $W/rp.sh
# 28 Sep thresholds (Pierre): monitor abort T_ABORT >= 50.0 C, start refused T_START_MAX >= 45.0 C, stock JEITA hot 55 C
echo 495 > $F/b/temp
MODE=chg-mon BAT=$F/b CHG=$F/c DUR=20 dash $W/rp.sh > $W/m0.log; exp "mon 49.5C no abort rc" $? 0; exp "mon 49.5C keeps charging" "$(cat $F/c/status)" 1
echo 300 > $F/b/temp
( sleep 0.5; echo 500 > $F/b/temp ) &
MODE=chg-mon BAT=$F/b CHG=$F/c DUR=60 dash $W/rp.sh > $W/m1.log; exp "mon temp abort 50.0C rc" $? 3; exp "mon temp abort suspends" "$(cat $F/c/status)" 0
echo 300 > $F/b/temp; echo 1 > $F/c/status
( sleep 0.5; echo 405 > $F/b/temp ) &
T_ABORT=400 MODE=chg-mon BAT=$F/b CHG=$F/c DUR=60 dash $W/rp.sh > $W/m1b.log; exp "mon T_ABORT override 40.0C rc" $? 3
echo 450 > $F/b/temp; echo 1 > $F/c/status
MODE=chg-on D=$W BAT=$F/b CHG=$F/c dash $W/rp.sh > $W/s1.log; exp "start refused at 45.0C rc" $? 1; exp "start refused at 45.0C msg" "$(grep -c 'refused: battery already at 450' $W/s1.log)" 1
echo 449 > $F/b/temp
MODE=chg-on D=$W BAT=$F/b CHG=$F/c dash $W/rp.sh > $W/s2.log; exp "start 44.9C passes temp gate" "$(grep -c 'refused: battery' $W/s2.log)" 0
echo 300 > $F/b/temp
echo 300 > $F/b/temp; echo 1 > $F/c/status; echo -1200000 > $F/b/current_now
MODE=chg-mon BAT=$F/b CHG=$F/c DUR=60 dash $W/rp.sh > $W/m2.log; exp "mon overcurrent abort" "$(cat $F/c/status)" 0
echo -700000 > $F/b/current_now; echo 1 > $F/c/status; echo 800000 > $F/c/current_max
MODE=chg-mon BAT=$F/b CHG=$F/c DUR=30 dash $W/rp.sh > $W/m3.log; exp "mon normal end keeps charging" "$(cat $F/c/status)" 1; exp "mon end ICL" "$(cat $F/c/current_max)" 500000
echo 4430000 > $F/b/voltage_now
MODE=chg-mon BAT=$F/b CHG=$F/c DUR=30 dash $W/rp.sh > $W/m4.log; exp "mon OV abort" "$(cat $F/c/status)" 0
echo 4000000 > $F/b/voltage_now; echo 1 > $F/c/status; echo -600000 > $F/b/current_now
MODE=chg-mon BAT=$F/b CHG=$F/c DUR=20 dash $W/rp.sh > $W/m5.log; exp "mon CHG_ON_PASS marker once" "$(grep -c CHG_ON_PASS $W/m5.log)" 1
echo -5 > $F/b/temp; echo 1 > $F/c/status
MODE=chg-mon BAT=$F/b CHG=$F/c DUR=30 dash $W/rp.sh > $W/m6.log; exp "mon cold abort" "$(cat $F/c/status)" 0
echo 300 > $F/b/temp
# ---- quick charge monitor (power28: run-power.sh MODE=qc / qc-mon / chg-off with HVDCP)
H=$W/h; mkdir -p $H; echo 300 > $F/b/temp; echo 3900000 > $F/b/voltage_now; echo -900000 > $F/b/current_now
qs() { echo "state=$1 qc=3 pulses=20 target_uV=9000000 usbin_uV=$2 usbin_uA=$3 icl_uA=1000000 enable=1 reason=-" > $H/hvdcp_status; }
qreset() { echo Y > $H/hvdcp_enable; echo 1500000 > $H/fcc_max_ua; echo 1000000 > $H/hvdcp_icl_ua; echo 1 > $F/c/status; echo 1000000 > $F/c/current_max; }
QM() { MODE=qc-mon BAT=$F/b CHG=$F/c HVP=$H QICL=1000000 QSLEEP=0.2 QOFF_WAIT=0 dash $P/bundle/run-power.sh; }
qreset; qs active 9010000 900000
DUR=10 QM > $W/q1.log; exp "qc mon normal rc" $? 0; exp "qc QC_PASS once" "$(grep -c QC_PASS $W/q1.log)" 1
exp "qc end -> hvdcp off (5 V)" "$(cat $H/hvdcp_enable)" 0; exp "qc end ICL 500 mA" "$(cat $F/c/current_max)" 500000; exp "qc end keeps input on" "$(cat $F/c/status)" 1
qreset; qs active 9700000 900000
DUR=20 QM > $W/q2.log; exp "qc USBIN 9.7 V abort rc" $? 3; exp "qc 9.7 V -> hvdcp off" "$(cat $H/hvdcp_enable)" 0; exp "qc 9.7 V -> input suspended" "$(cat $F/c/status)" 0
qreset; qs active 9600000 900000
DUR=10 QM > $W/q2b.log; exp "qc USBIN 9.6 V exactly: no abort" $? 0
qreset; qs active 9000000 900000; echo 500 > $F/b/temp
DUR=20 QM > $W/q3.log; exp "qc temp 50.0 C abort rc" $? 3; exp "qc temp abort suspends" "$(cat $F/c/status)" 0; echo 300 > $F/b/temp
qreset; echo 4430000 > $F/b/voltage_now
DUR=20 QM > $W/q4.log; exp "qc battery 4.43 V abort rc" $? 3; echo 3900000 > $F/b/voltage_now
qreset; qs active 9000000 1300000
DUR=20 QM > $W/q5.log; exp "qc USBIN 1.3 A at ICL 1.0 A abort rc" $? 3; exp "qc overcurrent msg" "$(grep -c 'USBIN current 1300000' $W/q5.log)" 1
qreset; qs active 9000000 1150000
DUR=20 QM > $W/q5b.log; exp "qc USBIN 1.15 A (inside +10 % +100 mA) no abort" $? 0
qreset; qs active 9000000 900000; echo -1700000 > $F/b/current_now
DUR=20 QM > $W/q6.log; exp "qc battery 1.7 A at FCC 1.5 A abort rc" $? 3; echo -900000 > $F/b/current_now
qreset; echo "state=failed qc=2 pulses=0 target_uV=9000000 usbin_uV=5010000 usbin_uA=400000 icl_uA=500000 enable=1 reason=overvoltage 9900000" > $H/hvdcp_status
DUR=10 QM > $W/q7.log; exp "qc driver abort reported, monitor continues" "$? $(grep -c QC_DRIVER_ABORT $W/q7.log) $(grep -c QC_PASS $W/q7.log)" "0 1 0"
qreset; qs active 9000000 900000; echo 450 > $F/b/temp
MODE=qc D=$W BAT=$F/b CHG=$F/c HVP=$H dash $P/bundle/run-power.sh > $W/q8.log; exp "qc start refused at 45.0 C" "$? $(grep -c 'refused: battery already at 450' $W/q8.log)" "1 1"
echo 300 > $F/b/temp
MODE=qc QICL=1600000 D=$W BAT=$F/b CHG=$F/c HVP=$H dash $P/bundle/run-power.sh > $W/q9.log; exp "qc QICL 1.6 A refused without FORCE" "$(grep -c 'refused: QICL' $W/q9.log)" 1
MODE=qc QV=12000000 D=$W BAT=$F/b CHG=$F/c HVP=$H dash $P/bundle/run-power.sh > $W/q10.log; exp "qc 12 V refused" "$(grep -c 'refused: QV' $W/q10.log)" 1
MODE=qc FCC=2000000 D=$W BAT=$F/b CHG=$F/c HVP=$H dash $P/bundle/run-power.sh > $W/q11.log; exp "qc FCC 2.0 A refused without FORCE" "$(grep -c 'refused: FCC' $W/q11.log)" 1
MODE=qc D=$W BAT=$F/b CHG=$F/c HVP=$H dash $P/bundle/run-power.sh > $W/q12.log; exp "qc refuses a qcom_smbx.ko without HVDCP" "$(grep -c 'not the power28 build' $W/q12.log)" 1
qreset; MODE=chg-off BAT=$F/b CHG=$F/c HVP=$H QOFF_WAIT=0 dash $P/bundle/run-power.sh > $W/q13.log
exp "chg-off: HVDCP off then suspend" "$(cat $H/hvdcp_enable)/$(cat $F/c/status)" 0/0
# ---- power29 (29 Sep): gadget pull-up off on the wall charger, one APSD rerun, watchdog stop/restart, register dump
U=$W/udc; mkdir -p $U/a800000.usb; echo connect > $U/a800000.usb/soft_connect
printf '#!/bin/sh\necho "$*" >> %s/setprop.log\n' $W > $W/setprop; chmod +x $W/setprop
echo "1307=01 1308=02 1362=4c" > $H/hvdcp_regs
Q9() { MODE=qc-mon BAT=$F/b CHG=$F/c HVP=$H QICL=1000000 QSLEEP=0.2 QOFF_WAIT=0 UDCC=$U SETPROP=$W/setprop WDMARK=$W/wdmark dash $P/bundle/run-power.sh; }
qreset; : > $W/setprop.log; : > $W/wdmark; : > $H/hvdcp_rerun; echo "Unknown [SDP] DCP CDP" > $F/c/usb_type; echo 1 > $F/c/online
echo "state=idle qc=0 pulses=0 target_uV=0 usbin_uV=4960937 usbin_uA=451240 icl_uA=0 enable=1 apsd=0x01/0x01 reruns=0 reason=-" > $H/hvdcp_status
( sleep 0.6; echo 0 > $F/c/online; echo "[Unknown] SDP DCP CDP" > $F/c/usb_type; sleep 0.6; cp $U/a800000.usb/soft_connect $W/sc-unplugged
  echo 1 > $F/c/online; echo "Unknown SDP [DCP] CDP" > $F/c/usb_type
  echo "state=dcp-5v qc=0 pulses=0 target_uV=0 usbin_uV=5078125 usbin_uA=755136 icl_uA=800000 enable=1 apsd=0x41/0x02 reruns=0 reason=-" > $H/hvdcp_status
  await_value $H/hvdcp_rerun 1; cp $H/hvdcp_rerun $W/rerun-wall; echo 0 > $F/c/online; sleep 0.6; echo 1 > $F/c/online; echo "Unknown [SDP] DCP CDP" > $F/c/usb_type
  echo "state=idle qc=0 pulses=0 target_uV=0 usbin_uV=4960937 usbin_uA=451240 icl_uA=0 enable=1 apsd=0x01/0x01 reruns=0 reason=-" > $H/hvdcp_status ) &
DUR=80 Q9 > $W/q14.log; wait
exp "p29 SDP at start: gadget untouched" "$(grep -c 'soft_connect=' $W/q14.log | awk '{print ($1>0)}')/$(head -3 $W/q14.log | grep -c soft_connect)" 1/0
exp "p29 unplug -> gadget pull-up off" "$(cat $W/sc-unplugged)" disconnect
exp "p29 dcp-5v on the wall -> exactly one APSD rerun" "$(cat $W/rerun-wall)/$(grep -c QC_RERUN $W/q14.log)" 1/1
exp "p29 back on SDP -> gadget connected" "$(cat $U/a800000.usb/soft_connect)" connect
exp "p29 regs in every QCMON line" "$(grep 'QCMON t=' $W/q14.log | grep -vc 'regs\[1307=01')" 0
exp "p29 QC_RESULT FAIL (wall seen, no QC)" "$(grep -c 'QC_RESULT FAIL wall charger seen' $W/q14.log)" 1
exp "p29 end -> watchdog restarted" "$(grep -c 'ctl.start a6lusbwd' $W/setprop.log)/$([ -e $W/wdmark ] && echo mark || echo nomark)" 1/nomark
qreset; : > $W/wdmark; echo 1 > $F/c/online; echo "Unknown SDP [DCP] CDP" > $F/c/usb_type; echo connect > $U/a800000.usb/soft_connect; qs active 9000000 900000
DUR=6 Q9 > $W/q15.log; exp "p29 QC_RESULT PASS" "$(grep -c 'QC_RESULT PASS max usbin 9000000' $W/q15.log)" 1
exp "p29 PASS: no rerun while active, gadget restored" "$(grep -c QC_RERUN $W/q15.log)/$(cat $U/a800000.usb/soft_connect)" 0/connect
qreset; : > $W/wdmark; : > $W/setprop.log; echo disconnect > $U/a800000.usb/soft_connect; qs active 9800000 900000
DUR=20 Q9 > $W/q16.log; exp "p29 9.8 V abort still works (rc 3, suspend)" "$?/$(cat $F/c/status)/$(cat $H/hvdcp_enable)" 3/0/0
exp "p29 abort -> gadget + watchdog restored" "$(cat $U/a800000.usb/soft_connect)/$(grep -c 'ctl.start' $W/setprop.log)" connect/1
qreset; qs active 9000000 900000; echo 510 > $F/b/temp
DUR=20 Q9 > $W/q17.log; exp "p29 50 C abort still works" "$?/$(cat $F/c/status)" 3/0; echo 300 > $F/b/temp
qreset; : > $W/wdmark; : > $W/setprop.log
MODE=qc-off BAT=$F/b CHG=$F/c HVP=$H QOFF_WAIT=0 SETPROP=$W/setprop WDMARK=$W/wdmark dash $P/bundle/run-power.sh > $W/q18.log
exp "p29 qc-off: HVDCP off, suspend, watchdog restarted" "$(cat $H/hvdcp_enable)/$(cat $F/c/status)/$(grep -c 'ctl.start' $W/setprop.log)" 0/0/1
rm -f $F/c/usb_type $H/hvdcp_regs $H/hvdcp_rerun
# ---- guard
G=$W/g; mkdir -p $G/b $G/c
echo 250 > $G/b/temp; echo 3900000 > $G/b/voltage_now; echo 1 > $G/c/online; echo "Unknown SDP CDP [DCP]" > $G/c/usb_type; echo 1 > $G/c/current_max; echo 1 > $G/c/status
BAT=$G/b CHG=$G/c PERIOD=0.1 dash $P/rom/a6l-chg-guard.sh > $W/g.log 2>&1 & GP=$!
st() { echo "$1" > $G/b/temp; echo "$2" > $G/b/voltage_now; sleep 0.5; echo "$(cat $G/c/current_max)/$(cat $G/c/status)"; }
exp "guard normal DCP" "$(st 250 3900000)" 2000000/1
exp "guard cool" "$(st 50 3900000)" 700000/1
exp "guard cold" "$(st -10 3900000)" 700000/0
exp "guard cold hysteresis 1.0C" "$(st 10 3900000)" 700000/0
exp "guard cool again 3.0C" "$(st 30 3900000)" 700000/1
exp "guard 44.5C still normal" "$(st 445 3900000)" 2000000/1
exp "guard warm low V" "$(st 450 3900000)" 750000/1
exp "guard warm high V" "$(st 450 4120000)" 750000/0
exp "guard warm V hysteresis" "$(st 450 4070000)" 750000/0
exp "guard warm resume" "$(st 450 4000000)" 750000/1
exp "guard hot" "$(st 560 4000000)" 700000/0
exp "guard hot hysteresis" "$(st 540 4000000)" 700000/0
exp "guard warm after hot" "$(st 500 4000000)" 750000/1
exp "guard OV" "$(st 300 4430000)" 2000000/0
exp "guard OV hysteresis" "$(st 300 4380000)" 2000000/0
exp "guard OV clear" "$(st 300 4300000)" 2000000/1
echo "Unknown [SDP] CDP DCP" > $G/c/usb_type; exp "guard SDP" "$(st 300 4000000)" 500000/1
# r5 F1: the thermal limit never raises the source budget (review: SDP 5 C gave 700 mA, SDP 46 C gave 750 mA)
exp "guard SDP cool 5C stays 500 mA" "$(st 50 4000000)" 500000/1
exp "guard SDP warm 46C stays 500 mA" "$(st 460 4000000)" 500000/1
exp "guard SDP hot" "$(st 560 4000000)" 500000/0
exp "guard SDP cold" "$(st -10 4000000)" 500000/0
echo "Unknown SDP [CDP] DCP" > $G/c/usb_type
exp "guard CDP normal" "$(st 300 4000000)" 1500000/1
exp "guard CDP cool" "$(st 50 4000000)" 700000/1
exp "guard CDP warm" "$(st 460 4000000)" 750000/1
echo "[Unknown] SDP CDP DCP" > $G/c/usb_type
exp "guard Unknown cool" "$(st 50 4000000)" 500000/1
exp "guard Unknown warm" "$(st 460 4000000)" 500000/1
exp "guard Unknown normal" "$(st 300 4000000)" 500000/1
rm $G/b/temp; sleep 0.5; exp "guard no data suspends" "$(cat $G/c/status)" 0
echo 250 > $G/b/temp; sleep 0.5; exp "guard data back resumes" "$(cat $G/c/status)" 1
echo abc > $G/b/temp; sleep 0.5; exp "guard invalid telemetry suspends" "$(cat $G/c/status)" 0
kill $GP 2>/dev/null; wait 2>/dev/null
# ---- guard r5 F2: desired vs applied suspend state, fault injection
echo "Unknown SDP CDP [DCP]" > $G/c/usb_type
G1() { BAT=$G/b CHG=$G/c ONESHOT=1 dash $P/rom/a6l-chg-guard.sh > $W/g1.log 2>&1; }
echo 250 > $G/b/temp; echo 3900000 > $G/b/voltage_now; echo 0 > $G/c/status
G1; exp "restart normal while suspended -> explicit resume" "$(cat $G/c/status)" 1
echo 560 > $G/b/temp; echo 0 > $G/c/status
G1; exp "restart hot while suspended -> suspend re-applied" "$(cat $G/c/status)" 0
echo 1 > $G/c/status; G1; exp "restart hot while resumed -> suspend" "$(cat $G/c/status)" 0
# failed suspend write (node is a directory), node recovers, next iteration must retry
echo 560 > $G/b/temp; rm -f $G/c/status; mkdir $G/c/status
BAT=$G/b CHG=$G/c PERIOD=0.1 dash $P/rom/a6l-chg-guard.sh > $W/g2.log 2>&1 & GP=$!
sleep 0.5; exp "suspend write failure logged once" "$(grep -c 'ERROR: write .*status=0' $W/g2.log)" 1
rmdir $G/c/status; echo 1 > $G/c/status; sleep 0.5
exp "suspend retried after node recovery" "$(cat $G/c/status)" 0
exp "recovery logged" "$(grep -c 'suspend=1 applied after' $W/g2.log)" 1
# failed resume write, then recovery
rm -f $G/c/status; mkdir $G/c/status; echo 250 > $G/b/temp; sleep 0.5
exp "resume write failure logged" "$(grep -c 'ERROR: write .*status=1' $W/g2.log)" 1
rmdir $G/c/status; echo 0 > $G/c/status; sleep 0.5
exp "resume retried after node recovery" "$(cat $G/c/status)" 1
# missing node, then the node appears
rm -f $G/c/status; echo 560 > $G/b/temp; sleep 0.5; echo 1 > $G/c/status; sleep 0.5
exp "missing node then recovery -> suspend applied" "$(cat $G/c/status)" 0
echo 250 > $G/b/temp; sleep 0.5; exp "back to normal -> resume" "$(cat $G/c/status)" 1
# warm-voltage hysteresis still holds on the desired state while the write keeps failing
rm -f $G/c/status; mkdir $G/c/status; echo 460 > $G/b/temp; echo 4120000 > $G/b/voltage_now; sleep 0.4
echo 4070000 > $G/b/voltage_now; sleep 0.4; rmdir $G/c/status; echo 1 > $G/c/status; sleep 0.5
exp "warm V hysteresis across a failed write" "$(cat $G/c/status)" 0
echo 4000000 > $G/b/voltage_now; sleep 0.5; exp "warm resume below 4.05 V" "$(cat $G/c/status)" 1
exp "bounded logging (no ERROR flood)" "$([ "$(grep -c ERROR $W/g2.log)" -le 6 ] && echo ok)" ok
kill $GP 2>/dev/null; wait 2>/dev/null
# ---- r5 bug hunt power P1/P2 (29 Sep 2026): kernel ICL rewrite after plug-in, source change, hysteresis across a glitch
rmdir $G/c/status 2>/dev/null; echo 1 > $G/c/status; echo 460 > $G/b/temp; echo 3900000 > $G/b/voltage_now; echo 1 > $G/c/online
echo "Unknown SDP CDP [DCP]" > $G/c/usb_type; echo 1 > $G/c/current_max
BAT=$G/b CHG=$G/c PERIOD=5 TICK=0.1 TICKS=50 dash $P/rom/a6l-chg-guard.sh > $W/g3.log 2>&1 & GP=$!
sleep 0.5; exp "P1 warm DCP 750 mA" "$(cat $G/c/current_max)" 750000
echo 2000000 > $G/c/current_max; sleep 0.6
exp "P1 kernel plug-in ICL 2.0 A re-limited within a tick (not a period)" "$(cat $G/c/current_max)" 750000
echo 0 > $G/c/online; echo "[Unknown] SDP CDP DCP" > $G/c/usb_type; sleep 0.6
echo 1 > $G/c/online; echo "Unknown SDP [CDP] DCP" > $G/c/usb_type; echo 250 > $G/b/temp; sleep 0.6
exp "P1 source change re-evaluated within a tick (CDP normal 1.5 A)" "$(cat $G/c/current_max)" 1500000
echo 400000 > $G/c/current_max; sleep 0.6
exp "P1 AICL below the request: no rewrite loop (readback lower kept)" "$(cat $G/c/current_max)" 400000
kill $GP 2>/dev/null; wait 2>/dev/null
echo 560 > $G/b/temp; echo 4000000 > $G/b/voltage_now; echo 1 > $G/c/status; echo "Unknown SDP CDP [DCP]" > $G/c/usb_type
BAT=$G/b CHG=$G/c PERIOD=0.1 dash $P/rom/a6l-chg-guard.sh > $W/g4.log 2>&1 & GP=$!
sleep 0.5; exp "P2 hot suspended" "$(cat $G/c/status)" 0
rm $G/b/temp; sleep 0.4; echo 540 > $G/b/temp; sleep 0.5
exp "P2 hot hysteresis survives a telemetry glitch (54.0 C stays suspended)" "$(cat $G/c/status)" 0
echo 520 > $G/b/temp; sleep 0.5; exp "P2 below 53 C -> warm, resumes" "$(cat $G/c/status)" 1
echo 4120000 > $G/b/voltage_now; sleep 0.4; echo 4070000 > $G/b/voltage_now; rm $G/b/temp; sleep 0.4; echo 460 > $G/b/temp; sleep 0.5
exp "P2 warm-voltage hold survives a telemetry glitch" "$(cat $G/c/status)" 0
kill $GP 2>/dev/null; wait 2>/dev/null
# ---- fastcharge-rom (29 Sep 2026): guard Q1-Q5 = 9 V awareness (hvdcp_enable), driver-owned ramp ICL, USB pull-up, rerun, monitor
H2=$W/hv; U2=$W/udc; mkdir -p $H2 $U2/a800000.usb
echo 1 > $H2/hvdcp_enable; echo "state=active qc=3 pulses=20 target_uV=9000000 usbin_uV=9060000" > $H2/hvdcp_status; : > $H2/hvdcp_rerun
echo g1 > $U2/a800000.usb/function; echo "not attached" > $U2/a800000.usb/state; echo connect > $U2/a800000.usb/soft_connect; echo 250 > $G/b/temp; echo 3900000 > $G/b/voltage_now; echo 1 > $G/c/online; echo 1 > $G/c/status
echo "Unknown SDP CDP [DCP]" > $G/c/usb_type; echo 2000000 > $G/c/current_max; : > $W/icl_at_5v
# fake driver: after hvdcp_enable=0 it needs 0.3 s (QC3 decrements + FORCE_5V) before it leaves 'active'; records the ICL then
( while :; do if [ "$(cat $H2/hvdcp_enable)" = 0 ]; then case "$(cat $H2/hvdcp_status)" in state=active*|state=verify*)
    sleep 0.3; cat $G/c/current_max > $W/icl_at_5v; echo "state=off qc=0 pulses=0" > $H2/hvdcp_status;; esac; fi; sleep 0.05; done ) 2>/dev/null & DP=$!
QG() { HVP=$H2 UDCC=$U2 UDC=a800000.usb QC=${QC:-1} PULLUP=${PULLUP:-1} BAT=$G/b CHG=$G/c PERIOD=0.1 HV_WAIT_N=20 HV_WAIT_STEP=0.1 exec dash $P/rom/a6l-chg-guard.sh; }   # exec: $! = the guard (SIGTERM test)
QG > $W/q.log 2>&1 & GP=$!
qs() { echo "$1" > $G/b/temp; echo "$2" > $G/b/voltage_now; sleep 1.2; echo "$(cat $H2/hvdcp_enable)/$(cat $G/c/current_max)/$(cat $G/c/status)"; }
exp "Q normal 25C at 9 V: QC kept, 2.0 A (clamped by the driver to hvdcp_icl_ua)" "$(qs 250 3900000)" 1/2000000/1
exp "Q4 wall charger [DCP] -> pull-up off" "$(cat $U2/a800000.usb/soft_connect)" disconnect
exp "Q1 warm 46C: back to 5 V then 750 mA" "$(qs 460 3900000)" 0/750000/1
exp "Q1 ICL still the 9 V value until the driver left 'active' (no 750 mA at 9 V window reversed)" "$(cat $W/icl_at_5v)" 2000000
exp "Q2 44C normal again: no re-enable above 43.0 C (hysteresis)" "$(qs 440 3900000)" 0/2000000/1
exp "Q2 42.5C: QC re-enabled after the 5 V limit" "$(qs 425 3900000)" 1/2000000/1
echo "state=active qc=2 pulses=0 target_uV=9000000" > $H2/hvdcp_status
exp "Q1 cool 5C: 5 V + 700 mA" "$(qs 50 3900000)" 0/700000/1
exp "Q2 11C normal: stays at 5 V below 12.0 C" "$(qs 110 3900000)" 0/2000000/1
exp "Q2 13C: QC on" "$(qs 130 3900000)" 1/2000000/1
echo "state=active qc=2" > $H2/hvdcp_status
exp "Q1 OV 4.43 V: 5 V first, then input suspended" "$(qs 250 4430000)" 0/2000000/0
exp "Q2 OV cleared: resumed, then QC on" "$(qs 250 4300000)" 1/2000000/1
echo "state=active qc=2" > $H2/hvdcp_status
exp "Q1 hot 56C: 5 V + suspended" "$(qs 560 3900000)" 0/700000/0
exp "Q2 back to 25C: QC on" "$(qs 250 3900000)" 1/2000000/1
echo "state=active qc=2" > $H2/hvdcp_status; rm $G/b/temp; sleep 1.2
exp "Q1 no telemetry: 5 V + suspended" "$(cat $H2/hvdcp_enable)/$(cat $G/c/status)" 0/0
exp "Q2 telemetry back: QC on" "$(qs 250 3900000)" 1/2000000/1
kill $GP 2>/dev/null; wait $GP 2>/dev/null
QC=0 QG > $W/q0.log 2>&1 & GP=$!
exp "Q2 persist.vendor.a6l.chg.qc=0: 5 V in the normal zone" "$(qs 250 3900000)" 0/2000000/1
kill $GP 2>/dev/null; wait $GP 2>/dev/null
# Q3: driver ramp (wait/verify: ICL <= 1 A until 9 V is verified) is not overridden by the guard's 2.0 A
echo 1 > $H2/hvdcp_enable; sleep 0.2; echo "state=verify qc=3" > $H2/hvdcp_status; echo 1000000 > $G/c/current_max   # enable first (fake driver race)
QG > $W/q3.log 2>&1 & GP=$!
sleep 1.2; exp "Q3 verify: guard leaves the 1.0 A ramp limit" "$(cat $G/c/current_max)" 1000000
echo "state=active qc=3" > $H2/hvdcp_status; sleep 1.2
exp "Q3 active: guard budget 2.0 A (driver clamps to hvdcp_icl_ua)" "$(cat $G/c/current_max)" 2000000
echo "state=wait qc=0" > $H2/hvdcp_status; echo 1000000 > $G/c/current_max; echo 460 > $G/b/temp; sleep 1.2
exp "Q3 warm during the ramp: lower limit still applied (5 V)" "$(cat $H2/hvdcp_enable)/$(cat $G/c/current_max)" 0/750000
echo 250 > $G/b/temp; sleep 1.2
exp "Q5 monitor: state changes logged" "$([ "$(grep -c 'A6L_CHG_GUARD qc state=' $W/q3.log)" -ge 3 ] && echo ok)" ok
# Q4: pull-up per source + one APSD rerun per plug-in once the pull-up is off and the driver settled at dcp-5v
echo "Unknown [SDP] CDP DCP" > $G/c/usb_type; sleep 1.2
exp "Q4 USB host [SDP] -> pull-up on (adb/MTP)" "$(cat $U2/a800000.usb/soft_connect)" connect
echo 0 > $G/c/online; echo "[Unknown] SDP CDP DCP" > $G/c/usb_type; sleep 1.2
exp "Q7 unplugged -> pull-up untouched (r6c: online=0 [Unknown] misread on the laptop)" "$(cat $U2/a800000.usb/soft_connect)" connect
echo 1 > $H2/hvdcp_enable; echo "state=wait qc=0 apsd=0x01/0x02" > $H2/hvdcp_status
echo connect > $U2/a800000.usb/soft_connect; echo 1 > $G/c/online; echo "Unknown SDP CDP [DCP]" > $G/c/usb_type; sleep 1.2
exp "Q4 [DCP]: pull-up re-asserted off (gadget HAL rebind)" "$(cat $U2/a800000.usb/soft_connect)" disconnect
exp "Q4 no rerun before dcp-5v" "$(cat $H2/hvdcp_rerun)" ""
echo "state=dcp-5v qc=0 apsd=0x01/0x02" > $H2/hvdcp_status; sleep 1.2
exp "Q4 dcp-5v with the pull-up off -> one hvdcp_rerun" "$(cat $H2/hvdcp_rerun)" 1
: > $H2/hvdcp_rerun; sleep 1.2; exp "Q4 at most one rerun per plug-in" "$(cat $H2/hvdcp_rerun)" ""
# Keep the fake unplug visible while the guard is scheduled on a busy build host.
echo 0 > $G/c/online; sleep 1.2; echo 1 > $G/c/online; sleep 1.2
exp "Q4 replug -> rerun allowed again" "$(cat $H2/hvdcp_rerun)" 1
kill $GP 2>/dev/null; wait $GP 2>/dev/null
exp "Q4 guard stopped (SIGTERM) -> pull-up reconnected" "$(cat $U2/a800000.usb/soft_connect)" connect
PULLUP=0 QG > $W/q4.log 2>&1 & GP=$!; sleep 1.2
exp "Q4 persist.vendor.a6l.chg.pullup=0 -> gadget untouched on DCP" "$(cat $U2/a800000.usb/soft_connect)" connect
kill $GP 2>/dev/null; wait $GP 2>/dev/null
# Q6 (r6c): no gadget bound (UDC function empty) -> the guard never writes soft_connect (r6b: -EOPNOTSUPP every evaluation)
echo "Unknown SDP CDP [DCP]" > $G/c/usb_type; : > $U2/a800000.usb/function; echo untouched > $U2/a800000.usb/soft_connect
QG > $W/q6.log 2>&1 & GP=$!; sleep 1.2
exp "Q6 [DCP] without a bound gadget -> soft_connect untouched" "$(cat $U2/a800000.usb/soft_connect)" untouched
echo "Unknown [SDP] CDP DCP" > $G/c/usb_type; sleep 1.2
exp "Q6 [SDP] without a bound gadget -> untouched" "$(cat $U2/a800000.usb/soft_connect)" untouched
echo g1 > $U2/a800000.usb/function; echo owner-connect > $U2/a800000.usb/soft_connect; sleep 1.2
exp "Q6 [SDP], gadget bound by its owner -> guard does not pull up again" "$(cat $U2/a800000.usb/soft_connect)" owner-connect
echo "Unknown SDP CDP [DCP]" > $G/c/usb_type; sleep 1.2
exp "Q6 [DCP], bound -> pull-up off" "$(cat $U2/a800000.usb/soft_connect)" disconnect
: > $U2/a800000.usb/function; echo 0 > $G/c/online; sleep 0.6; echo rebound > $U2/a800000.usb/soft_connect; echo 1 > $G/c/online
echo "Unknown [SDP] CDP DCP" > $G/c/usb_type; echo g1 > $U2/a800000.usb/function; sleep 1.2
exp "Q6 unbound in between: the new binding's pull-up is its owner's (no guard connect)" "$(cat $U2/a800000.usb/soft_connect)" rebound
kill $GP 2>/dev/null; wait $GP 2>/dev/null
exp "Q6 no ERROR, no soft_connect log without a gadget" "$(grep -c ERROR $W/q6.log)/$(grep -c 'soft_connect' $W/q6.log)" 0/1
# Q7 (r6d): the gadget is NEVER disconnected on a host or on an unconfirmed source; only [DCP] + online=1 + no host state
: > $W/q7.log; echo g1 > $U2/a800000.usb/function; echo connect > $U2/a800000.usb/soft_connect; echo configured > $U2/a800000.usb/state
echo 1 > $G/c/online; echo "Unknown [SDP] CDP DCP" > $G/c/usb_type
QG > $W/q7.log 2>&1 & GP=$!; sleep 1.2
q7() { echo "$1" > $G/c/online; echo "$2" > $G/c/usb_type; echo "$3" > $U2/a800000.usb/state; sleep 1.2; cat $U2/a800000.usb/soft_connect; }
exp "Q7 r6c case: adb configured, online=0 [Unknown] -> connected" "$(q7 0 '[Unknown] SDP DCP CDP' configured)" connect
exp "Q7 configured + online=1 [Unknown] -> connected" "$(q7 1 '[Unknown] SDP DCP CDP' configured)" connect
exp "Q7 configured + [DCP] misreport -> connected" "$(q7 1 'Unknown SDP [DCP] CDP' configured)" connect
exp "Q7 addressed + [DCP] -> connected" "$(q7 1 'Unknown SDP [DCP] CDP' addressed)" connect
exp "Q7 suspended host + [DCP] -> connected" "$(q7 1 'Unknown SDP [DCP] CDP' suspended)" connect
exp "Q7 not attached + [SDP] -> connected" "$(q7 1 'Unknown [SDP] CDP DCP' 'not attached')" connect
exp "Q7 not attached + [CDP] -> connected" "$(q7 1 'Unknown SDP [CDP] DCP' 'not attached')" connect
exp "Q7 not attached + online=1 [Unknown] -> connected" "$(q7 1 '[Unknown] SDP DCP CDP' 'not attached')" connect
exp "Q7 not attached + online=0 -> connected" "$(q7 0 '[Unknown] SDP DCP CDP' 'not attached')" connect
exp "Q7 no disconnect logged on a host / unknown source" "$(grep -c 'soft_connect disconnect' $W/q7.log)" 0
exp "Q7 confirmed wall charger (online=1 [DCP], not attached) -> off" "$(q7 1 'Unknown SDP [DCP] CDP' 'not attached')" disconnect
exp "Q7 unplugged after the wall: own disconnect kept (next APSD sees free D+/D-)" "$(q7 0 '[Unknown] SDP DCP CDP' 'not attached')" disconnect
exp "Q7 then a USB host [SDP] -> own disconnect undone" "$(q7 1 'Unknown [SDP] CDP DCP' 'not attached')" connect
q7 1 'Unknown SDP [DCP] CDP' 'not attached' > /dev/null
exp "Q7 wall again, then [Unknown] -> own disconnect undone" "$(q7 1 '[Unknown] SDP DCP CDP' 'not attached')" connect
q7 1 'Unknown SDP [DCP] CDP' 'not attached' > /dev/null
exp "Q7 wall, then a host bus state while unplugged-reported -> reconnected" "$(q7 0 '[Unknown] SDP DCP CDP' default)" connect
kill $GP 2>/dev/null; wait $GP 2>/dev/null
exp "Q7 no ERROR" "$(grep -c ERROR $W/q7.log)" 0
echo "not attached" > $U2/a800000.usb/state
echo "Unknown SDP CDP [DCP]" > $G/c/usb_type
# without the HVDCP parameters (older qcom_smbx, charger not loaded yet) the guard behaves as before
rm -rf $H2; QG > $W/q5.log 2>&1 & GP=$!
exp "Q no hvdcp params: normal DCP unchanged" "$(st 250 3900000)" 2000000/1
exp "Q no hvdcp params: warm unchanged" "$(st 460 3900000)" 750000/1
kill $GP $DP 2>/dev/null; wait 2>/dev/null
exp "Q no ERROR in the QC runs" "$(cat $W/q.log $W/q3.log $W/q5.log | grep -c ERROR)" 0
rm -rf $W
echo "A6L_POWER_SIM $([ $fails = 0 ] && echo PASS || echo FAIL $fails)"; exit $fails
