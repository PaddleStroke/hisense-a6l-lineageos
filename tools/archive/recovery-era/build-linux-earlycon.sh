#!/usr/bin/env bash
# Incrementally add only the bounded diagnostic LCD early console.
set -euo pipefail
workspace=/mnt/c/Users/Pierre/Desktop/A6L
kernel_dir="$HOME/kernel/a6l-mainline"
out="$HOME/kernel/out-a6l-probe"
clang_dir="$HOME/android/a6l-lineage24/prebuilts/clang/host/linux-x86/clang-r584948/bin"
test "$(git -C "$kernel_dir" rev-parse HEAD)" = e47d622cb6d2440a9eacdc8bb2df32c037bec7b8
exec 9>"$out/.build.lock"
flock -n 9 || { echo 'A kernel build is already running'; exit 1; }
export PATH="$clang_dir:$PATH"
export KBUILD_BUILD_USER=a6l KBUILD_BUILD_HOST=a6l-build
export KBUILD_BUILD_TIMESTAMP='2026-09-14 00:00:00 UTC'
cd "$kernel_dir"
cp "$workspace/device/hisense/a6l/kernel/a6l_earlycon.c" drivers/tty/serial/
python3 - <<'PY'
from pathlib import Path
p=Path('drivers/tty/serial/Makefile')
s=p.read_text()
line='obj-$(CONFIG_SERIAL_EARLYCON) += a6l_earlycon.o\n'
if line not in s:
    p.write_text(s+'\n# Local A6L diagnostic only; requires exact board/reservation checks.\n'+line)
PY
for setting in CONFIG_FONT_8x16=y CONFIG_SERIAL_EARLYCON=y; do
    grep -qx "$setting" "$out/.config"
done
make O="$out" ARCH=arm64 LLVM=1 -j8 Image
echo A6L_EARLYCON_BUILD_SUCCESS
