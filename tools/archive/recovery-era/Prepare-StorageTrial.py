"""Generate V18 guarded recovery tools from the hash-pinned V17 workflow."""
import hashlib
import json
from pathlib import Path
import py_compile

ROOT = Path(__file__).resolve().parents[1]
TOOLS = ROOT / 'tools'
OUT = ROOT / 'firmware/extracted/recovery-probe-storage-v18-20260916'
package = json.loads((OUT / 'report.json').read_text())
assert package['packaging_passed']
assert json.loads((OUT / 'captured-abl-validation.json').read_text())['passed']
candidate = hashlib.sha256((OUT / 'recovery-diagnostic-unsigned.img').read_bytes()).hexdigest()
assert candidate == package['candidate_sha256']
old_hash = '50bd78a2a94a7d151defbe637b28492207bdd6b5f857cc413175b13535658c15'
old_previous = '5b28bc987c7f0962662d5a7b72693d177b456405b20f998fc3235576225317f5'
previous = json.loads((TOOLS / 'diagnostic-user-v17-tools.json').read_text())['files']
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
    source = raw.decode().replace('v17', 'v18').replace('V17', 'V18')
    source = source.replace('recovery-probe-usb-load-v18-20260916', 'recovery-probe-storage-v18-20260916')
    target = name.replace('v17', 'v18').replace('V17', 'V18')
    if name == 'RecoveryTransitionV17.py':
        assert old_previous in source
        source = source.replace(old_previous, old_hash).replace('verified-v16', 'verified-v17').replace('verified V16', 'verified V17')
    else:
        source = source.replace(old_hash, candidate)
    if name.startswith('Inspect-'):
        anchor = 'for mode, filename in ['
        assert source.count(anchor) == 1
        source = source.replace(anchor, anchor + "('install-diagnostic', 'ram-staging/recovery-diagnostic-staged-usb-v17.img'), ")
    if name == 'Collect-ProbeSerial-v17.py':
        # Keep logging through the deferred-driver retry; no extra filming needed.
        assert source.count('int(v) >= 14') == 2
        source = source.replace('int(v) >= 14', 'int(v) >= 30')
    manifest['files'][target] = write(target, source)

for before, after in [('Test-RecoveryTransitionV17.py', 'Test-RecoveryTransitionV18.py'),
                      ('Verify-UsbLoadReadbacks.py', 'Verify-StorageReadbacks.py')]:
    source = (TOOLS / before).read_text().replace('v17', 'v18').replace('V17', 'V18').replace(old_hash, candidate)
    write(after, source)
(TOOLS / 'diagnostic-user-v18-tools.json').write_text(json.dumps(manifest, indent=2) + '\n')
print(json.dumps(manifest, indent=2))
