"""Prepare V17 guarded recovery replacement from the exact verified V16 tools."""
import hashlib
import json
from pathlib import Path
import py_compile

ROOT = Path(__file__).resolve().parents[1]
TOOLS = ROOT / 'tools'
OUT = ROOT / 'firmware/extracted/recovery-probe-usb-load-v17-20260916'
package = json.loads((OUT / 'report.json').read_text())
assert package['packaging_passed']
assert json.loads((OUT / 'captured-abl-validation.json').read_text())['passed']
candidate = hashlib.sha256((OUT / 'recovery-diagnostic-unsigned.img').read_bytes()).hexdigest()
assert candidate == package['candidate_sha256']
old_hash = '5b28bc987c7f0962662d5a7b72693d177b456405b20f998fc3235576225317f5'
old_previous = '7dbab370e9580ec3ac25bd24a9edadfedef5334447a754d83b789a72d0b1271d'
previous = json.loads((TOOLS / 'diagnostic-user-v16-tools.json').read_text())['files']
manifest = {'candidate_sha256': candidate, 'sources': previous, 'files': {}}

def write(name, source):
    target = TOOLS / name
    assert not target.exists(), target
    target.write_bytes(source.encode())
    py_compile.compile(str(target), doraise=True)
    return hashlib.sha256(target.read_bytes()).hexdigest()

for name, digest in previous.items():
    raw = (TOOLS / name).read_bytes()
    assert hashlib.sha256(raw).hexdigest() == digest, name
    source = raw.decode().replace('v16', 'v17').replace('V16', 'V17')
    source = source.replace('recovery-probe-staged-usb-v17-20260916', 'recovery-probe-usb-load-v17-20260916')
    target = name.replace('v16', 'v17').replace('V16', 'V17')
    if name == 'RecoveryTransitionV16.py':
        assert old_previous in source
        source = source.replace(old_previous, old_hash).replace('verified-v15', 'verified-v16').replace('verified V15', 'verified V16')
    else:
        source = source.replace(old_hash, candidate)
    if name.startswith('Inspect-'):
        anchor = 'for mode, filename in ['
        assert source.count(anchor) == 1
        source = source.replace(anchor, anchor + "('install-diagnostic', 'ram-staging/recovery-diagnostic-staged-usb-v16.img'), ")
    manifest['files'][target] = write(target, source)

for before, after in [('Test-RecoveryTransitionV16.py', 'Test-RecoveryTransitionV17.py'),
                      ('Verify-UsbEventTraceReadbacks.py', 'Verify-UsbLoadReadbacks.py')]:
    source = (TOOLS / before).read_text().replace('v16', 'v17').replace('V16', 'V17').replace(old_hash, candidate)
    write(after, source)
(TOOLS / 'diagnostic-user-v17-tools.json').write_text(json.dumps(manifest, indent=2) + '\n')
print(json.dumps(manifest, indent=2))
