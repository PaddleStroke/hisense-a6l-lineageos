#!/usr/bin/env python3
"""Repackage verified saved boot/recovery payloads offline with AOSP tools.

Outputs are analysis artifacts, not installation images. No device access,
shell evaluation, original-file mutation, signing or AVB footer generation.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import sys


def sha(data):
    return hashlib.sha256(data).hexdigest()


def layout(data):
    if len(data) < 1648 or data[:8] != b'ANDROID!':
        raise ValueError('Not a complete Android boot header')
    k, ka, r, ra, s, sa, tags, page, version, osver = struct.unpack_from('<10I', data, 8)
    if version != 1 or page not in (2048, 4096, 8192, 16384):
        raise ValueError('This measured-A6L audit supports header v1 only')
    if struct.unpack_from('<I', data, 1644)[0] != 1648:
        raise ValueError('Unexpected header size')
    align = lambda n: (n + page - 1) // page * page
    sections = {}
    offset = page
    for name, size in [('kernel', k), ('ramdisk', r), ('second', s)]:
        if offset + size > len(data):
            raise ValueError(f'Truncated {name}')
        if size:
            sections[name] = {'offset': offset, 'bytes': size,
                              'sha256': sha(data[offset:offset + size])}
        offset += align(size)
    dtbo_size, dtbo_offset = struct.unpack_from('<IQ', data, 1632)
    if dtbo_size:
        if dtbo_offset < offset or dtbo_offset + dtbo_size > len(data):
            raise ValueError('Invalid recovery DTBO bounds')
        sections['recovery_dtbo'] = {'offset': dtbo_offset, 'bytes': dtbo_size,
                                    'sha256': sha(data[dtbo_offset:dtbo_offset + dtbo_size])}
        offset = dtbo_offset + align(dtbo_size)
    return {'page_size': page, 'header_version': version, 'os_version_encoded': osver,
            'kernel_address': ka, 'ramdisk_address': ra, 'second_address': sa,
            'tags_address': tags, 'body_end': offset, 'sections': sections}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('backup', type=Path)
    parser.add_argument('android_source', type=Path)
    parser.add_argument('output', type=Path, help='New, non-existing private analysis directory')
    args = parser.parse_args()
    backup, source, out = [p.resolve() for p in (args.backup, args.android_source, args.output)]
    manifest = json.loads((backup / 'firmware-verification.json').read_text())
    unpack = source / 'system/tools/mkbootimg/unpack_bootimg.py'
    pack = source / 'system/tools/mkbootimg/mkbootimg.py'
    if not unpack.is_file() or not pack.is_file():
        raise ValueError('AOSP boot tools missing')
    out.mkdir(parents=True, exist_ok=False)
    report = {'scope': 'Offline payload/packaging roundtrip; not a boot or signature test',
              'tools': {str(p.relative_to(source)): sha(p.read_bytes()) for p in (unpack, pack)},
              'images': []}
    all_payloads_match = True
    all_bodies_match = True
    for name in ('boot', 'recovery'):
        item = next(x for x in manifest['partitions'] if x['name'] == name)
        with (backup / 'emmc-firmware-prefix.bin').open('rb') as f:
            f.seek(item['offset'])
            original = f.read(item['bytes'])
        if len(original) != item['bytes'] or sha(original) != item['sha256']:
            raise ValueError(f'{name} does not match verified backup')
        before = layout(original)
        work = out / name
        work.mkdir()
        saved = work / 'captured.bin'
        saved.write_bytes(original)
        proc = subprocess.run([sys.executable, str(unpack), '--boot_img', str(saved),
                               '--out', str(work / 'parts'), '--format', 'mkbootimg', '-0'],
                              check=True, capture_output=True)
        raw_args = proc.stdout.split(b'\0')
        if raw_args[-1] == b'':
            raw_args.pop()
        # Empty values (notably the board name) are valid positional arguments.
        pack_args = [arg.decode('utf-8') for arg in raw_args]
        (work / 'mkbootimg-arguments.json').write_text(json.dumps(pack_args, indent=2) + '\n')
        rebuilt_path = work / 'roundtrip-unsigned.img'
        subprocess.run([sys.executable, str(pack), *pack_args, '--output', str(rebuilt_path)],
                       check=True, capture_output=True)
        rebuilt = rebuilt_path.read_bytes()
        after = layout(rebuilt)
        # Current mkbootimg zeros second_addr when second_size is zero. The
        # captured v1 images retain 0x00f00000 there. Preserve that unused field
        # in a separate analysis output, while retaining the raw tool result.
        normalized = bytearray(rebuilt)
        normalizations = []
        if 'second' not in before['sections'] and 'second' not in after['sections']:
            if before['second_address'] != after['second_address']:
                normalized[28:32] = original[28:32]
                normalizations.append({'field': 'unused second_addr', 'offset': 28,
                                       'from': after['second_address'],
                                       'to': before['second_address']})
        normalized_path = work / 'roundtrip-preserved-header.img'
        normalized_path.write_bytes(normalized)
        normalized_layout = layout(normalized)
        payloads_match = before == normalized_layout
        all_payloads_match &= payloads_match
        all_bodies_match &= normalized == original[:before['body_end']]
        common = min(before['body_end'], len(rebuilt), len(original))
        differences = [i for i in range(common) if original[i] != rebuilt[i]]
        header_regions = {'fields_before_id': (0, 576), 'image_id': (576, 608),
                          'extra_cmdline_and_v1_fields': (608, 1648),
                          'header_page_padding': (1648, before['page_size'])}
        entry = {'name': name, 'captured_partition_sha256': sha(original),
                 'rebuilt_sha256': sha(rebuilt), 'captured_partition_bytes': len(original),
                 'rebuilt_bytes': len(rebuilt), 'layout_and_payload_hashes_match': payloads_match,
                 'body_bytes_match': not differences and len(rebuilt) == before['body_end'],
                 'body_difference_count': len(differences),
                 'first_difference_offsets': differences[:20],
                 'header_regions_equal': {n: original[a:b] == rebuilt[a:b]
                                          for n, (a, b) in header_regions.items()},
                 'normalizations': normalizations,
                 'normalized_sha256': sha(normalized),
                 'normalized_body_bytes_match': normalized == original[:before['body_end']],
                 'captured_layout': before, 'rebuilt_layout': after,
                 'original_tail_bytes_not_recreated': len(original) - before['body_end']}
        report['images'].append(entry)
        print(f'{name}: payload/layout match={payloads_match}; body differences={len(differences)}')
    report['all_layouts_and_payloads_match'] = all_payloads_match
    report['all_normalized_bodies_match'] = all_bodies_match
    (out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    if not all_payloads_match or not all_bodies_match:
        raise SystemExit('Payload/layout/body mismatch; inspect report.json')
    print('Offline verification complete. These outputs must not be flashed.')


if __name__ == '__main__':
    main()
