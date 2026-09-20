"""Verify V68 staged files only; never open USB or invoke a phone operation."""
import hashlib
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parent
V68 = '2448b101eb780b1630cc6fd7181315da50211de2504b08cd6dad9e34d44a7520'
V46 = 'ce3727dda592065becb883ef3fe663cb3579290edb841854e01f4fc49d02a3c6'
STOCK = '9688e3dfb349186fb60efc057a618aeaf696b030ad52974932ea83a077b31621'

def check(path, digest):
    assert hashlib.sha256(path.read_bytes()).hexdigest() == digest, str(path)

pins = json.loads((ROOT / 'v68-pins.json').read_text())
for name, digest in pins.items():
    assert not Path(name).is_absolute() and '..' not in Path(name).parts
    check(ROOT / name, digest)
manifest = json.loads((ROOT / 'diagnostic-user-v68-tools.json').read_text())
assert manifest['candidate_sha256'] == V68 and manifest['previous_sha256'] == V46
for name, digest in manifest['files'].items():
    if name != 'diagnostic-user-v68-tools.json':
        check(ROOT / name, digest)
v38 = json.loads((ROOT / 'diagnostic-user-v38-tools.json').read_text())
for name, digest in v38['files'].items():
    check(ROOT / name, digest)
check(ROOT / 'ram-staging/recovery-diagnostic-staged-usb-v68.img', V68)
check(ROOT / 'stock-recovery/restore-stock-recovery.img', STOCK)
assert json.loads((ROOT / 'v68/captured-abl-validation.json').read_text())['passed']
bundle = json.loads((ROOT / 'v68/bundle/manifest.json').read_text())['files']
for name, digest in bundle.items():
    assert not Path(name).is_absolute() and '..' not in Path(name).parts
    check(ROOT / 'v68/bundle' / name, digest)
print(json.dumps({'passed': True, 'pin_count': len(pins),
                  'verified': {'v38_tools': len(v38['files']), 'v68_tools': len(manifest['files']), 'v68_bundle': len(bundle)},
                  'scope': 'Offline staged-file verification; no USB access or phone actions'}, indent=2))
