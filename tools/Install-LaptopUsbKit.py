#!/usr/bin/env python3
"""Verify and extract the exact three-file diagnostic kit on the laptop."""
import hashlib
import json
from pathlib import Path
import zipfile

directory = Path('/home/pierrelouis/A6L-usb-20260915')
archive = directory / 'a6l-linux-usb-test.zip'
expected = '5d09ee1033d65154c4a9500cb8e8ecacb410c3cf85d5d1c43929aaa07d68e1a7'
assert hashlib.sha256(archive.read_bytes()).hexdigest() == expected, 'Archive hash differs'
with zipfile.ZipFile(archive) as package:
    assert set(package.namelist()) == {'Inspect-A6LLinux.py', 'Inspect-FastbootNative.py', 'README.md'}
    assert package.testzip() is None
    hashes = {}
    for name in package.namelist():
        data = package.read(name)
        with (directory / name).open('xb') as handle:
            handle.write(data)
        hashes[name] = hashlib.sha256(data).hexdigest()
print(json.dumps({'directory': str(directory), 'archive_sha256': expected, 'files_sha256': hashes}, indent=2))
