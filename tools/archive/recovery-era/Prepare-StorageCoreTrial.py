"""Generate V21 guarded recovery tools from the pinned V20 workflow."""
import hashlib
import json
from pathlib import Path
import py_compile

ROOT = Path(__file__).resolve().parents[1]
TOOLS = ROOT / 'tools'
OUT = ROOT / 'firmware/extracted/recovery-probe-storage-core-v21-20260916'
package = json.loads((OUT / 'report.json').read_text())
assert package['packaging_passed']
assert json.loads((OUT / 'captured-abl-validation.json').read_text())['passed']
candidate = hashlib.sha256((OUT / 'recovery-diagnostic-unsigned.img').read_bytes()).hexdigest()
assert candidate == package['candidate_sha256']
old_hash = 'c1ba3da846fa31cc9a40a8ba6c2aba434d8711d0f67b795469bf65c6180d4392'
old_previous = '9c092eb41e8c894bff5aea6a1df442582cb4378d782e48d3883aafa1d04bed4e'
previous = json.loads((TOOLS / 'diagnostic-user-v20-tools.json').read_text())['files']
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
    source = raw.decode().replace('v20', 'v21').replace('V20', 'V21')
    source = source.replace('recovery-probe-storage-trace-v21-20260916', 'recovery-probe-storage-core-v21-20260916')
    target = name.replace('v20', 'v21').replace('V20', 'V21')
    if name == 'RecoveryTransitionV20.py':
        assert old_previous in source
        source = source.replace(old_previous, old_hash).replace('verified-v19', 'verified-v20').replace('verified V19', 'verified V20')
    else:
        source = source.replace(old_hash, candidate)
    if name.startswith('Inspect-'):
        anchor = 'for mode, filename in ['
        assert source.count(anchor) == 1
        source = source.replace(anchor, anchor + "('install-diagnostic', 'ram-staging/recovery-diagnostic-staged-usb-v20.img'), ")
    manifest['files'][target] = write(target, source)

for before, after in [('Test-RecoveryTransitionV20.py', 'Test-RecoveryTransitionV21.py'),
                      ('Verify-StorageTraceReadbacks.py', 'Verify-StorageCoreReadbacks.py')]:
    source = (TOOLS / before).read_text().replace('v20', 'v21').replace('V20', 'V21').replace(old_hash, candidate)
    write(after, source)
(TOOLS / 'diagnostic-user-v21-tools.json').write_text(json.dumps(manifest, indent=2) + '\n')
print(json.dumps(manifest, indent=2))
