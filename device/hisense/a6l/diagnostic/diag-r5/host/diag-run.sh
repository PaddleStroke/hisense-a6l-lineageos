#!/bin/bash
# diag-r5 LAPTOP: start one kit step on the phone DETACHED (setsid, no tty, output -> /tmp/diag/log.txt) so adb returns at once
# and the step keeps running even if adb or the stream drops. Env for the phone side: DIAG_ENV="A6L_DIAG_PAUSE=3 ...".
# usage: host/diag-run.sh <step> [args]     e.g.  host/diag-run.sh pre ; host/diag-run.sh msm skip_gpu=1
S=${ADB_SERIAL:-HLTE730T-PROBE}
[ $# -ge 1 ] || { echo "usage: $0 <step> [args]"; exit 1; }
adb -s "$S" shell "cd /tmp/diag && ${DIAG_ENV:-} /system/bin/toybox setsid /system/bin/sh ./d.sh $* </dev/null >>/tmp/diag/log.txt 2>&1 &" >/dev/null 2>&1
sleep 1; echo "started: d.sh $* (watch the stream; log on phone: /tmp/diag/log.txt)"
