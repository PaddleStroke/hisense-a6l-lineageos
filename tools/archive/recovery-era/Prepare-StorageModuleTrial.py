"""Generate V19 guarded recovery tools from the hash-pinned V18 workflow."""
import hashlib
import json
from pathlib import Path
import py_compile

ROOT = Path(__file__).resolve().parents[1]
TOOLS = ROOT / 'tools'
OUT = ROOT / 'firmware/extracted/recovery-probe-storage-module-v19-20260916'
package = json.loads((OUT / 'report.json').read_text())
assert package['packaging_passed']
assert json.loads((OUT / 'captured-abl-validation.json').read_text())['passed']
candidate = hashlib.sha256((OUT / 'recovery-diagnostic-unsigned.img').read_bytes()).hexdigest()
assert candidate == package['candidate_sha256']
old_hash = 'a983ef25bdca4a06dae88ab74a4db8582b0a4507f01e14ad6bb2a9354c123ee6'
old_previous = '50bd78a2a94a7d151defbe637b28492207bdd6b5f857cc413175b13535658c15'
previous = json.loads((TOOLS / 'diagnostic-user-v18-tools.json').read_text())['files']
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
    source = raw.decode().replace('v18', 'v19').replace('V18', 'V19')
    source = source.replace('recovery-probe-storage-v19-20260916', 'recovery-probe-storage-module-v19-20260916')
    target = name.replace('v18', 'v19').replace('V18', 'V19')
    if name == 'RecoveryTransitionV18.py':
        assert old_previous in source
        source = source.replace(old_previous, old_hash).replace('verified-v17', 'verified-v18').replace('verified V17', 'verified V18')
    else:
        source = source.replace(old_hash, candidate)
    if name.startswith('Inspect-'):
        anchor = 'for mode, filename in ['
        assert source.count(anchor) == 1
        source = source.replace(anchor, anchor + "('install-diagnostic', 'ram-staging/recovery-diagnostic-staged-usb-v18.img'), ")
    if name == 'Collect-ProbeSerial-v18.py':
        # Keep logging through the deferred-driver retry; no extra filming needed.
        assert source.count('int(v) >= 30') == 2
        source = source.replace('int(v) >= 30', 'int(v) >= 32')
    manifest['files'][target] = write(target, source)

for before, after in [('Test-RecoveryTransitionV18.py', 'Test-RecoveryTransitionV19.py'),
                      ('Verify-StorageReadbacks.py', 'Verify-StorageModuleReadbacks.py')]:
    source = (TOOLS / before).read_text().replace('v18', 'v19').replace('V18', 'V19').replace(old_hash, candidate)
    write(after, source)
(TOOLS / 'diagnostic-user-v19-tools.json').write_text(json.dumps(manifest, indent=2) + '\n')
print(json.dumps(manifest, indent=2))
