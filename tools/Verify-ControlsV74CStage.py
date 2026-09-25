"""Verify the V74 candidate staged files only (tool sets v74c = install V74, v74r = roll back to V71); never opens USB."""
import hashlib
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parent
V74 = '24ede49b0f34b10f216617cb007c757e495fa2df1b6cd1ddd0afd63f9819da62'
V71 = '417164b78c3bc7c43eba3039caab240194b5a69000b78737c50c3503f398bcf9'
STOCK = '9688e3dfb349186fb60efc057a618aeaf696b030ad52974932ea83a077b31621'

def check(path, digest):
    assert hashlib.sha256(path.read_bytes()).hexdigest() == digest, str(path)

pins = json.loads((ROOT / 'v74c-pins.json').read_text())
for name, digest in pins.items():
    assert not Path(name).is_absolute() and '..' not in Path(name).parts
    check(ROOT / name, digest)
counts = {}
for tag, cand, prev in [('v74c', V74, V71), ('v74r', V71, V74)]:
    manifest = json.loads((ROOT / f'diagnostic-user-{tag}-tools.json').read_text())
    assert manifest['candidate_sha256'] == cand and manifest['previous_sha256'] == prev
    for name, digest in manifest['files'].items():
        check(ROOT / name, digest)
    counts[tag] = len(manifest['files'])
v38 = json.loads((ROOT / 'diagnostic-user-v38-tools.json').read_text())
for name, digest in v38['files'].items():
    check(ROOT / name, digest)
check(ROOT / 'ram-staging/recovery-diagnostic-staged-usb-v74c.img', V74)
check(ROOT / 'ram-staging/recovery-diagnostic-staged-usb-v74r.img', V71)
check(ROOT / 'stock-recovery/restore-stock-recovery.img', STOCK)
check(ROOT / 'v74/image/recovery-diagnostic-unsigned.img', V74)
assert json.loads((ROOT / 'v74/image/captured-abl-validation.json').read_text())['passed']
assert json.loads((ROOT / 'v74/image/report.json').read_text())['candidate_sha256'] == V74
for line in (ROOT / 'v74/image/SHA256SUMS').read_text().splitlines():
    digest, name = line.split(None, 1)
    check(ROOT / 'v74/image' / name.strip(), digest)
print(json.dumps({'passed': True, 'pin_count': len(pins), 'verified': dict(v38_tools=len(v38['files']), **counts),
                  'scope': 'Offline staged-file verification; no USB access or phone actions'}, indent=2))
