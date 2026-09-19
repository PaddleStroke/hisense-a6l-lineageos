"""Verify V46 staged files only; never open USB or invoke a phone operation."""
import hashlib
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parent
V46 = 'ce3727dda592065becb883ef3fe663cb3579290edb841854e01f4fc49d02a3c6'
STOCK = '9688e3dfb349186fb60efc057a618aeaf696b030ad52974932ea83a077b31621'

def check(path, digest):
    assert hashlib.sha256(path.read_bytes()).hexdigest() == digest, str(path)

pins = json.loads((ROOT / 'controls-v46-pins.json').read_text())
for name, digest in pins.items():
    assert not Path(name).is_absolute() and '..' not in Path(name).parts
    check(ROOT / name, digest)

manifest = json.loads((ROOT / 'diagnostic-user-v46-tools.json').read_text())
assert manifest['candidate_sha256'] == V46
assert manifest['previous_sha256'] == 'aae86f0da6f15a0c1f6e425e88806edb165289c459462b232f0b8773f5e4ac14'
for name, digest in manifest['files'].items():
    check(ROOT / name, digest)

v38 = json.loads((ROOT / 'diagnostic-user-v38-tools.json').read_text())
for name, digest in v38['files'].items():
    check(ROOT / name, digest)

check(ROOT / 'ram-staging/recovery-diagnostic-staged-usb-v46.img', V46)
check(ROOT / 'stock-recovery/restore-stock-recovery.img', STOCK)
assert json.loads((ROOT / 'controls-v46/captured-abl-validation.json').read_text())['passed']
package = ROOT / 'controls-v46'
assert json.loads((package / 'qemu-module-report.json').read_text())['passed']
payload = json.loads((package / 'manifest.json').read_text())['files']
for name, info in payload.items():
    assert not Path(name).is_absolute() and '..' not in Path(name).parts
    check(package / 'payload' / name, info['sha256'])

print(json.dumps({'passed': True, 'pin_count': len(pins),
                  'verified': {'v38_tools': len(v38['files']), 'v46_tools': len(manifest['files']), 'v46_payload': len(payload)},
                  'scope': 'Offline staged-file verification; no USB access or phone actions'}, indent=2))
