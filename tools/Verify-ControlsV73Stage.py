"""Verify V73 staged files only; never open USB or invoke a phone operation."""
import hashlib
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parent
V73 = 'ab556daee50e1ef4ccb6f92f1df0ae3ad75772536a4ae823d3b2aa677e43e0a2'
V72 = '6aa00cd02346a3f4312827bccb1db7df1a6ce3dd5147b721d2e43080ccc05ee4'
STOCK = '9688e3dfb349186fb60efc057a618aeaf696b030ad52974932ea83a077b31621'

def check(path, digest):
    assert hashlib.sha256(path.read_bytes()).hexdigest() == digest, str(path)

pins = json.loads((ROOT / 'v73-pins.json').read_text())
for name, digest in pins.items():
    assert not Path(name).is_absolute() and '..' not in Path(name).parts
    check(ROOT / name, digest)
manifest = json.loads((ROOT / 'diagnostic-user-v73-tools.json').read_text())
assert manifest['candidate_sha256'] == V73 and manifest['previous_sha256'] == V72
for name, digest in manifest['files'].items():
    if name != 'diagnostic-user-v73-tools.json':
        check(ROOT / name, digest)
v38 = json.loads((ROOT / 'diagnostic-user-v38-tools.json').read_text())
for name, digest in v38['files'].items():
    check(ROOT / name, digest)
check(ROOT / 'ram-staging/recovery-diagnostic-staged-usb-v73.img', V73)
check(ROOT / 'stock-recovery/restore-stock-recovery.img', STOCK)
assert json.loads((ROOT / 'v73/captured-abl-validation.json').read_text())['passed']
bundle = json.loads((ROOT / 'v73/bundle/manifest.json').read_text())['files']
for name, digest in bundle.items():
    assert not Path(name).is_absolute() and '..' not in Path(name).parts
    check(ROOT / 'v73/bundle' / name, digest)
print(json.dumps({'passed': True, 'pin_count': len(pins),
                  'verified': {'v38_tools': len(v38['files']), 'v73_tools': len(manifest['files']), 'v73_bundle': len(bundle)},
                  'scope': 'Offline staged-file verification; no USB access or phone actions'}, indent=2))
