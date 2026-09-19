#!/usr/bin/env python3
"""Inspect a completed A6L compile-probe system image without mounting/flashing it."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import struct
import subprocess


def digest(path):
    value = hashlib.sha256()
    with path.open('rb') as f:
        for block in iter(lambda: f.read(8 * 1024 * 1024), b''):
            value.update(block)
    return value.hexdigest()


def run_read(command, output):
    proc = subprocess.run(command, text=True, capture_output=True)
    output.write_text(proc.stdout + proc.stderr)
    return proc


p = argparse.ArgumentParser(description=__doc__)
p.add_argument('source', type=Path)
p.add_argument('completed_build_log', type=Path)
p.add_argument('output', type=Path, help='New private output directory')
a = p.parse_args()
with a.completed_build_log.open('rb') as f:
    f.seek(0, 2)
    f.seek(max(0, f.tell() - 16384))
    tail = f.read().decode(errors='replace')
if not re.search(r'#### build completed successfully', tail):
    raise SystemExit('No final successful-build marker; leave the running build alone')

product = a.source / 'out/target/product/a6l'
image = product / 'system.img'
if not image.is_file():
    raise SystemExit('Completed system.img is missing')
a.output.mkdir(parents=True, exist_ok=False)
with image.open('rb') as f:
    header = f.read(28)
sparse = struct.unpack_from('<I', header)[0] == 0xed26ff3a
report = {'scope': 'Offline image inspection, not a boot/driver test',
          'image': str(image.resolve()), 'image_bytes': image.stat().st_size,
          'image_sha256': digest(image), 'android_sparse': sparse}
raw = a.output / 'system-expanded.img'
if sparse:
    _, major, minor, file_hdr, chunk_hdr, block_size, blocks, chunks, checksum = struct.unpack('<I4H4I', header)
    report['sparse_header'] = {'major': major, 'minor': minor, 'block_size': block_size,
                               'blocks': blocks, 'chunks': chunks,
                               'expanded_bytes': block_size * blocks}
    if major != 1 or file_hdr < 28 or chunk_hdr < 12:
        raise SystemExit('Unexpected sparse format')
    proc = run_read([str(a.source / 'out/host/linux-x86/bin/simg2img'), str(image), str(raw)],
                    a.output / 'simg2img.log')
    if proc.returncode:
        raise SystemExit('Sparse expansion failed; inspect simg2img.log')
else:
    shutil.copyfile(image, raw)
report['expanded_bytes'] = raw.stat().st_size
report['expanded_sha256'] = digest(raw)
checks = {}
for name, command in {
    'filesystem': ['e2fsck', '-f', '-n', str(raw)],
    'superblock': ['dumpe2fs', '-h', str(raw)],
    'root': ['debugfs', '-R', 'ls -l /', str(raw)],
    'root-init': ['debugfs', '-R', 'stat /init', str(raw)],
    'system-init': ['debugfs', '-R', 'stat /system/bin/init', str(raw)],
    'build-properties': ['debugfs', '-R', 'cat /system/build.prop', str(raw)],
    'apex-list': ['debugfs', '-R', 'ls -l /system/apex', str(raw)],
    'system-ext-apex-list': ['debugfs', '-R', 'ls -l /system/system_ext/apex', str(raw)],
    'linker-config': ['debugfs', '-R', 'cat /system/etc/linker.config.json', str(raw)],
}.items():
    print(f'Reading {name}...', flush=True)
    proc = run_read(command, a.output / f'{name}.txt')
    checks[name] = {'exit_code': proc.returncode}
    if name == 'build-properties':
        wanted = ('ro.build.version.', 'ro.lineage.', 'ro.product.system.')
        report['selected_properties'] = dict(line.split('=', 1) for line in proc.stdout.splitlines()
                                             if line.startswith(wanted) and '=' in line)
report['checks'] = checks
init_read = subprocess.run(['debugfs', '-R', 'cat /system/bin/init', str(raw)],
                           capture_output=True)
if init_read.stdout.startswith(b'\x7fELF'):
    init_file = a.output / 'system-init.elf'
    init_file.write_bytes(init_read.stdout)
    run_read(['readelf', '-h', '-l', str(init_file)], a.output / 'system-init-elf.txt')
    report['init_observations'] = {
        'sha256': digest(init_file),
        'matches_staged_init': digest(init_file) == digest(product / 'system/bin/init'),
        'contains_second_stage_argument_error':
            b'Second-stage init requires an argument to main()' in init_read.stdout,
    }
else:
    report['init_observations'] = {'error': 'Could not read an ELF at /system/bin/init'}
report['filesystem_check_passed'] = checks['filesystem']['exit_code'] == 0
report['source_revision_manifest_sha256'] = digest(a.source / 'a6l-source-revisions.xml')
(a.output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
print(json.dumps({key: report[key] for key in ('image_bytes', 'expanded_bytes', 'image_sha256',
                                             'filesystem_check_passed')}, indent=2))
if not report['filesystem_check_passed']:
    raise SystemExit('Filesystem check did not pass; inspect filesystem.txt')
