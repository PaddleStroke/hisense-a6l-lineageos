#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# a6l_dualux offline tests (agent dualux): 1) unit tests of dualux_logic.c, 2) the real daemon (host build) end to end
# against a fake sysfs tree + property files + input-event FIFOs (no phone, no Android). usage: run-tests.sh [WORKDIR]
set -uo pipefail
S=$(cd "$(dirname "$0")/.." && pwd); W=${1:-$(mktemp -d)}; mkdir -p "$W"; cd "$W"
CC=${CC:-cc}; pass=0; fail=0
ok() { echo "PASS $*"; pass=$((pass+1)); }; ko() { echo "FAIL $*"; fail=$((fail+1)); }
$CC -Wall -Wextra -O1 -o t_dx "$S/tests/test_dualux_logic.c" "$S/dualux_logic.c" -lm && ./t_dx | tail -2 | grep -q TESTS_PASS && ok "unit $(./t_dx | tail -2 | head -1)" || ko unit
E=$S/../../src
$CC -Wall -Wextra -O1 -o t_rf "$S/tests/test_refresh_modes.c" "$E/eink_logic.c" && ./t_rf | grep -q REFRESH_TESTS_PASS && ok refresh-modes || ko refresh-modes
$CC -Wall -Wextra -O1 -o t_el "$S/../../tests/test_eink_logic.c" "$E/eink_logic.c" && ./t_el | tail -1 | grep -q TESTS_PASS && ok "eink_logic regression" || ko "eink_logic regression"
$CC -O1 -Wall -Wno-misleading-indentation -DNO_DRM -o mirror_nodrm "$E/a6l_eink_mirror.c" "$E/eink_logic.c" 2>/dev/null && ok "mirror builds (NO_DRM)" || ko "mirror build"
# r5 review F50: the unchanged enforce_backlight() with controlled bl_power reads/writes (failed unblank is retried)
python3 - "$S/a6l_dualux.c" backlight_method.inc <<'PY'
import sys
s=open(sys.argv[1]).read(); i=s.index('static void enforce_backlight(void)'); j=s.index('{',i); d=0
for k in range(j,len(s)):
    d+=s[k]=='{'; d-=s[k]=='}'
    if d==0: break
open(sys.argv[2],'w').write(s[i:k+1]+'\n')
PY
$CC -Wall -Wextra -O1 -Wno-misleading-indentation -I. -I"$S" -o t_bl "$S/tests/test_backlight_retry.c" "$S/dualux_logic.c" -lm && ./t_bl | tail -1 | grep -q BACKLIGHT_RETRY_TESTS_PASS && ok "backlight unblank retry (r5 F50)" || { ./t_bl 2>&1 | grep FAIL; ko "backlight unblank retry"; }
# r5 review F63: the unchanged enforce_frontlight() with a controlled backend (recreated node re-applied, desired level wins)
python3 - "$S/a6l_dualux.c" frontlight_method.inc <<'PY2'
import sys
s=open(sys.argv[1]).read(); i=s.index('static void enforce_frontlight(void)'); j=s.index('{',i); d=0
for k in range(j,len(s)):
    d+=s[k]=='{'; d-=s[k]=='}'
    if d==0: break
open(sys.argv[2],'w').write(s[i:k+1]+'\n')
PY2
$CC -Wall -Wextra -O1 -Wno-misleading-indentation -I. -I"$S" -o t_fl "$S/tests/test_frontlight_recover.c" "$S/dualux_logic.c" -lm && ./t_fl | tail -1 | grep -q FRONTLIGHT_RECOVER_TESTS_PASS && ok "frontlight backend recovery (r5 F63)" || { ./t_fl 2>&1 | grep FAIL; ko "frontlight backend recovery"; }
$CC -Wall -Wextra -Werror -O1 -Wno-misleading-indentation -o dualux "$S/a6l_dualux.c" "$S/dualux_logic.c" -lm && ok build-host || { ko build-host; exit 1; }
python3 "$S/tests/e2e.py" "$W" ./dualux && ok e2e || ko e2e
python3 "$S/tests/e2e_failopen.py" "$W" ./dualux && ok "e2e fail-open power key (r5 pass2 F20)" || ko "e2e fail-open"
python3 "$S/tests/e2e_exit_asleep.py" "$W" ./dualux && ok "e2e exit while asleep keeps the backlight off (r5 bug hunt E3)" || ko "e2e exit while asleep"
python3 "$S/tests/e2e_appearance.py" "$W" "$W/dualux" && ok "pre-light appearance tokens, first frame, timeout and sleep" || ko "appearance handshake"
python3 "$S/tests/e2e_reader.py" "$W" "$W/dualux" > e2e_reader.log 2>&1 && ok "reader sleep: idle sleep, volume-key wake + page turn, other wake-ups, keyguard gate (eink-round11)" || { tail -5 e2e_reader.log; ko "reader sleep"; }
python3 "$S/tests/e2e_watchdog.py" "$W" "$W/dualux" && ok "watchdog survives a suspend, kills a hang; e-ink restored after an unexpected restart (eink-round3 0016)" || ko "watchdog / restore"
# eink-lockscreen: lock screen logic (renderer, schedule, state machine) + a6l_einklock end to end (fake and real a6l_epdd)
$CC -Wall -Wextra -O1 -Wno-misleading-indentation -o t_elk "$S/tests/test_einklock_logic.c" "$S/einklock_logic.c" -lm && ./t_elk | tail -1 | grep -q EINKLOCK_TESTS_PASS && ok "einklock unit $(./t_elk | tail -2 | head -1)" || ko "einklock unit"
DRM=$(pkg-config --cflags libdrm 2>/dev/null || echo -I/usr/include/libdrm)
$CC -Wall -Wextra -Werror -O1 -Wno-misleading-indentation -o einklock "$S/a6l_einklock.c" "$S/einklock_logic.c" -lm && \
$CC -O1 -Wall -Wno-misleading-indentation $DRM -o epdd_host "$E/a6l_epdd.c" -ldrm -ldl 2>/dev/null && $CC -shared -fPIC -o libfake_tcon.so "$S/../../tests/fake_tcon.c" && \
python3 "$S/tests/e2e_einklock.py" "$W" "$W/einklock" "$W/epdd_host" "$W/libfake_tcon.so" > e2e_einklock.log 2>&1 && ok "e2e lock screen: entry, ticks, redraw, restore, LCD/disabled, real a6l_epdd" || { grep FAIL e2e_einklock.log; ko "e2e lock screen"; }
echo "RESULT pass=$pass fail=$fail"; [ $fail = 0 ] && echo DUALUX_TESTS_PASS || echo DUALUX_TESTS_FAIL
