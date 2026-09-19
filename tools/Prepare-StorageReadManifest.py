"""Pin immutable read-only ranges to independent backup and recent GPT reads."""
import hashlib
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BACKUP = ROOT / 'firmware/raw-backup-20260914'
OUT = ROOT / 'firmware/extracted/storage-read-v37-20260917'
manifest = json.loads((BACKUP / 'firmware-verification.json').read_text())
assert manifest['primary_and_secondary_gpt_crc_valid']
assert manifest['disk_bytes'] == 125074145280
ranges = []


def add(name, offset, data, expected):
    digest = hashlib.sha256(data).hexdigest()
    assert digest == expected, name
    assert offset % 512 == 0 and len(data) % 512 == 0
    assert offset + len(data) <= manifest['disk_bytes']
    ranges.append(dict(name=name, offset=offset, bytes=len(data), sha256=digest))


with (BACKUP / 'emmc-firmware-prefix.bin').open('rb') as f:
    data = f.read(1048576)
    recent = ROOT / 'captures/capture-diagnostic-install-user-v36/edl/primary.bin'
    assert data == recent.read_bytes()
    add('gpt-primary', 0, data, '0a170df9e55b8c1e2c7ad971f44c2b05ddfa78efc1b47588ebdbefadfdd396b0')
    for name in ['boot', 'dtbo', 'vbmeta']:
        entry = next(p for p in manifest['partitions'] if p['name'] == name)
        f.seek(entry['offset'])
        data = f.read(entry['bytes'])
        assert data == (BACKUP / 'independent-read' / (name + '.bin')).read_bytes()
        add(name, entry['offset'], data, entry['sha256'])
tail = manifest['regions'][1]
add('gpt-tail', tail['disk_offset'], (BACKUP / tail['file']).read_bytes(), tail['sha256'])
OUT.mkdir(exist_ok=False)
report = dict(disk_bytes=manifest['disk_bytes'], ranges=ranges, passes=2,
              read_chunk_bytes=[131072, 65536], direct_io=True,
              scope='Fixed immutable firmware and GPT ranges only; no user data, calibration, RPMB or device writes')
(OUT / 'read-manifest.json').write_text(json.dumps(report, indent=2) + '\n')
header = '/* Generated from independently verified A6L backups. */\n'
header += '#define A6L_READ_DISK_BYTES 125074145280ULL\n'
header += 'static const struct read_region read_regions[] = {\n'
for p in ranges:
    header += '    {"%s", %dULL, %dULL, "%s"},\n' % (p['name'], p['offset'], p['bytes'], p['sha256'])
header += '};\n'
(ROOT / 'device/hisense/a6l/diagnostic/storage_read_ranges.h').write_bytes(header.encode())
(OUT / 'storage_read_ranges.h').write_bytes(header.encode())
print(json.dumps(report, indent=2))
