#!/bin/bash
# ATTENDED ONLY, after T1 showed an answering address. Pierre covers/uncovers the front sensor when the log says so
# (~8 s and ~18 s after start). Copy to .relay/inbox/<new name>.sh.
exec < /dev/null
cd /mnt/c/Users/Pierre/Desktop/A6L/.relay
./ph.sh 55 'export PATH=/tmp/bin:$PATH; export D=/tmp/stk MODE=data; sh /tmp/stk/run-stk.sh 2>&1 | tail -60' | cut -c1-200
