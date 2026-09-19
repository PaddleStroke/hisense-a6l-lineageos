"""Package V20 RAM init and its matching eMMC module; no device access."""
import gzip
import hashlib
import json
from pathlib import Path
import struct
import subprocess

ROOT = Path(__file__).resolve().parents[1]
ANDROID = Path('/home/a6l/android/a6l-lineage24')
BUILD = Path('/home/a6l/kernel/out-a6l-baseline-7.2')
KERNEL = ROOT / 'firmware/extracted/baseline-7.2-kernel-20260917'
OUT = ROOT / 'firmware/extracted/storage-read-ramdisk-v37-20260917'

def sha(data):
    return hashlib.sha256(data).hexdigest()

def run(*argv, **kwargs):
    return subprocess.run([str(v) for v in argv], check=True, capture_output=True, **kwargs).stdout

log = Path('/home/a6l/logs/build-diagnostic-init.log').read_bytes()
assert b'#### build completed successfully' in log[-16384:]
source = (ANDROID / 'device/hisense/a6l/diagnostic/init.c').read_bytes()
assert source == (ROOT / 'device/hisense/a6l/diagnostic/init.c').read_bytes()
data = (ANDROID / 'out/target/product/a6l/system/bin/a6l_probe_init').read_bytes()
assert data[:6] == b'\x7fELF\x02\x01' and struct.unpack_from('<H', data, 18)[0] == 183
phoff = struct.unpack_from('<Q', data, 32)[0]
phsize, phnum = struct.unpack_from('<HH', data, 54)
assert phsize == 56 and phoff + phsize * phnum <= len(data)
assert not any(struct.unpack_from('<I', data, phoff+i*phsize)[0] in (2, 3) for i in range(phnum))
assert b'A6L_STORAGE_READ_BEGIN passes=2 direct=1' in data
assert b'A6L_STORAGE_HASH_SELFTEST success=%d' in data
assert b'A6L_STORAGE_MODULE_FORK_ARMED' in data
assert b'A6L_STORAGE_MODULE_LOAD_BEGIN pause_ms=1000' in data and b'A6L_STORAGE_SNAPSHOT_DONE' in data
module = (KERNEL / 'sdhci-msm.ko').read_bytes()
assert module[:6] == b'\x7fELF\x02\x01' and struct.unpack_from('<H', module, 18)[0] == 183
info = run('modinfo', KERNEL / 'sdhci-msm.ko').decode()
assert 'name:           sdhci_msm' in info
assert run('modinfo', '-F', 'depends', KERNEL / 'sdhci-msm.ko').strip() == b''
assert b'7.2.3-a6l-probe+' in run('modinfo', '-F', 'vermagic', KERNEL / 'sdhci-msm.ko')
config = (KERNEL / 'config').read_bytes()
assert b'CONFIG_MMC_SDHCI_MSM=m\n' in config
OUT.mkdir(exist_ok=False)
for name, value in [('init', data), ('init-source.c', source), ('sdhci-msm.ko', module), ('build.log', log)]:
    (OUT / name).write_bytes(value)
listing = ('dir /dev 0755 0 0\nnod /dev/console 0600 0 0 c 5 1\n'
           'dir /proc 0755 0 0\ndir /sys 0755 0 0\n'
           f'file /init {OUT / "init"} 0755 0 0\n'
           f'file /sdhci-msm.ko {OUT / "sdhci-msm.ko"} 0400 0 0\n')
(OUT / 'ramdisk.list').write_text(listing)
archive = run(BUILD / 'usr/gen_init_cpio', '-t', '1789344000', OUT / 'ramdisk.list')
(OUT / 'ramdisk.cpio').write_bytes(archive)
compressed = gzip.compress(archive, mtime=0)
(OUT / 'ramdisk.cpio.gz').write_bytes(compressed)
(OUT / 'ramdisk-listing.txt').write_bytes(run('cpio', '-itv', input=archive))
report = dict(packaging_passed=True, init_sha256=sha(data), source_sha256=sha(source),
              module_sha256=sha(module), ramdisk_sha256=sha(compressed), module_info=info,
              scope='V37 RAM logger and fixed-range direct read-only SHA256 verifier; exact V36 storage module; no persistent filesystem mounts or block writes')
report['extra_sources'] = {}
for name in ['storage_read.c', 'storage_read_ranges.h']:
    value = (ANDROID / 'device/hisense/a6l/diagnostic' / name).read_bytes()
    assert value == (ROOT / 'device/hisense/a6l/diagnostic' / name).read_bytes()
    (OUT / name).write_bytes(value)
    report['extra_sources'][name] = sha(value)
assert sha(module) == 'c0d430c0396b3356a2b626dd480486022eeb67bee0d11454c4996ff3fbf1582d'
host_checks = json.loads((ROOT / 'firmware/extracted/storage-read-v37-20260917/host-tests.json').read_text())
assert host_checks['passed'] and host_checks['read_only_syscall_trace']
assert host_checks['source_sha256'] == report['extra_sources']['storage_read.c']
assert host_checks['ranges_sha256'] == report['extra_sources']['storage_read_ranges.h']
(OUT / 'report.json').write_text(json.dumps(report, indent=2)+'\n')
print(json.dumps(report, indent=2))
