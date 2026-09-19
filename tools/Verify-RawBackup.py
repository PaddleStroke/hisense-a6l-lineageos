"""Validate an eMMC user-area image; never access the phone or write image bytes."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import zlib

SECTOR = 512


def header(source, lba):
    source.seek(lba * SECTOR)
    raw = source.read(SECTOR)
    if raw[:8] != b'EFI PART':
        raise ValueError(f'Missing GPT signature at LBA {lba}')
    size, crc = struct.unpack_from('<II', raw, 12)
    if not 92 <= size <= SECTOR:
        raise ValueError('Invalid GPT header size')
    check = bytearray(raw[:size])
    check[16:20] = bytes(4)
    if zlib.crc32(check) != crc:
        raise ValueError('GPT header CRC mismatch')
    current, alternate, first, last = struct.unpack_from('<QQQQ', raw, 24)
    entries_lba, count, entry_size, entries_crc = struct.unpack_from('<QIII', raw, 72)
    if current != lba or count > 4096 or not 128 <= entry_size <= 4096:
        raise ValueError('Invalid GPT geometry')
    source.seek(entries_lba * SECTOR)
    entries = source.read(count * entry_size)
    if len(entries) != count * entry_size or zlib.crc32(entries) != entries_crc:
        raise ValueError('GPT entries CRC mismatch')
    partitions = []
    for i in range(count):
        entry = entries[i * entry_size:(i + 1) * entry_size]
        if entry[:16] == bytes(16):
            continue
        start, end = struct.unpack_from('<QQ', entry, 32)
        name = entry[56:128].decode('utf-16le').split('\0')[0]
        if not first <= start <= end <= last:
            raise ValueError(f'Partition bounds invalid: {name}')
        partitions.append(dict(name=name, offset=start * SECTOR, bytes=(end - start + 1) * SECTOR))
    ordered = sorted(partitions, key=lambda p: p['offset'])
    if any(a['offset'] + a['bytes'] > b['offset'] for a, b in zip(ordered, ordered[1:])):
        raise ValueError('Overlapping partitions')
    return dict(current=current, alternate=alternate, disk_guid=raw[56:72].hex(),
                partitions=ordered), entries


def verify(path, prefix_only=False):
    with path.open('rb') as source:
        primary, entries = header(source, 1)
        expected = (primary['alternate'] + 1) * SECTOR
        report = dict(image=path.name, expected_bytes=expected,
                      primary_gpt_crc_valid=True, partitions=primary['partitions'])
        if prefix_only:
            print(json.dumps(report, indent=2))
            return report
        if path.stat().st_size != expected:
            raise ValueError('Image size does not match GPT disk size')
        secondary, secondary_entries = header(source, primary['alternate'])
        if secondary['alternate'] != 1 or entries != secondary_entries or primary['disk_guid'] != secondary['disk_guid']:
            raise ValueError('Primary/secondary GPT mismatch')
        report['secondary_gpt_crc_valid'] = True
        whole = hashlib.sha256()
        source.seek(0)
        for part in report['partitions']:
            gap = part['offset'] - source.tell()
            while gap:
                block = source.read(min(gap, 8 * 1024 * 1024))
                if not block: raise ValueError('Truncated gap')
                whole.update(block)
                gap -= len(block)
            remaining = part['bytes']
            digest = hashlib.sha256()
            while remaining:
                block = source.read(min(remaining, 8 * 1024 * 1024))
                if not block: raise ValueError('Truncated partition')
                whole.update(block)
                digest.update(block)
                remaining -= len(block)
            part['sha256'] = digest.hexdigest()
            print('Hashed', part['name'], flush=True)
        while block := source.read(8 * 1024 * 1024):
            whole.update(block)
        report['sha256'] = whole.hexdigest()
        report['scope'] = 'eMMC user area only; excludes hardware boot areas, RPMB and fuses'
        report['restore_tested'] = False
    path.with_suffix('.verification.json').write_text(json.dumps(report, indent=2) + '\n')
    print('Image and partition checksums saved', flush=True)
    return report


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('image', type=Path)
    parser.add_argument('--prefix-only', action='store_true')
    args = parser.parse_args()
    verify(args.image, args.prefix_only)
