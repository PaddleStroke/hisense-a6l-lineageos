#!/usr/bin/env bash
# Merge a6l-release.config onto the r5 Android config and check every value landed; optionally build it and compare the
# module ABI (Module.symvers CRCs) with r5. WSL, offline. docs/kernel-gaps-20260929.md
# usage: build-release-config.sh [--build]     O=${A6L_RELEASE_O:-/home/a6l/kernel/out-a6l-rom-r5-release}
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd); REPO=$(cd "$HERE/../../../../.." && pwd)
K=/home/a6l/kernel/a6l-rom-r5-src; R5=$REPO/firmware/extracted/kernel-r5-20260930; O=${A6L_RELEASE_O:-/home/a6l/kernel/out-a6l-rom-r5-release}
export PATH=/home/a6l/android/a6l-lineage24/prebuilts/clang/host/linux-x86/clang-r584948/bin:$PATH
export LOCALVERSION=+ KBUILD_BUILD_USER=a6l KBUILD_BUILD_HOST=a6l-build KBUILD_BUILD_TIMESTAMP='2026-09-30 00:00:00 UTC'
mkdir -p "$O"; cp "$R5/config" "$O/.config.r5"
( cd "$K" && ARCH=arm64 LLVM=1 KCONFIG_CONFIG="$O/.config" scripts/kconfig/merge_config.sh -m -O "$O" "$O/.config.r5" "$HERE/a6l-release.config" ) > "$O/merge.log" 2>&1
make -C "$K" O="$O" ARCH=arm64 LLVM=1 olddefconfig >> "$O/merge.log" 2>&1
bad=0
while read -r l; do
  case "$l" in "# CONFIG_"*" is not set") o=${l#\# }; o=${o%% *}; grep -q "^$o=" "$O/.config" && { echo "NOT UNSET $o"; bad=1; } ;;
               CONFIG_*=*) grep -qx "$l" "$O/.config" || { echo "NOT SET $l (got: $(grep "^${l%%=*}=" "$O/.config" || echo unset))"; bad=1; } ;; esac
done < "$HERE/a6l-release.config"
# unchanged Android requirements / r5 choices that must survive the merge
for o in MODVERSIONS=y SECURITY_SELINUX=y STATIC_USERMODEHELPER=y HARDENED_USERCOPY=y BLK_INLINE_ENCRYPTION_FALLBACK=y FS_ENCRYPTION=y \
         NETFILTER_XT_MATCH_QUOTA=y TASK_IO_ACCOUNTING=y TASK_XACCT=y TRACEPOINTS=y CFI=y SHADOW_CALL_STACK=y \
         THERMAL_EMERGENCY_POWEROFF_DELAY_MS=100; do  # thermal: r5 config now carries a6l-thermal.config
  grep -qx "CONFIG_$o" "$O/.config" || { echo "LOST CONFIG_$o"; bad=1; }; done
( cd "$K" && scripts/diffconfig "$O/.config.r5" "$O/.config" ) > "$O/config-diff-r5-release.txt" || true
echo "diff vs r5: $(wc -l < "$O/config-diff-r5-release.txt") lines"; cat "$O/config-diff-r5-release.txt"
[ $bad = 0 ] && echo "A6L_RELEASE_CONFIG_PASS" || { echo "A6L_RELEASE_CONFIG_FAIL"; exit 1; }
if [ "${1:-}" = --build ]; then
  make -C "$K" O="$O" ARCH=arm64 LLVM=1 -j12 Image modules > "$O/build.log" 2>&1 || { echo "BUILD FAIL"; grep -a -m20 -E "error:" "$O/build.log"; exit 1; }
  echo "warnings: $(grep -c 'warning:' "$O/build.log" || true)"
  python3 - "$R5/Module.symvers" "$O/Module.symvers" <<'PY'
import sys
def load(p):
    d = {}
    for l in open(p):
        f = l.rstrip('\n').split('\t')
        if f[2] == 'vmlinux': d[f[1]] = f[0]
    return d
a, b = load(sys.argv[1]), load(sys.argv[2])
chg = sorted(s for s in a if s in b and a[s] != b[s]); gone = sorted(s for s in a if s not in b)
print(f"vmlinux exports r5={len(a)} release={len(b)} crc_changed={len(chg)} missing={len(gone)}")
for s in chg[:20]: print('  CRC', s)
for s in gone[:40]: print('  MISSING', s)
PY
  sha256sum "$O/arch/arm64/boot/Image"; strings "$O/vmlinux" | grep -m1 "^Linux version"
fi
