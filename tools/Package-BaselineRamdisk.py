"""Reuse the verified RAM logger with the older kernel's matching storage module."""
import gzip
import hashlib
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
OLD = ROOT / 'firmware/extracted/storage-state-ramdisk-v32-20260917'
KERNEL = ROOT / 'firmware/extracted/baseline-6.19-kernel-20260917'
OUT = ROOT / 'firmware/extracted/baseline-6.19-ramdisk-20260917'
BUILD = Path('/home/a6l/kernel/out-a6l-baseline-6.19')


def sha(data):
    return hashlib.sha256(data).hexdigest()


def run(*args, **kwargs):
    return subprocess.run([str(a) for a in args], check=True, capture_output=True, **kwargs).stdout


old = json.loads((OLD / 'report.json').read_text())
init = (OLD / 'init').read_bytes()
source = (OLD / 'init-source.c').read_bytes()
assert old['packaging_passed'] and sha(init) == old['init_sha256'] and sha(source) == old['source_sha256']
module = (KERNEL / 'sdhci-msm.ko').read_bytes()
assert run('modinfo', '-F', 'depends', KERNEL / 'sdhci-msm.ko').strip() == b''
assert b'6.19.10-a6l-probe' in run('modinfo', '-F', 'vermagic', KERNEL / 'sdhci-msm.ko')
assert b'CONFIG_MMC_SDHCI_MSM=m\n' in (KERNEL / 'config').read_bytes()
OUT.mkdir(exist_ok=False)
for name, data in [('init', init), ('init-source.c', source), ('sdhci-msm.ko', module)]:
    (OUT / name).write_bytes(data)
listing = ('dir /dev 0755 0 0\nnod /dev/console 0600 0 0 c 5 1\n'
           'dir /proc 0755 0 0\ndir /sys 0755 0 0\n'
           f'file /init {OUT / "init"} 0755 0 0\n'
           f'file /sdhci-msm.ko {OUT / "sdhci-msm.ko"} 0400 0 0\n')
(OUT / 'ramdisk.list').write_text(listing)
archive = run(BUILD / 'usr/gen_init_cpio', '-t', '1789344000', OUT / 'ramdisk.list')
compressed = gzip.compress(archive, mtime=0)
(OUT / 'ramdisk.cpio').write_bytes(archive)
(OUT / 'ramdisk.cpio.gz').write_bytes(compressed)
(OUT / 'ramdisk-listing.txt').write_bytes(run('cpio', '-itv', input=archive))
report = dict(packaging_passed=True, init_sha256=sha(init), source_sha256=sha(source),
              module_sha256=sha(module), ramdisk_sha256=sha(compressed),
              scope='Same verified RAM logger; matching 6.19.10 eMMC module; no persistent mounts or block-device opens')
(OUT / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
print(json.dumps(report, indent=2))
