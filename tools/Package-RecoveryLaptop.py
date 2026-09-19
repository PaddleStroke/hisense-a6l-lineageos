#!/usr/bin/env python3
"""Package the inspected EDL reader and pinned Linux libraries, without phone access."""
import hashlib
import importlib.util
import json
from pathlib import Path, PurePosixPath
import shutil
import zipfile

root = Path(__file__).resolve().parent.parent
out = root / 'tools/a6l-recovery-kit'
out.mkdir(exist_ok=False)
(out / 'deps').mkdir()
for wheel in sorted((root / 'tools/a6l-edl-linux-wheels').glob('*.whl')):
    with zipfile.ZipFile(wheel) as archive:
        for name in archive.namelist():
            p = PurePosixPath(name)
            assert not p.is_absolute() and '..' not in p.parts
        archive.extractall(out / 'deps')
docopt = Path(importlib.util.find_spec('docopt').origin)
shutil.copy2(docopt, out / 'deps/docopt.py')
for file in (root / 'tools/edl/edlclient').rglob('*.py'):
    if '__pycache__' in file.parts:
        continue
    target = out / 'edl' / file.relative_to(root / 'tools/edl')
    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(file, target)
for name in ['edl.py', 'LICENSE']:
    shutil.copy2(root / 'tools/edl' / name, out / 'edl' / name)
# The runtime library is installed as .so.0 on Ubuntu; no -dev package is needed.
usb = out / 'edl/edlclient/Library/Connection/usblib.py'
source = usb.read_text()
old = 'usb.backend.libusb1.get_backend(find_library=lambda x: "libusb-1.0.so")'
assert source.count(old) == 1
source = source.replace(old, 'usb.backend.libusb1.get_backend()')
usb.write_text(source)
loader = root / 'firmware/programmer/inspected/USB_Drivers_Hisense_A6L/qpst_qfil_programmer_data/prog_emmc_ufs_firehose_Sdm660_ddr_30060000.elf'
assert hashlib.sha256(loader.read_bytes()).hexdigest() == '6003242582a610712c6b32c8f09475fb78a166e4bb3b018e9f01a7b9bb083642'
shutil.copy2(loader, out / 'programmer.elf')
with (root / 'firmware/raw-backup-20260914/emmc-firmware-prefix.bin').open('rb') as stream:
    (out / 'expected-primary.bin').write_bytes(stream.read(1048576))
shutil.copy2(root / 'firmware/raw-backup-20260914/emmc-gpt-tail.bin', out / 'expected-tail.bin')
shutil.copy2(root / 'tools/Verify-RawBackup.py', out / 'Verify-RawBackup.py')
manifest = {'scope': 'Private recovery kit; no phone operation performed by packaging',
            'edl_base': '2f8e89a848afaaef68997fcbcb5b178d958d497b',
            'edl_changes': ['Existing inspected A6L Windows/Sahara fixes', 'Use default libusb1 lookup on Linux'],
            'files': {}}
for file in sorted(out.rglob('*')):
    if file.is_file():
        manifest['files'][file.relative_to(out).as_posix()] = hashlib.sha256(file.read_bytes()).hexdigest()
(out / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
archive = root / 'tools/a6l-recovery-kit.zip'
with zipfile.ZipFile(archive, 'x', zipfile.ZIP_DEFLATED) as dest:
    for file in out.rglob('*'):
        if file.is_file():
            dest.write(file, file.relative_to(out).as_posix())
print(json.dumps({'file': str(archive), 'bytes': archive.stat().st_size,
                  'sha256': hashlib.sha256(archive.read_bytes()).hexdigest(),
                  'files': len(manifest['files'])}, indent=2))
