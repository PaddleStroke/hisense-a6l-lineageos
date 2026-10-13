#!/usr/bin/env python3
"""Test an ARM64 header load-offset adaptation offline at a measured QEMU address."""
import gzip
import hashlib
import json
import os
from pathlib import Path
import re
import socket
import struct
import subprocess
import time

root = Path(__file__).resolve().parent.parent
source = root / 'firmware/extracted/kernel-probe-20260914/Image'
ramdisk = root / 'firmware/extracted/diagnostic-ramdisk-20260914/ramdisk.cpio.gz'
output = root / 'firmware/extracted/abl-offset-test-20260914'
original = source.read_bytes()
assert hashlib.sha256(original).hexdigest() == '57120be5a70114bf476f26a8f39ac6af0660d6240d6017379d160d258f76bf92'
assert hashlib.sha256(ramdisk.read_bytes()).hexdigest() == '14141431a5e57a4510159365eddddc5322aceb271272787a5282ee27f2a9d983'
assert original[56:60] == b'ARM\x64'
assert struct.unpack_from('<Q', original, 8)[0] == 0
assert 'CONFIG_RELOCATABLE=y' in (source.parent / 'kernel.config').read_text()
image_size = struct.unpack_from('<Q', original, 16)[0]
offset = 0x80000
assert offset + image_size < 0x3200000, 'Image runtime region reaches stock DTB address'
adapted = bytearray(original)
struct.pack_into('<Q', adapted, 8, offset)
assert adapted[:8] == original[:8] and adapted[16:] == original[16:]
output.mkdir(exist_ok=False)
image = output / 'Image.a6l-offset'
image.write_bytes(adapted)
(output / 'Image.a6l-offset.gz').write_bytes(gzip.compress(adapted, mtime=0))
qmp_path = Path(f'/tmp/a6l-offset-{os.getpid()}.sock')
command = ['qemu-system-aarch64', '-machine', 'virt,gic-version=3', '-cpu', 'cortex-a53',
           '-smp', '2', '-m', '1024', '-nodefaults', '-nographic', '-monitor', 'none',
           '-serial', 'stdio', '-nic', 'none', '-no-reboot', '-S',
           '-qmp', f'unix:{qmp_path},server=on,wait=off',
           '-kernel', str(image), '-initrd', str(ramdisk),
           '-append', 'console=ttyAMA0,115200 loglevel=6 panic=-1']
report = {'scope': 'Offline load-offset check only; no A6L hardware or bootloader emulation',
          'command': command, 'load_offset': hex(offset), 'image_size': image_size,
          'adapted_sha256': hashlib.sha256(adapted).hexdigest(),
          'original_sha256': hashlib.sha256(original).hexdigest(),
          'changed_byte_offsets': [i for i, (a, b) in enumerate(zip(original, adapted)) if a != b]}
proc = None
try:
    with (output / 'console.log').open('wb') as console, (output / 'stderr.log').open('wb') as errors:
        proc = subprocess.Popen(command, stdout=console, stderr=errors)
        for _ in range(100):
            if qmp_path.exists() or proc.poll() is not None:
                break
            time.sleep(0.05)
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as connection:
            connection.settimeout(5)
            connection.connect(str(qmp_path))
            stream = connection.makefile('rwb', buffering=0)
            report['qmp_greeting'] = json.loads(stream.readline())

            def qmp(execute, arguments=None):
                request = {'execute': execute}
                if arguments:
                    request['arguments'] = arguments
                stream.write(json.dumps(request).encode() + b'\n')
                while True:
                    line = stream.readline()
                    if not line:
                        raise RuntimeError('QMP disconnected')
                    response = json.loads(line)
                    if 'error' in response:
                        raise RuntimeError(response['error'])
                    if 'return' in response:
                        return response['return']

            qmp('qmp_capabilities')
            memory = qmp('human-monitor-command', {'command-line': 'xp /64bx 0x40080000'})
            report['physical_header_dump'] = memory
            measured = bytes(int(value, 16) for value in re.findall(r'0x([0-9a-fA-F]{2})(?![0-9a-fA-F])', memory))
            report['physical_header_matches_at_0x40080000'] = measured == adapted[:64]
            assert report['physical_header_matches_at_0x40080000'], 'QEMU did not place the expected Image at the requested offset'
            qmp('cont')
            try:
                proc.wait(timeout=35)
                report['expected_timeout'] = False
            except subprocess.TimeoutExpired:
                report['expected_timeout'] = True
    log = (output / 'console.log').read_bytes()
    report['checks'] = {
        'header_only_change': adapted[:8] == original[:8] and adapted[16:] == original[16:],
        'measured_load_address': report['physical_header_matches_at_0x40080000'],
        'runtime_region_below_stock_dtb': offset + image_size < 0x3200000,
        'diagnostic_ready': b'A6L_RAM_PROBE_READY' in log,
        'diagnostic_alive': b'A6L_RAM_PROBE_ALIVE' in log,
        'no_kernel_panic': b'Kernel panic' not in log,
        'stayed_running': report['expected_timeout'],
        'original_image_unchanged': source.read_bytes() == original}
    report['all_checks_passed'] = all(report['checks'].values())
except Exception as error:
    report['error'] = str(error)
    report['all_checks_passed'] = False
finally:
    if proc and proc.poll() is None:
        proc.kill()
        proc.wait(timeout=5)
    qmp_path.unlink(missing_ok=True)
    (output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
print(json.dumps(report, indent=2))
raise SystemExit(0 if report['all_checks_passed'] else 1)
