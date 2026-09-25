#!/system/bin/sh
# Hisense A6L radio (agent ril): attended QMI test steps for the V74 recovery RAM session.
# Precondition: the modem runs through v74/radio2 started with A6L_KEEP=1 (rmtfs/tqftpserv/diag-router alive).
# usage: sh /tmp/ril/ril-test.sh <step> [args]
#   ro                      read-only: services, IDs, SIM (ICCID, masked IMSI), operating mode, registration, 60 s of indications
#   online                  RF: modem operating mode ONLINE (network registration), then 90 s of indications
#   sms-listen [s]          print incoming SMS for s seconds (default 180) and ack them
#   sms-send <+num> <text>  RF: send one SMS (A6L_SMS_TO must equal <+num>)
#   dial <num> [s]          RF: voice call to <num>, hang up after s seconds (A6L_DIAL_TO must equal <num>); no audio
#   call-wait [s]           print incoming calls (A6L_ANSWER=1 answers), hang up everything at the end
#   call-list               list the modem's calls (id/state/direction/number)
#   dtmf <digits> [wait]    DTMF on the call in conversation (run in a 2nd shell during dial/call-wait)
#   dial-dtmf <num> <digits> [s]  RF: dial, wait for answer, send the digits, hang up after s (40)
#   data [apn] [v4|v6|v4v6] RF: start a data call (no IPA netdev: modem-only), print IP/GW/DNS/MTU, stop
#   lowpower                modem operating mode LOW POWER (stops transmitting, keeps QMI up)
#   stop                    low power, stop the modem remoteproc and the radio2 daemons
export PATH=/tmp/bin:$PATH
D=${D:-/tmp/ril}
Q="$D/a6l-qmi"
L=${L:-/tmp/ril-logs}; mkdir -p $L
step=$1; shift 2>/dev/null
ts=$(date +%H%M%S 2>/dev/null || echo now)
run() { echo "A6L_RIL_STEP $step: $*"; "$Q" "$@" 2>&1 | tee -a $L/$step-$ts.txt; }
rf() { [ "$A6L_RF_APPROVED" = 1 ] || { echo "A6L_RIL_REFUSED $step needs A6L_RF_APPROVED=1 (Pierre's go)"; exit 3; }; }
[ -x "$Q" ] || chmod 755 "$Q"
case "$step" in
ro)
    run lookup; run info; run sim; run mode get; run reg; run watch 60 ;;
online)
    rf; run mode online; run watch 90; run reg ;;
sms-listen)
    run sms-listen ${1:-180} ;;
sms-send)
    rf; run sms-send "$1" "$2" ;;
dial)
    rf; run dial "$1" ${2:-20} ;;
call-wait)
    run call-wait ${1:-60} ;;
call-list)
    run call-list ;;
dtmf)
    run dtmf "$1" ${2:-30} ;;
dial-dtmf)
    rf; [ "$A6L_DIAL_TO" = "$1" ] || { echo "A6L_RIL_REFUSED set A6L_DIAL_TO=$1"; exit 3; }
    "$Q" dial "$1" ${3:-40} > $L/dial-dtmf-dial-$ts.txt 2>&1 &
    dp=$!
    run dtmf "$2" 35
    wait $dp; cat $L/dial-dtmf-dial-$ts.txt ;;
data)
    rf; run data "${1:-}" ${2:-v4v6} 10 ;;
lowpower)
    run mode lowpower ;;
stop)
    "$Q" mode lowpower 2>&1 | tee -a $L/stop-$ts.txt
    for r in /sys/class/remoteproc/remoteproc*; do
        case "$(cat $r/name 2>/dev/null)" in *4080000*|mss|modem) [ "$(cat $r/state)" = running ] && echo stop > $r/state; echo "A6L_RIL_STOP $r $(cat $r/state)";; esac
    done
    sleep 2
    for p in rmtfs tqftpserv diag-router; do
        ps -A -o PID,ARGS | grep "bin/$p" | grep -v grep | while read pid rest; do kill $pid && echo "A6L_RIL_KILLED $p $pid"; done
    done
    echo "A6L_RIL_STOPPED" ;;
*)
    sed -n '2,18p' "$0"; exit 1 ;;
esac
echo "A6L_RIL_DONE $step (log $L)"
