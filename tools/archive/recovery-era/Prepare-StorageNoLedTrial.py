"""Generate V22 guarded recovery tools from the pinned V21 workflow."""
import hashlib
import json
from pathlib import Path
import py_compile

ROOT = Path(__file__).resolve().parents[1]
TOOLS = ROOT / 'tools'
OUT = ROOT / 'firmware/extracted/recovery-probe-storage-noled-v22-20260916'
package = json.loads((OUT / 'report.json').read_text())
assert package['packaging_passed']
assert json.loads((OUT / 'captured-abl-validation.json').read_text())['passed']
candidate = hashlib.sha256((OUT / 'recovery-diagnostic-unsigned.img').read_bytes()).hexdigest()
assert candidate == package['candidate_sha256']
old_hash = 'd4fa4f8b5b50e3c7b9e123ad5cde913d5e5000fee3535620af4e50773b13d407'
old_previous = 'c1ba3da846fa31cc9a40a8ba6c2aba434d8711d0f67b795469bf65c6180d4392'
previous = json.loads((TOOLS / 'diagnostic-user-v21-tools.json').read_text())['files']
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
    source = raw.decode().replace('v21', 'v22').replace('V21', 'V22')
    source = source.replace('recovery-probe-storage-core-v22-20260916', 'recovery-probe-storage-noled-v22-20260916')
    target = name.replace('v21', 'v22').replace('V21', 'V22')
    if name == 'RecoveryTransitionV21.py':
        assert old_previous in source
        source = source.replace(old_previous, old_hash).replace('verified-v20', 'verified-v21').replace('verified V20', 'verified V21')
    else:
        source = source.replace(old_hash, candidate)
    if name.startswith('Inspect-'):
        anchor = 'for mode, filename in ['
        assert source.count(anchor) == 1
        source = source.replace(anchor, anchor + "('install-diagnostic', 'ram-staging/recovery-diagnostic-staged-usb-v21.img'), ")
    manifest['files'][target] = write(target, source)

for before, after in [('Test-RecoveryTransitionV21.py', 'Test-RecoveryTransitionV22.py'),
                      ('Verify-StorageCoreReadbacks.py', 'Verify-StorageNoLedReadbacks.py')]:
    source = (TOOLS / before).read_text().replace('v21', 'v22').replace('V21', 'V22').replace(old_hash, candidate)
    write(after, source)
(TOOLS / 'diagnostic-user-v22-tools.json').write_text(json.dumps(manifest, indent=2) + '\n')
print(json.dumps(manifest, indent=2))
