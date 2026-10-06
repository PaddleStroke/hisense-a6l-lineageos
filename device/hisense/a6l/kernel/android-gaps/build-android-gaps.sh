#!/usr/bin/env bash
# Build the A6L "kernel gaps" out-of-tree modules (xt_quota2, uid_sys_stats, dm-default-key) against the V67 and r5
# kernels, W=1, clang r584948, and collect them into firmware/extracted/kernel-gaps-20260929/{v67,r5}/ + SHA256SUMS.
# dm-default-key is r5 only (V67 has no BLK_INLINE_ENCRYPTION). Offline, WSL. docs/kernel-gaps-20260929.md
# usage: build-android-gaps.sh [outdir]   (default: <repo>/firmware/extracted/kernel-gaps-20260929)
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd); REPO=$(cd "$HERE/../../../../.." && pwd)
OUT=${1:-$REPO/firmware/extracted/kernel-gaps-20260929}
W=${A6L_GAPS_WORK:-/home/a6l/kernel/android-gaps-build}
export PATH=/home/a6l/android/a6l-lineage24/prebuilts/clang/host/linux-x86/clang-r584948/bin:$PATH
export KBUILD_BUILD_USER=a6l KBUILD_BUILD_HOST=a6l-build KBUILD_BUILD_TIMESTAMP='2026-09-29 00:00:00 UTC'
declare -A KSRC=([v67]=/home/a6l/kernel/a6l-baseline-7.2 [r5]=/home/a6l/kernel/a6l-rom-r5-src)
declare -A KOUT=([v67]=/home/a6l/kernel/out-a6l-phone-v67 [r5]=/home/a6l/kernel/out-a6l-rom-r5)
declare -A MODS=([v67]="xt_quota2 uid_sys_stats" [r5]="xt_quota2 uid_sys_stats dm-default-key")
fail=0; rm -rf "$W"; mkdir -p "$OUT"
for k in v67 r5; do
  mkdir -p "$OUT/$k"
  for m in ${MODS[$k]}; do
    b=$W/$k/$m; mkdir -p "$b"; cp -a "$HERE/$m/." "$b/"
    if make -C "${KSRC[$k]}" O="${KOUT[$k]}" ARCH=arm64 LLVM=1 W=1 -j8 M="$b" modules > "$b.log" 2>&1; then
      nw=$(grep -c -E 'warning:' "$b.log" || true); echo "built $k/$m warnings=$nw"; [ "$nw" = 0 ] || { grep -E 'warning:' "$b.log" | head; fail=1; }
      cp "$b/$m.ko" "$OUT/$k/$m.ko"; cp "$b.log" "$OUT/$k/$m.build.log"
    else echo "FAIL $k/$m"; grep -a -E 'error|Error' "$b.log" | head -20; fail=1; fi
  done
  cp "${KOUT[$k]}/Module.symvers" "$W/$k.Module.symvers"
done
( cd "$OUT" && find v67 r5 -name '*.ko' | sort | xargs sha256sum > SHA256SUMS )
cp "$0" "$OUT/build-android-gaps.sh"
echo "A6L_GAPS_BUILD fail=$fail"; exit $fail
