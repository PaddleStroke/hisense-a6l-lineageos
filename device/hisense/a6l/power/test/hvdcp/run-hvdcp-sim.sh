#!/usr/bin/env bash
# A6L power28: host register-level test of the HVDCP patch. usage: run-hvdcp-sim.sh <kernel qcom_smbx.c (unpatched)> <smbx patch dir>
# Builds the r4 driver (fcc-jeita) and r4+hvdcp with ASan/UBSan, runs all scenarios, then checks that with hvdcp_enable=0
# the register write trace is identical to the r4 driver (-> A6L_HVDCP_OFF_TRACE SAME).
set -e; S=$(cd "$(dirname "$0")" && pwd); SRC=$1; P=${2:-$S/../../../kernel/power/smbx}; W=$(mktemp -d)
cp "$SRC" $W/old.c; tr -d '\r' < $P/qcom_smbx-a6l-fcc-jeita.patch | patch -s -p4 --no-backup-if-mismatch $W/old.c
cp $W/old.c $W/new.c; tr -d '\r' < $P/qcom_smbx-a6l-hvdcp.patch | patch -s -p4 --no-backup-if-mismatch $W/new.c
CF="-g -O1 -Wall -Wno-unused-function -Wno-unused-variable -Wno-unused-but-set-variable -Wno-pointer-sign -fsanitize=address,undefined -fno-sanitize-recover=all -I$S"
gcc $CF -DQCOM_SMBX_C="\"$W/new.c\"" -o $W/sim-new $S/sim-smb2.c
gcc $CF -DA6L_OLD_DRIVER -DQCOM_SMBX_C="\"$W/old.c\"" -o $W/sim-old $S/sim-smb2.c
export ASAN_OPTIONS=detect_leaks=0; rc=0; $W/sim-new || rc=1
$W/sim-old trace-off > $W/t-old; $W/sim-new trace-off > $W/t-new
if cmp -s $W/t-old $W/t-new; then echo "A6L_HVDCP_OFF_TRACE SAME ($(wc -l < $W/t-new) register writes)"; else echo "A6L_HVDCP_OFF_TRACE DIFF"; diff $W/t-old $W/t-new | head -20; rc=1; fi
rm -rf $W; exit $rc
