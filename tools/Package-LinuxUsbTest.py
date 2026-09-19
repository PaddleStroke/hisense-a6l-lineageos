#!/usr/bin/env python3
"""Package only the reviewed laptop runner, collector and instructions."""
import hashlib
from pathlib import Path
import zipfile

root = Path(__file__).resolve().parent.parent
files = {
    'Inspect-A6LLinux.py': root / 'tools/Inspect-A6LLinux.py',
    'Inspect-FastbootNative.py': root / 'tools/Inspect-FastbootNative.py',
    'README.md': root / 'docs/linux-laptop-test.md',
}
archive = root / 'tools/a6l-linux-usb-test.zip'
with zipfile.ZipFile(archive, 'w', compression=zipfile.ZIP_DEFLATED) as package:
    for name, path in files.items():
        package.writestr(name, path.read_bytes())
with zipfile.ZipFile(archive) as package:
    assert set(package.namelist()) == set(files)
    assert package.testzip() is None
    for name, path in files.items():
        assert package.read(name) == path.read_bytes()
print(str(archive))
print('SHA256 ' + hashlib.sha256(archive.read_bytes()).hexdigest())
print('Verified 3 entries; no firmware or capture data included.')
