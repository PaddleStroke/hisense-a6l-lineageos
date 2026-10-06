#!/usr/bin/env bash
# Host test (WSL; bug hunt round2 install-tools, 29 Sep 2026): Prepare-RomV2Boot.py refuses A6L_FSTAB=fbe with a kernel whose
# config lacks CONFIG_FS_ENCRYPTION=y (V67) before creating anything; r5 passes the guard (then stops at the missing test DTB).
# Nothing is built (A6L_V75_DTB points to a missing file). usage: bash tools/tests/test-rom-boot-fbe-guard.sh [Prepare-RomV2Boot.py]
set -u
HERE=$(cd "$(dirname "$0")/.." && pwd); P=${1:-$HERE/Prepare-RomV2Boot.py}
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT; fail=0
out=$(A6L_FSTAB=fbe A6L_V75_DTB=$T/missing.dtb timeout 60 python3 $P $T/v67-fbe 2>&1); rc=$?
if [ $rc != 0 ] && grep -q ROM_V2_BOOT_REFUSED <<<"$out" && [ ! -e $T/v67-fbe ]; then echo "PASS v67_fbe_refused"; else echo "FAIL v67_fbe_refused rc=$rc dir=$([ -e $T/v67-fbe ] && echo created) $(tail -1 <<<"$out")"; fail=1; fi
out=$(A6L_KERNEL=r5 A6L_FSTAB=fbe A6L_V75_DTB=$T/missing.dtb timeout 60 python3 $P $T/r5-fbe 2>&1)
grep -q ROM_V2_BOOT_REFUSED <<<"$out" && { echo "FAIL r5_fbe_passes_guard"; fail=1; } || echo "PASS r5_fbe_passes_guard"
out=$(A6L_V75_DTB=$T/missing.dtb timeout 60 python3 $P $T/v67-default 2>&1)
grep -q ROM_V2_BOOT_REFUSED <<<"$out" && { echo "FAIL v67_default_passes_guard"; fail=1; } || echo "PASS v67_default_passes_guard"
echo "ROM_BOOT_FBE_GUARD_TEST $([ $fail = 0 ] && echo PASS || echo FAIL)"; exit $fail
