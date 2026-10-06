#!/bin/bash
# ATTENDED ONLY (main agent, with Pierre's go: enables pm660l L3 at the DT's 3.0 V). Copy to .relay/inbox/<new name>.sh.
exec < /dev/null
cd /mnt/c/Users/Pierre/Desktop/A6L/.relay
./lap.sh 40 'cd ~/A6L-usb-20260915/v75/stk && sha256sum -c SHA256SUMS | tail -3 && adb -s HLTE730T-PROBE push . /tmp/stk 2>&1 | tail -1'
./ph.sh 50 'export PATH=/tmp/bin:$PATH; export D=/tmp/stk MODE=probe; sh /tmp/stk/run-stk.sh 2>&1 | tail -40' | cut -c1-200
