#!/usr/bin/env bash
# Host test for diagnostic/a6l_mmio.c argument validation (r5 bug hunt round2 kernel-drivers, 29 Sep 2026).
# Run as a NON-ROOT user (refuses root): a pre-fix binary that accepts a bad argument reaches open("/dev/mem"), which
# then fails with rc 1 instead of the expected rc 2/3 - nothing is ever mapped. usage: test-a6l-mmio.sh [source.c]
set -uo pipefail
[ "$(id -u)" != 0 ] || { echo "refusing to run as root"; exit 2; }
D=$(cd "$(dirname "$0")/.." && pwd); SRC=${1:-$D/a6l_mmio.c}; T=$(mktemp -d); trap 'rm -rf $T' EXIT
cc -O2 -Wall -Wextra -Werror -o $T/mmio "$SRC" || { echo "BUILD FAIL"; exit 1; }
pass=0; fail=0
t() { # expected_rc description args...
  local want=$1 d=$2; shift 2; local out rc
  out=$(A6L_MMIO_WRITE=1 A6L_MMIO_CHECK_ONLY=1 $T/mmio "$@" 2>&1); rc=$?
  if [ $rc = $want ]; then pass=$((pass+1)); else fail=$((fail+1)); echo "FAIL $d: rc=$rc want=$want ($out)"; fi; }
t 2 "write value typo (0xzz)"            w 0c8c1000 0xzz
t 2 "write value non-hex (zz)"           w 0c8c1000 zz
t 2 "write value empty"                  w 0c8c1000 ""
t 2 "write value > 32 bits"              w 0c8c1000 1ffffffff
t 2 "write value trailing junk"          w 0c8c1000 12g
t 2 "write extra argument"               w 0c8c1000 1 2
t 2 "address typo"                       r 0c8c10x0
t 2 "count junk"                         r 0c8c1000 4x
t 2 "count negative"                     r 0c8c1000 -1
t 3 "addr+4n wraps (huge addr)"          r fffffffffffffffc 1
t 3 "addr+4n past window end"            r 0c8fff00 128
t 3 "read past GCC window"               r 00193ffc 2
t 3 "write into read-only MDSS"          w 0c900100 1
t 2 "mode typo"                          x 0c8c1000
t 0 "valid MMCC read"                    r 0c8c1000 4
t 0 "valid last MMCC word"               r 0c8ffffc 1
t 0 "valid MMCC write"                   w 0c8c1000 80000000
t 0 "valid clamp write"                  w 0c828014 3
echo "a6l_mmio tests: pass=$pass fail=$fail"; [ $fail = 0 ]
