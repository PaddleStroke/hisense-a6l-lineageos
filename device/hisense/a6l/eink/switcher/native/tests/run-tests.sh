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
$CC -Wall -Wextra -Werror -O1 -Wno-misleading-indentation -o dualux "$S/a6l_dualux.c" "$S/dualux_logic.c" -lm && ok build-host || { ko build-host; exit 1; }
python3 "$S/tests/e2e.py" "$W" ./dualux && ok e2e || ko e2e
echo "RESULT pass=$pass fail=$fail"; [ $fail = 0 ] && echo DUALUX_TESTS_PASS || echo DUALUX_TESTS_FAIL
