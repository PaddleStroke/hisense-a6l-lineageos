"""Verify the built eMMC module loads into its matching kernel in diskless QEMU."""
import gzip
import hashlib
import json
from pathlib import Path
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]
KERNEL = ROOT / 'firmware/extracted/baseline-7.2-kernel-20260917'
OUT = Path('/home/a6l/kernel/test-baseline-7.2-smoke-20260917')
ARCHIVE = ROOT / 'firmware/extracted/baseline-7.2-smoke-20260917'
BUILD = Path('/home/a6l/kernel/out-a6l-baseline-7.2')
CLANG = Path('/home/a6l/android/a6l-lineage24/prebuilts/clang/host/linux-x86/clang-r584948/bin/clang')

def run(*args):
    return subprocess.run([str(a) for a in args], check=True, capture_output=True).stdout

OUT.mkdir(exist_ok=False)
assert not ARCHIVE.exists()
run(CLANG, '--target=aarch64-linux-android', '-static', '-nostdlib', '-ffreestanding', '-fno-stack-protector',
    '-Wl,-e,_start', '-O2', ROOT / 'tools/module-smoke-init.c', '-o', OUT / 'init')
(OUT / 'ramdisk.list').write_text('dir /dev 0755 0 0\nnod /dev/console 0600 0 0 c 5 1\n'
    f'file /init {OUT / "init"} 0755 0 0\nfile /sdhci-msm.ko {KERNEL / "sdhci-msm.ko"} 0400 0 0\n')
archive = run(BUILD / 'usr/gen_init_cpio', '-t', '1789344000', OUT / 'ramdisk.list')
(OUT / 'ramdisk.cpio.gz').write_bytes(gzip.compress(archive, mtime=0))
args = ['qemu-system-aarch64', '-machine', 'virt,gic-version=3', '-cpu', 'cortex-a53',
        '-smp', '2', '-m', '2048', '-nodefaults', '-nographic', '-monitor', 'none',
        '-serial', 'stdio', '-nic', 'none', '-no-reboot', '-kernel', str(KERNEL / 'Image'),
        '-initrd', str(OUT / 'ramdisk.cpio.gz'), '-append', 'console=ttyAMA0 loglevel=7 panic=0']
report = {'passed': False, 'scope': 'Diskless generic ARM64 QEMU module symbol/ABI load test; no SDM660 hardware emulated',
          'module_sha256': hashlib.sha256((KERNEL / 'sdhci-msm.ko').read_bytes()).hexdigest(), 'command': args}
proc = None
try:
    with (OUT / 'console.log').open('wb') as log:
        proc = subprocess.Popen(args, stdout=log, stderr=subprocess.STDOUT)
        deadline = time.monotonic()+60
        while time.monotonic()<deadline:
            data = (OUT / 'console.log').read_bytes()
            if b'A6L_MODULE_SMOKE_PASS' in data or b'A6L_MODULE_SMOKE_FAIL' in data or b'Kernel panic' in data:
                break
            assert proc.poll() is None, 'QEMU exited early'
            time.sleep(.25)
        assert b'A6L_MODULE_SMOKE_PASS' in data, data[-5000:].decode(errors='replace')
        assert b'Unknown symbol' not in data and b'Kernel panic' not in data
        report['passed'] = True
finally:
    if proc and proc.poll() is None:
        proc.kill()
        proc.wait(timeout=5)
    (OUT / 'report.json').write_text(json.dumps(report, indent=2)+'\n')
    ARCHIVE.mkdir(exist_ok=False)
    for p in OUT.iterdir():
        if p.is_file():
            (ARCHIVE / p.name).write_bytes(p.read_bytes())
print(json.dumps({'passed': report['passed'], 'module_sha256': report['module_sha256']}))
