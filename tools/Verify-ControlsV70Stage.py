"""Verify V70 staged files only; never open USB or invoke a phone operation."""
import hashlib
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parent
V70 = 'b93771e701d677c58e377feae794a25d3a9c4c1f5cd379bb904910e4661a4373'
V69 = '4dc4861ebcbd30b5f38ab236b3bdb37e57140803dca1b993d4749808c2815eec'
STOCK = '9688e3dfb349186fb60efc057a618aeaf696b030ad52974932ea83a077b31621'

def check(path, digest):
    assert hashlib.sha256(path.read_bytes()).hexdigest() == digest, str(path)

pins = json.loads((ROOT / 'v70-pins.json').read_text())
for name, digest in pins.items():
    assert not Path(name).is_absolute() and '..' not in Path(name).parts
    check(ROOT / name, digest)
manifest = json.loads((ROOT / 'diagnostic-user-v70-tools.json').read_text())
assert manifest['candidate_sha256'] == V70 and manifest['previous_sha256'] == V69
for name, digest in manifest['files'].items():
    if name != 'diagnostic-user-v70-tools.json':
        check(ROOT / name, digest)
v38 = json.loads((ROOT / 'diagnostic-user-v38-tools.json').read_text())
for name, digest in v38['files'].items():
    check(ROOT / name, digest)
check(ROOT / 'ram-staging/recovery-diagnostic-staged-usb-v70.img', V70)
check(ROOT / 'stock-recovery/restore-stock-recovery.img', STOCK)
assert json.loads((ROOT / 'v70/captured-abl-validation.json').read_text())['passed']
bundle = json.loads((ROOT / 'v70/bundle/manifest.json').read_text())['files']
for name, digest in bundle.items():
    assert not Path(name).is_absolute() and '..' not in Path(name).parts
    check(ROOT / 'v70/bundle' / name, digest)
print(json.dumps({'passed': True, 'pin_count': len(pins),
                  'verified': {'v38_tools': len(v38['files']), 'v70_tools': len(manifest['files']), 'v70_bundle': len(bundle)},
                  'scope': 'Offline staged-file verification; no USB access or phone actions'}, indent=2))
