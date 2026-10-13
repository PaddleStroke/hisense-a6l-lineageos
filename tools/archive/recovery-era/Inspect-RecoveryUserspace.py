#!/usr/bin/env python3
"""Read selected stock recovery configuration files from its verified RAM archive."""
import gzip
import hashlib
from pathlib import Path

root = Path(__file__).resolve().parent.parent
archive_path = root / 'firmware/extracted/boot-roundtrip-20260914-v4/recovery/parts/ramdisk'
compressed = archive_path.read_bytes()
assert hashlib.sha256(compressed).hexdigest() == 'c48cf8b1c6fb998e8708e619b8cd51be0dd43260fe60cc94d351600954127291'
archive = gzip.decompress(compressed)
output = root / 'firmware/extracted/stock-recovery-userspace-20260914'
output.mkdir(exist_ok=True)
wanted = {'default.prop', 'prop.default', 'init.rc', 'init.hmct.recovery.rc', 'init.recovery.qcom.rc',
          'etc/recovery.fstab', 'res/images/recovery_main.png', 'res/images/recovery_main_no_sdcard.png'}
offset = 0
while True:
    assert archive[offset:offset + 6] in (b'070701', b'070702')
    fields = [int(archive[offset + 6 + i * 8:offset + 14 + i * 8], 16) for i in range(13)]
    size, name_size = fields[6], fields[11]
    name_bytes = archive[offset + 110:offset + 110 + name_size]
    assert name_bytes.endswith(b'\0')
    name = name_bytes[:-1].decode()
    start = (offset + 110 + name_size + 3) & ~3
    assert start + size <= len(archive)
    if name == 'TRAILER!!!':
        break
    if name in wanted:
        # Fixed allowlist, flattened filenames, no archive-controlled paths.
        destination = output / name.replace('/', '_')
        destination.write_bytes(archive[start:start + size])
        print(f'{name}: {size} bytes')
        wanted.remove(name)
    offset = (start + size + 3) & ~3
assert not wanted, f'Expected archive members missing: {wanted}'
