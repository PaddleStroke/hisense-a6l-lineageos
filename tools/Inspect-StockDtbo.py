#!/usr/bin/env python3
"""Extract verified DTBO entries and apply one to saved DTBs offline in Linux."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
from a6l_fdt import read_fdt, cells, strings


p = argparse.ArgumentParser(description=__doc__)
p.add_argument('backup', type=Path)
p.add_argument('base_trees', type=Path)
p.add_argument('output', type=Path)
p.add_argument('--index', type=int, required=True)
a = p.parse_args()
manifest = json.loads((a.backup / 'firmware-verification.json').read_text())
part = next(x for x in manifest['partitions'] if x['name'] == 'dtbo')
with (a.backup / 'emmc-firmware-prefix.bin').open('rb') as f:
    f.seek(part['offset'])
    data = f.read(part['bytes'])
if len(data) != part['bytes'] or hashlib.sha256(data).hexdigest() != part['sha256']:
    raise SystemExit('DTBO capture hash mismatch')
magic, total, header, entry_size, count, offset, page, version = struct.unpack_from('>8I', data)
if (magic != 0xd7b7ab1e or version != 0 or header < 32 or entry_size < 32
        or total > len(data) or offset + count * entry_size > total
        or not 0 <= a.index < count):
    raise SystemExit('Unexpected DTBO table layout')
a.output.mkdir(parents=True, exist_ok=False)
report = {'dtbo_partition_sha256': part['sha256'], 'version': version,
          'selected_index': a.index, 'selection_source': 'Caller supplies index; verify against ro.boot.dtbo_idx',
          'entries': [], 'merged': []}
for i in range(count):
    size, start, ident, revision, *custom = struct.unpack_from('>8I', data, offset + i * entry_size)
    if start < offset + count * entry_size or start + size > total:
        raise SystemExit('DTBO payload outside data bounds')
    blob = data[start:start + size]
    read_fdt(blob)
    dest = a.output / f'overlay-{i:02d}.dtbo'
    dest.write_bytes(blob)
    subprocess.run(['dtc', '-I', 'dtb', '-O', 'dts', '-o', str(dest.with_suffix('.dts')), str(dest)],
                   check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    report['entries'].append({'index': i, 'bytes': size, 'id': ident, 'rev': revision,
                              'custom': custom, 'sha256': hashlib.sha256(blob).hexdigest()})
for item in json.loads((a.base_trees / 'manifest.json').read_text()):
    base = a.base_trees / item['file']
    if hashlib.sha256(base.read_bytes()).hexdigest() != item['sha256']:
        raise SystemExit('Base DTB hash mismatch')
    dest = a.output / f'{base.stem}-merged.dtb'
    proc = subprocess.run(['fdtoverlay', '-i', str(base), '-o', str(dest),
                           str(a.output / f'overlay-{a.index:02d}.dtbo')],
                          capture_output=True, text=True)
    entry = {'base': base.name, 'base_sha256': item['sha256'],
             'merge_exit': proc.returncode, 'merge_stderr': proc.stderr}
    report['merged'].append(entry)
    if proc.returncode:
        continue
    nodes = read_fdt(dest.read_bytes())
    decoded = subprocess.run(['dtc', '-I', 'dtb', '-O', 'dts', '-o',
                              str(dest.with_suffix('.dts')), str(dest)], capture_output=True, text=True)
    (dest.with_suffix('.log')).write_text(decoded.stderr)
    decoded.check_returncode()
    entry['sha256'] = hashlib.sha256(dest.read_bytes()).hexdigest()
    entry['model'] = strings(nodes['/']['model'])
    entry['nodes'] = len(nodes)
    changed = []
    before = read_fdt(base.read_bytes())
    for path, props in nodes.items():
        if path.startswith('/__'):
            continue
        differences = [key for key, value in props.items() if before.get(path, {}).get(key) != value]
        if differences:
            changed.append({'path': path, 'properties': differences})
    entry['changed_nodes'] = changed
(a.output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
print(json.dumps(report, indent=2))
raise SystemExit(0 if all(x['merge_exit'] == 0 for x in report['merged']) else 1)
