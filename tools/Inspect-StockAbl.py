#!/usr/bin/env python3
"""Extract and inspect only the verified backup's Android bootloader offline."""
import hashlib
import io
import json
from pathlib import Path
import re
from elftools.elf.elffile import ELFFile

workspace = Path(__file__).resolve().parent.parent
backup = workspace / 'firmware/raw-backup-20260914'
output = workspace / 'firmware/extracted/stock-abl-20260914'
output.mkdir(exist_ok=False)
manifest = json.loads((backup / 'firmware-verification.json').read_text())
part = next(p for p in manifest['partitions'] if p['name'] == 'abl')
with (backup / 'emmc-firmware-prefix.bin').open('rb') as stream:
    stream.seek(part['offset'])
    data = stream.read(part['bytes'])
digest = hashlib.sha256(data).hexdigest()
assert digest == part['sha256'], 'Backup partition hash differs'
(output / 'abl.bin').write_bytes(data)
elf = ELFFile(io.BytesIO(data))
report = {'sha256': digest, 'bytes': len(data),
          'elf_machine': elf.header['e_machine'], 'segments': []}
for index, segment in enumerate(elf.iter_segments()):
    entry = {key: segment.header[key] for key in
             ['p_type', 'p_offset', 'p_vaddr', 'p_paddr', 'p_filesz', 'p_memsz']}
    entry['index'] = index
    report['segments'].append(entry)
strings = [f'{match.start():08x} {match.group().decode("ascii")}'
           for match in re.finditer(rb'[ -~]{5,}', data)]
(output / 'strings-ascii.txt').write_text('\n'.join(strings) + '\n')
report['firmware_volume_signatures'] = [m.start() for m in re.finditer(b'_FVH', data)]
report['mz_signatures'] = [m.start() for m in re.finditer(b'MZ', data)]
(output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
print(json.dumps(report, indent=2))
