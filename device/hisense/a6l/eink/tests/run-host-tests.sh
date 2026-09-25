#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# eink3 offline tests on a Linux host (no phone, no QEMU): unit tests of the policy / key / touch logic, then a6l_epdd
# (host build, fake libtcon) + a6l_eink_mirror (host build) end to end over the control socket, then key + rear-touch
# events through FIFOs. Needs cc/clang, libdrm headers (libdrm-dev), python3 + numpy. usage: run-host-tests.sh [WORKDIR]
set -uo pipefail
S=$(cd "$(dirname "$0")/.." && pwd); W=${1:-$(mktemp -d)}; mkdir -p "$W"; cd "$W"
CC=${CC:-cc}; DRM=$(pkg-config --cflags libdrm 2>/dev/null || echo -I/usr/include/libdrm); pass=0; fail=0
ok() { echo "PASS $*"; pass=$((pass+1)); }; ko() { echo "FAIL $*"; fail=$((fail+1)); }
$CC -Wall -Wextra -O1 -o t_logic "$S/tests/test_eink_logic.c" "$S/src/eink_logic.c" && ./t_logic | tail -1 | grep -q TESTS_PASS && ok unit-logic || ko unit-logic
$CC -shared -fPIC -o libfake_tcon.so "$S/tests/fake_tcon.c" || ko fake-lib
$CC -O1 -Wall -Wextra -Wno-misleading-indentation $DRM -o epdd "$S/src/a6l_epdd.c" -ldrm -ldl && ok build-epdd || ko build-epdd
$CC -O1 -Wall -Wextra -Wno-misleading-indentation $DRM -o mirror "$S/src/a6l_eink_mirror.c" "$S/src/eink_logic.c" -ldrm && ok build-mirror || ko build-mirror
python3 "$S/tests/gen_frames.py" frames >/dev/null && ok frames || ko frames
head -c $((0x70080)) /dev/urandom > wf.bin; head -c $((0x70080)) /dev/zero | tr '\0' '\377' > erased.bin
rm -rf e2e; mkdir e2e
./epdd --dry e2e/u --lib ./libfake_tcon.so --waveform /nonexistent --waveform erased.bin --waveform wf.bin --listen e2e/sock > e2e/epdd.log 2>&1 &
EP=$!; sleep 1
timeout 40 ./mirror --epd-socket e2e/sock --source 'files:frames/f%d.raw' --no-props --mode mirror --key-dev none --touch-dev none --frames 56 --clear-every 3 > e2e/mirror.log 2>&1
python3 - <<'PY' > e2e/client.log 2>&1
import socket
s=socket.socket(socket.AF_UNIX); s.connect("e2e/sock"); f=s.makefile("rwb")
for c in [b"ping\n", b"status\n", b"frame 720 1440 reading\n"+bytes([128])*(720*1440), b"mode fast\n", b"quit\n"]:
    f.write(c); f.flush(); print(c[:24], "->", f.readline().decode().strip())
PY
wait $EP
grep -q "waveform from erased.bin rejected" e2e/epdd.log && grep -q "waveform: wf.bin" e2e/epdd.log && ok waveform-fallback-order || ko waveform-fallback-order
grep -q "cmd: clear" e2e/mirror.log && ok mirror-enter-clear || ko mirror-enter-clear
q=$(grep -c "cmd: frame 720 1440 quality" e2e/mirror.log); f=$(grep -c "cmd: frame 720 1440 fastest" e2e/mirror.log)
[ "$q" -ge 2 ] && [ "$f" -ge 5 ] && ok "mirror-policy quality=$q fastest=$f" || ko "mirror-policy quality=$q fastest=$f"
grep -q "cmd: refresh\|cmd: frame 720 1440 quality" e2e/mirror.log && ok mirror-settle || ko mirror-settle
grep -q "OK pong" e2e/client.log && grep -q "OK updates=" e2e/client.log && grep -q "reading.*OK shown" e2e/client.log && ok socket-protocol || ko socket-protocol
n=$(grep -c "epdd: OK" e2e/mirror.log); r=$(grep -c "cmd:" e2e/mirror.log); [ "$n" = "$r" ] && [ "$r" -gt 0 ] && ok "every command answered OK ($r cmds, $n replies)" || ko "unanswered commands ($r cmds, $n OK replies)"
rm -rf io; mkdir io; mkfifo io/key io/touch
timeout 25 ./mirror --dry --source 'files:frames/f%d.raw' --no-props --key-dev io/key --touch-dev io/touch --touch-debug > io/mirror.log 2>&1 &
MP=$!; sleep 0.5; python3 "$S/tests/input_test.py" io/key io/touch > io/driver.log 2>&1; sleep 0.5; kill $MP 2>/dev/null; wait $MP 2>/dev/null
L=io/mirror.log
grep -q "e-ink key: mirror" $L && grep -q "e-ink key long press: clear" $L && grep -q "e-ink key: off" $L && ok key-state-machine || ko key-state-machine
grep -q "TOUCH_OUT 3 53 541" $L && grep -q "TOUCH_OUT 3 54 1170" $L && ok touch-centre-mapping || ko touch-centre-mapping
grep -q "TOUCH_OUT 3 53 0" $L && grep -q "TOUCH_OUT 3 54 0" $L && ok touch-corner-mapping || ko touch-corner-mapping
[ "$(grep -c 'TOUCH_OUT 3 57 [0-9]' $L)" = 1 ] && ok "touch: only the in-picture contact forwarded (off/bar/after-off dropped)" || ko "touch forwarding count $(grep -c 'TOUCH_OUT 3 57 [0-9]' $L)"
echo "EINK3_HOST_TESTS pass=$pass fail=$fail (workdir $W)"
[ $fail = 0 ]
