#!/usr/bin/env bash
# Diagnostic log only: identify the device before a probe can stop output.
set -euo pipefail
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
python3 - <<'PY'
from pathlib import Path
p = Path('drivers/base/dd.c')
s = p.read_text()
old = '\tcalltime = ktime_get();\n\tret = really_probe(dev, drv);'
new = ('\tprintk(KERN_DEBUG "A6L probe begin %s driver %s\\n",\n'
       '\t       dev_name(dev), drv->name);\n' + old)
if new not in s:
    assert s.count(old) == 1, 'Unexpected probe wrapper source'
    s = s.replace(old, new, 1)
    p.write_text(s)
assert s.count('A6L probe begin') == 1
PY
make O="$out" ARCH=arm64 LLVM=1 -j8 Image
echo A6L_PROBE_TRACE_BUILD_SUCCESS
