"""Verify V74 staged files only; never open USB or invoke a phone operation."""
import hashlib
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parent
V74 = '417164b78c3bc7c43eba3039caab240194b5a69000b78737c50c3503f398bcf9'
V73 = 'ab556daee50e1ef4ccb6f92f1df0ae3ad75772536a4ae823d3b2aa677e43e0a2'
STOCK = '9688e3dfb349186fb60efc057a618aeaf696b030ad52974932ea83a077b31621'

def check(path, digest):
    assert hashlib.sha256(path.read_bytes()).hexdigest() == digest, str(path)

pins = json.loads((ROOT / 'v74-pins.json').read_text())
for name, digest in pins.items():
    assert not Path(name).is_absolute() and '..' not in Path(name).parts
    check(ROOT / name, digest)
manifest = json.loads((ROOT / 'diagnostic-user-v74-tools.json').read_text())
assert manifest['candidate_sha256'] == V74 and manifest['previous_sha256'] == V73
for name, digest in manifest['files'].items():
    if name != 'diagnostic-user-v74-tools.json':
        check(ROOT / name, digest)
v38 = json.loads((ROOT / 'diagnostic-user-v38-tools.json').read_text())
for name, digest in v38['files'].items():
    check(ROOT / name, digest)
check(ROOT / 'ram-staging/recovery-diagnostic-staged-usb-v74.img', V74)
check(ROOT / 'stock-recovery/restore-stock-recovery.img', STOCK)
assert json.loads((ROOT / 'v74/captured-abl-validation.json').read_text())['passed']
bundle = json.loads((ROOT / 'v74/bundle/manifest.json').read_text())['files']
for name, digest in bundle.items():
    assert not Path(name).is_absolute() and '..' not in Path(name).parts
    check(ROOT / 'v74/bundle' / name, digest)
print(json.dumps({'passed': True, 'pin_count': len(pins),
                  'verified': {'v38_tools': len(v38['files']), 'v74_tools': len(manifest['files']), 'v74_bundle': len(bundle)},
                  'scope': 'Offline staged-file verification; no USB access or phone actions'}, indent=2))
