#!/usr/bin/env python3
"""Verify recovered kernel code ranges and inventory stock e-ink/module interfaces.

Read-only analysis of saved files. No device access, kernel execution or writes
to calibration interfaces. Requires pyelftools and a recovered symbolized ELF.
"""
import argparse
from bisect import bisect_right
import hashlib
import json
from pathlib import Path
import re
import struct
from elftools.elf.elffile import ELFFile


def sha(data):
    return hashlib.sha256(data).hexdigest()


parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('backup', type=Path)
parser.add_argument('extracted', type=Path)
parser.add_argument('output', type=Path)
a = parser.parse_args()
manifest = json.loads((a.backup / 'firmware-verification.json').read_text())
boot = next(x for x in manifest['partitions'] if x['name'] == 'boot')
with (a.backup / 'emmc-firmware-prefix.bin').open('rb') as f:
    f.seek(boot['offset'])
    data = f.read(boot['bytes'])
if len(data) != boot['bytes'] or sha(data) != boot['sha256']:
    raise SystemExit('Boot bytes do not match the verified capture')
if data[:8] != b'ANDROID!':
    raise SystemExit('Expected Android boot header')
kernel_size = struct.unpack_from('<I', data, 8)[0]
page = struct.unpack_from('<I', data, 36)[0]
# gzip.decompress rejects appended DTBs; decompress exactly the first stream.
import zlib
decoder = zlib.decompressobj(16 + zlib.MAX_WBITS)
raw = decoder.decompress(data[page:page + kernel_size])
if not decoder.eof:
    raise SystemExit('Incomplete kernel gzip stream')
saved_raw = (a.backup / 'inspection/kernel-uncompressed').read_bytes()
if raw != saved_raw:
    raise SystemExit('Saved uncompressed kernel differs from verified boot payload')

elf_path = a.extracted / 'stock-symbolized.elf'
with elf_path.open('rb') as f:
    elf = ELFFile(f)
    if elf['e_machine'] != 'EM_AARCH64':
        raise SystemExit('Expected AArch64 kernel')
    symbols = list(elf.get_section_by_name('.symtab').iter_symbols())
    by_name = {s.name: s for s in symbols}
    by_address = {s['st_value']: s.name for s in symbols if s['st_value']}
    addresses = sorted(by_address)
    base = by_name['_text']['st_value']
    selected = []
    for s in symbols:
        if not re.fullmatch(r'(mdss_fb_set_epd_(connect|display_mode|commit_bitmap)|epd_spi_probe|epd_spi_read|epd_read_vcom|mirror_state_store)', s.name):
            continue
        start = s['st_value']
        end = addresses[bisect_right(addresses, start)]
        segment = next(p for p in elf.iter_segments() if p['p_type'] == 'PT_LOAD'
                       and p['p_vaddr'] <= start and end <= p['p_vaddr'] + p['p_filesz'])
        f.seek(segment['p_offset'] + start - segment['p_vaddr'])
        recovered = f.read(end - start)
        if recovered != raw[start - base:end - base]:
            raise SystemExit(f'Reconstructed ELF does not preserve {s.name}')
        selected.append({'name': s.name, 'address': hex(start), 'bytes': end - start,
                         'sha256': sha(recovered), 'bounds': 'next-symbol inspection range'})

    def cstring(address):
        offset = address - base
        if not 0 <= offset < len(raw):
            raise ValueError('String address outside saved kernel')
        end = raw.index(b'\0', offset, min(len(raw), offset + 512))
        return raw[offset:end].decode('utf-8')

    attributes = []
    for s in symbols:
        if not s.name.startswith('dev_attr_epd_'):
            continue
        offset = s['st_value'] - base
        name_pointer, mode, show, store = struct.unpack_from('<QH6xQQ', raw, offset)
        name = cstring(name_pointer)
        if name != s.name.removeprefix('dev_attr_'):
            raise ValueError('Attribute-name crosscheck failed')
        attributes.append({'name': name, 'declared_mode': oct(mode),
                           'show': by_address.get(show, hex(show)),
                           'store': by_address.get(store, hex(store))})

vendor_inventory = {x['path']: x for x in json.loads((a.extracted / 'vendor-inventory.json').read_text())}
modules = []
for path in sorted((a.extracted / 'vendor/lib/modules').glob('*.ko')):
    digest = sha(path.read_bytes())
    relative = path.relative_to(a.extracted / 'vendor').as_posix()
    if digest != vendor_inventory[relative]['sha256']:
        raise ValueError(f'Vendor module extraction hash mismatch: {relative}')
    with path.open('rb') as f:
        elf = ELFFile(f)
        info = elf.get_section_by_name('.modinfo')
        strings = [s.decode('utf-8') for s in info.data().split(b'\0') if s]
        versions = elf.get_section_by_name('__versions')
        modules.append({'name': path.name, 'sha256': digest,
                        'vermagic': [s[9:] for s in strings if s.startswith('vermagic=')],
                        'depends': [s[8:] for s in strings if s.startswith('depends=')],
                        'versioned_symbols_bytes': versions['sh_size'] if versions else 0})

report = {'scope': 'Offline interface evidence, not a runtime compatibility test',
          'boot_partition_sha256': boot['sha256'], 'raw_kernel_sha256': sha(raw),
          'symbolized_elf_sha256': sha(elf_path.read_bytes()), 'symbol_count': len(symbols),
          'text_base': hex(base), 'verified_code_ranges': selected,
          'device_attributes': attributes, 'vendor_modules': modules}
a.output.write_text(json.dumps(report, indent=2) + '\n')
print(f'Verified {len(selected)} code ranges, {len(attributes)} e-ink attributes, {len(modules)} module hashes.')
