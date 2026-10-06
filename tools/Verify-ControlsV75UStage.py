"""Verify the V75-usb recovery staged files only (tool sets v75u = install V75-usb over V74, v75r = roll back to V74); never opens USB.
Agent usbrec, 26 Sep 2026, on the model of Verify-ControlsV74CStage.py. Also installed as Verify-ControlsV75RStage.py (same content),
because Launch-ControlsV75RInstall.py calls that name."""
import hashlib
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parent
V75U = '8ecb8e9e4c5289cfe63c23eb32b749178935a6588673cea3a1b1ebee3c94b304'
V74 = '24ede49b0f34b10f216617cb007c757e495fa2df1b6cd1ddd0afd63f9819da62'
STOCK = '9688e3dfb349186fb60efc057a618aeaf696b030ad52974932ea83a077b31621'

def check(path, digest):
    assert hashlib.sha256(path.read_bytes()).hexdigest() == digest, str(path)

pins = json.loads((ROOT / 'v75u-pins.json').read_text())
for name, digest in pins.items():
    assert not Path(name).is_absolute() and '..' not in Path(name).parts
    check(ROOT / name, digest)
counts = {}
for tag, cand, prev in [('v75u', V75U, V74), ('v75r', V74, V75U)]:
    manifest = json.loads((ROOT / f'diagnostic-user-{tag}-tools.json').read_text())
    assert manifest['candidate_sha256'] == cand and manifest['previous_sha256'] == prev
    for name, digest in manifest['files'].items():
        check(ROOT / name, digest)
    counts[tag] = len(manifest['files'])
v38 = json.loads((ROOT / 'diagnostic-user-v38-tools.json').read_text())
for name, digest in v38['files'].items():
    check(ROOT / name, digest)
check(ROOT / 'ram-staging/recovery-diagnostic-staged-usb-v75u.img', V75U)
check(ROOT / 'ram-staging/recovery-diagnostic-staged-usb-v75r.img', V74)
check(ROOT / 'stock-recovery/restore-stock-recovery.img', STOCK)
check(ROOT / 'v75usb/image/recovery-diagnostic-unsigned.img', V75U)
check(ROOT / 'v74/image/recovery-diagnostic-unsigned.img', V74)          # rollback image kept on the laptop
assert json.loads((ROOT / 'v75usb/image/captured-abl-validation.json').read_text())['passed']
report = json.loads((ROOT / 'v75usb/image/report.json').read_text())
assert report['candidate_sha256'] == V75U and report['previous_sha256'] == V74
listed = []
for line in (ROOT / 'v75usb/image/SHA256SUMS').read_text().splitlines():
    digest, name = line.split(None, 1)
    p = ROOT / 'v75usb/image' / name.strip()
    if p.exists():                                   # SHA256SUMS also lists kernel/ramdisk/dtb parts not shipped
        check(p, digest); listed.append(name.strip())
assert 'recovery-diagnostic-unsigned.img' in listed   # report/validation are covered by v75u-pins.json
print(json.dumps({'passed': True, 'pin_count': len(pins), 'verified': dict(v38_tools=len(v38['files']), **counts),
                  'scope': 'Offline staged-file verification; no USB access or phone actions'}, indent=2))
