#!/usr/bin/env bash
# Compile an isolated kernel/DTB prototype. No phone access or boot packaging.
set -euo pipefail
workspace=/mnt/c/Users/Pierre/Desktop/A6L
kernel_dir="$HOME/kernel/a6l-mainline"
out="$HOME/kernel/out-a6l-probe"
clang_dir="$HOME/android/a6l-lineage24/prebuilts/clang/host/linux-x86/clang-r584948/bin"
revision=e47d622cb6d2440a9eacdc8bb2df32c037bec7b8
test "$(git -C "$kernel_dir" rev-parse HEAD)" = "$revision"
mkdir -p "$out" "$HOME/logs"
exec 9>"$out/.build.lock"
flock -n 9 || { echo 'A kernel probe build is already running'; exit 1; }
log="$HOME/logs/kernel-probe-$(date +%Y%m%d-%H%M%S).log"
exec > >(tee "$log") 2>&1
printf '%s\n' "$log" > "$workspace/logs/kernel-probe-log-path.txt"
export PATH="$clang_dir:$PATH"
export KBUILD_BUILD_USER=a6l KBUILD_BUILD_HOST=a6l-build
export KBUILD_BUILD_TIMESTAMP='2026-09-14 00:00:00 UTC'
cd "$kernel_dir"
cp "$workspace/device/hisense/a6l/kernel/sdm660-hisense-a6l-probe.dts" arch/arm64/boot/dts/qcom/
make O="$out" ARCH=arm64 LLVM=1 sdm660_defconfig
ARCH=arm64 LLVM=1 KCONFIG_CONFIG="$out/.config" scripts/kconfig/merge_config.sh -m \
    "$out/.config" "$workspace/device/hisense/a6l/kernel/probe.config"
make O="$out" ARCH=arm64 LLVM=1 olddefconfig
python3 - "$workspace/device/hisense/a6l/kernel/probe.config" "$out/.config" <<'PY'
from pathlib import Path
import sys
def values(path):
    return dict(line.split('=', 1) for line in Path(path).read_text().splitlines()
                if line.startswith('CONFIG_') and '=' in line)
requested, actual = values(sys.argv[1]), values(sys.argv[2])
missing = {key: {'requested': value, 'actual': actual.get(key)}
           for key, value in requested.items() if actual.get(key) != value}
if missing:
    raise SystemExit(f'Kernel configuration dropped or changed required settings: {missing}')
PY
make O="$out" ARCH=arm64 LLVM=1 -j8 qcom/sdm660-hisense-a6l-probe.dtb
make O="$out" ARCH=arm64 LLVM=1 -j8 Image.gz
printf '\nA6L_KERNEL_PROBE_BUILD_SUCCESS\n'
sha256sum "$out/arch/arm64/boot/Image.gz" \
    "$out/arch/arm64/boot/dts/qcom/sdm660-hisense-a6l-probe.dtb"
