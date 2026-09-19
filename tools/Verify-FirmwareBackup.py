"""Verify firmware prefix + GPT tail, intentionally excluding userdata."""
import hashlib
import importlib.util
import json
from pathlib import Path
import sys

spec = importlib.util.spec_from_file_location('rawverify', Path(__file__).with_name('Verify-RawBackup.py'))
rawverify = importlib.util.module_from_spec(spec)
spec.loader.exec_module(rawverify)


class SavedRegions:
    def __init__(self, prefix, tail, tail_offset):
        self.regions = [(0, prefix.stat().st_size, prefix),
                        (tail_offset, tail_offset + tail.stat().st_size, tail)]
        self.position = 0

    def seek(self, offset):
        self.position = offset

    def read(self, count):
        for start, end, path in self.regions:
            if start <= self.position and self.position + count <= end:
                with path.open('rb') as source:
                    source.seek(self.position - start)
                    data = source.read(count)
                self.position += len(data)
                return data
        raise ValueError('Read outside saved firmware regions')


def run(base):
    prefix = base / 'emmc-firmware-prefix.bin'
    tail = base / 'emmc-gpt-tail.bin'
    with prefix.open('rb') as source:
        primary, entries = rawverify.header(source, 1)
    size = (primary['alternate'] + 1) * 512
    parts = primary['partitions']
    userdata = next(p for p in parts if p['name'] == 'userdata')
    grow = next(p for p in parts if p['name'] == 'grow')
    if prefix.stat().st_size != userdata['offset']:
        raise ValueError('Firmware prefix length mismatch')
    if tail.stat().st_size != size - grow['offset']:
        raise ValueError('GPT tail length mismatch')
    regions = SavedRegions(prefix, tail, grow['offset'])
    secondary, entries2 = rawverify.header(regions, primary['alternate'])
    if secondary['alternate'] != 1 or entries != entries2 or primary['disk_guid'] != secondary['disk_guid']:
        raise ValueError('Primary and secondary GPT differ')
    manifest = dict(scope='All GPT partitions except userdata, plus primary/secondary GPT and prefix gaps',
                    excluded=['userdata', 'eMMC hardware boot areas', 'RPMB', 'fuses'],
                    primary_and_secondary_gpt_crc_valid=True, restore_tested=False,
                    disk_bytes=size, regions=[], partitions=[])
    for start, end, path in regions.regions:
        with path.open('rb') as source:
            digest = hashlib.file_digest(source, 'sha256').hexdigest()
        manifest['regions'].append(dict(file=path.name, disk_offset=start, bytes=end-start, sha256=digest))
    for part in parts:
        if part['name'] == 'userdata': continue
        regions.seek(part['offset'])
        digest = hashlib.sha256()
        remaining = part['bytes']
        while remaining:
            block = regions.read(min(remaining, 8 * 1024 * 1024))
            if not block: raise ValueError('Truncated partition')
            digest.update(block)
            remaining -= len(block)
        part['sha256'] = digest.hexdigest()
        second = base / 'independent-read' / (part['name'] + '.bin')
        if second.exists():
            with second.open('rb') as source: second_hash = hashlib.file_digest(source, 'sha256').hexdigest()
            if second.stat().st_size != part['bytes'] or second_hash != part['sha256']:
                raise ValueError('Independent read mismatch: ' + part['name'])
            part['independent_read_matches'] = True
        manifest['partitions'].append(part)
        print('Verified', part['name'], flush=True)
    (base / 'firmware-verification.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print('Verified',len(manifest['partitions']),'partitions; userdata excluded', flush=True)


if __name__ == '__main__':
    run(Path(sys.argv[1]))
