#!/usr/bin/env python3
"""Package the revised static RAM PID 1; no phone access or emulator execution."""
import argparse
import gzip
import hashlib
import json
from pathlib import Path
import shutil
import struct
import subprocess


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


p = argparse.ArgumentParser(description=__doc__)
p.add_argument('android_source', type=Path)
p.add_argument('kernel_output', type=Path)
p.add_argument('completed_build_log', type=Path)
p.add_argument('output', type=Path)
a = p.parse_args()
if '#### build completed successfully' not in a.completed_build_log.read_text()[-16384:]:
    raise SystemExit('Diagnostic init build is incomplete')
binary = a.android_source / 'out/target/product/a6l/system/bin/a6l_probe_init'
data = binary.read_bytes()
assert b'A6L_DEFERRED_BEGIN' in data and b'/proc/sys/kernel/printk_devkmsg' in data
if data[:6] != b'\x7fELF\x02\x01' or struct.unpack_from('<H', data, 18)[0] != 183:
    raise SystemExit('Diagnostic init must be little-endian AArch64 ELF64')
phoff = struct.unpack_from('<Q', data, 32)[0]
phsize, phnum = struct.unpack_from('<HH', data, 54)
if phsize != 56 or phoff + phsize * phnum > len(data):
    raise SystemExit('Invalid ELF program header layout')
if any(struct.unpack_from('<I', data, phoff + i * phsize)[0] in (2, 3) for i in range(phnum)):
    raise SystemExit('Diagnostic init must have no dynamic segment or interpreter')
a.output.mkdir(parents=True, exist_ok=False)
saved_init = a.output / 'init'
shutil.copyfile(binary, saved_init)
(a.output / 'init-source.c').write_bytes((a.android_source / 'device/hisense/a6l/diagnostic/init.c').read_bytes())
(a.output / 'build.log').write_bytes(a.completed_build_log.read_bytes())
readelf = subprocess.run(['readelf', '-h', '-l', str(saved_init)], capture_output=True, text=True, check=True)
(a.output / 'init-readelf.txt').write_text(readelf.stdout)
archive_list = a.output / 'ramdisk.list'
archive_list.write_text('dir /dev 0755 0 0\n'
                        'nod /dev/console 0600 0 0 c 5 1\n'
                        'dir /proc 0755 0 0\n'
                        'dir /sys 0755 0 0\n'
                        f'file /init {saved_init} 0755 0 0\n')
archive = subprocess.run([str(a.kernel_output / 'usr/gen_init_cpio'), '-t', '1789344000',
                           str(archive_list)], capture_output=True, check=True).stdout
(a.output / 'ramdisk.cpio').write_bytes(archive)
listing = subprocess.run(['cpio', '-itv'], input=archive, capture_output=True, check=True)
(a.output / 'ramdisk-listing.txt').write_bytes(listing.stdout + listing.stderr)
ramdisk = a.output / 'ramdisk.cpio.gz'
ramdisk.write_bytes(gzip.compress(archive, mtime=0))
report = {'scope': 'Static RAM diagnostic packaging only; run the separate QEMU test before using it',
          'init_sha256': digest(saved_init), 'ramdisk_sha256': digest(ramdisk),
          'source_sha256': digest(a.android_source / 'device/hisense/a6l/diagnostic/init.c')}
(a.output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
print(json.dumps(report, indent=2))
