#!/usr/bin/env bash
# Activate the r6 preparation (completeness audit, 29 Sep 2026) in a device/hisense/a6l tree. Idempotent.
# usage: bash rom/r6/apply-r6.sh [--apply] [--defaults] [<device/hisense/a6l dir>]
#   (no --apply)  print what would change (dry run)
#   --apply       1. rom.mk: inherit rom/r6/r6.mk (gatekeeper, thermal HAL, overlay, suspend tool)
#                 2. BoardConfig-rom.mk: rom/r6/sepolicy/vendor always in A6L_SEPOLICY_DIRS (thermal exec label, UDC genfs)
#   --defaults    3. also apply patches/0002-r6-charger-default.patch (persist.vendor.a6l.charger=1) - PIERRE's decision
# Run it on the Windows repo by the merge worker BEFORE the next pipeline prep (never while a build is running).
set -euo pipefail
APPLY=0; DEF=0; T=
for a in "$@"; do case "$a" in --apply) APPLY=1;; --defaults) DEF=1;; *) T=$a;; esac; done
T=${T:-$(cd "$(dirname "$0")/../.." && pwd)}; R=$T/rom; R6=$R/r6
[ -f $R/rom.mk ] && [ -f $R6/r6.mk ] || { echo "not a device/hisense/a6l tree: $T"; exit 1; }
INC='$(call inherit-product, device/hisense/a6l/rom/r6/r6.mk)'
SEP='A6L_SEPOLICY_DIRS += rom/r6/sepolicy/vendor'
do_() { if [ $APPLY = 1 ]; then eval "$1"; echo "done: $2"; else echo "would: $2"; fi; }
if tr -d '\r' < $R/rom.mk | grep -qxF "$INC"; then echo "ok: rom.mk already inherits r6.mk"
else do_ "printf '\n# r6 (completeness audit 29 Sep 2026; docs/completeness-audit-20260929.md)\n%s\n' '$INC' >> $R/rom.mk" "rom.mk += inherit r6.mk"; fi
if tr -d '\r' < $R/BoardConfig-rom.mk | grep -qxF "$SEP"; then echo "ok: BoardConfig-rom.mk already has r6 sepolicy"
else do_ "sed -i '/^A6L_SEPOLICY_DIRS ?=/a $SEP' $R/BoardConfig-rom.mk" "BoardConfig-rom.mk += $SEP"; fi
if [ $DEF = 1 ]; then
  if tr -d '\r' < $R/rom.mk | grep -q '^    persist.vendor.a6l.charger=1 \\$'; then echo "ok: charger default already 1"
  else do_ "sed -i 's/^    persist.vendor.a6l.charger=0 \\\\\\(\r\\?\\)\$/    persist.vendor.a6l.charger=1 \\\\\\1/' $R/rom.mk" "rom.mk persist.vendor.a6l.charger=0 -> 1"; fi
fi
[ $APPLY = 1 ] || echo "dry run: pass --apply to change the tree"
