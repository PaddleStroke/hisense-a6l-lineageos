#!/usr/bin/env python3
"""Package diagnostic PID 1 and boot it with the prototype kernel in diskless QEMU."""
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
command = ['qemu-system-aarch64', '-machine', 'virt,gic-version=3', '-cpu', 'cortex-a53',
           '-smp', '2', '-m', '1024', '-nodefaults', '-nographic', '-monitor', 'none',
           '-serial', 'stdio', '-nic', 'none', '-no-reboot',
           '-kernel', str(a.kernel_output / 'arch/arm64/boot/Image'), '-initrd', str(ramdisk),
           '-append', 'console=ttyAMA0,115200 loglevel=6 panic=-1']
timed_out = False
try:
    proc = subprocess.run(command, capture_output=True, timeout=45)
    output, errors, status = proc.stdout, proc.stderr, proc.returncode
except subprocess.TimeoutExpired as error:
    timed_out = True
    output, errors, status = error.stdout or b'', error.stderr or b'', None
(a.output / 'qemu-console.log').write_bytes(output)
(a.output / 'qemu-stderr.log').write_bytes(errors)
checks = {'static_init_copy_matches': digest(binary) == digest(saved_init),
          'diagnostic_pid1_started': b'A6L_RAM_PROBE_START' in output,
          'diagnostic_ready': b'A6L_RAM_PROBE_READY' in output,
          'diagnostic_stayed_alive': b'A6L_RAM_PROBE_ALIVE' in output,
          'no_kernel_panic': b'Kernel panic' not in output,
          'no_unexpected_emulator_exit': timed_out}
report = {'scope': 'Emulated ARM CPU and diagnostic RAM filesystem only; no A6L hardware emulation',
          'command': command, 'timeout_seconds': 45, 'expected_timeout': timed_out,
          'emulator_exit': status, 'checks': checks, 'all_checks_passed': all(checks.values()),
          'init_sha256': digest(saved_init), 'ramdisk_sha256': digest(ramdisk),
          'kernel_image_sha256': digest(a.kernel_output / 'arch/arm64/boot/Image'),
          'limits': ['QEMU virt does not emulate the A6L PMIC, eMMC, display or Qualcomm USB controller',
                     'The physical USB gadget path remains untested',
                     'No disk image or host directory was exposed to the guest']}
(a.output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
print(json.dumps(report, indent=2))
raise SystemExit(0 if report['all_checks_passed'] else 1)
