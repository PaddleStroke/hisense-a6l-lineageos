#!/bin/sh
# Host tests for sensors.a6l (agent senshal). usage: sh run-tests.sh [workdir]   (gcc + python3; < 60 s)
set -u
H=$(cd "$(dirname "$0")/.." && pwd)
W=${1:-${TMPDIR:-/tmp}/senshal-t}
mkdir -p "$W" && rm -rf "$W"/*
CF="-std=gnu11 -O1 -g -DA6L_HOST_TEST -Wall -Wextra -Werror -Wno-unused-parameter -Wno-format-truncation -I$H/tests/include -I$H"
SAN="${SAN:--fsanitize=address,undefined -fno-omit-frame-pointer}"
gcc $CF $SAN -o "$W/t" "$H/tests/test_sensors_a6l.c" "$H/sensors_a6l.c" "$H/a6l_motion.c" "$H/a6l_magcal.c" -lm -lpthread || { echo A6L_TESTS_BUILD_FAIL; exit 1; }
tot=0; bad=0
run() { # case fakeargs... ; env via PROPS
    c=$1; shift; r="$W/$c"; mkdir -p "$r"; secs=1
    case " $* " in *" --secs "*) secs=$(echo " $* " | sed 's/.* --secs \([0-9.]*\).*/\1/');; esac
    [ "$1" = none ] || python3 "$H/tests/mkfake_hal.py" "$r" "$@" > /dev/null || { echo "A6L_T_FAIL $c fake"; bad=$((bad+1)); return; }
    env A6L_T_ROOT="$r" A6L_T_SECS=$secs A6L_SYSROOT="$r/sys" A6L_DEVROOT="$r/dev" A6L_DATADIR="${DATADIR:-$r/data}" $PROPS timeout 50 "$W/t" "$c" > "$r/out.txt" 2> "$r/log.txt"
    rc=$?; tot=$((tot+1)); grep -E 'A6L_T_(PASS|FAIL)' "$r/out.txt"; [ $rc = 0 ] || { bad=$((bad+1)); echo "  ($c rc=$rc, log: $r/log.txt)"; tail -3 "$r/log.txt"; }
}
PROPS=""
run fit none
run list flat --stk --secs 1
run flat flat --secs 6
run rotate rotate --secs 10
DATADIR="$W/rotate/data" run reload rotate --secs 2
run flush flat --secs 4
run missing flat --no-mag --secs 2
run light flat --stk --secs 3
PROPS="A6L_PROP_persist_vendor_a6l_sensors_map_accel=+x+y+z" run map flat --secs 2
PROPS="A6L_PROP_persist_vendor_a6l_sensors_mag_unit=ut" run unit flat --secs 2
PROPS="A6L_PROP_persist_vendor_a6l_sensors_motion=0" run motionoff flat --secs 1
# r5 review F4 (29 Sep 2026): recovery without a second Android activate
run readerr flat --secs 8
run late flat --secs 8
run proxfd flat --stk --secs 1
# r5 review round6 F51 / F4 (29 Sep 2026): PS interrupt requested-vs-applied retry; IIO unregister/re-register
run psint flat --stk --secs 1
run unreg flat --stk --secs 1
# r5 bug hunt sensors-vib-wifi-bt (29 Sep 2026): STK3338 probing after the HAL opened; disable while the device is lost
run latestk flat --secs 1
run lostoff flat --stk --secs 1
# r5 deep review F30/F31/F32 (29 Sep 2026)
run backlog none
run queue none
run gyrocal none
run slowyaw slowyaw --secs 6
echo "A6L_SENSHAL_TESTS cases=$tot failed=$bad"
[ $bad = 0 ] && echo A6L_SENSHAL_TESTS_PASS || echo A6L_SENSHAL_TESTS_FAIL
