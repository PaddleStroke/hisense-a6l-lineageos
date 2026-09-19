"""Generate V23 guarded recovery tools from the pinned V22 workflow."""
import hashlib
import json
from pathlib import Path
import py_compile

ROOT = Path(__file__).resolve().parents[1]
TOOLS = ROOT / 'tools'
OUT = ROOT / 'firmware/extracted/recovery-probe-storage-host-v23-20260916'
package = json.loads((OUT / 'report.json').read_text())
assert package['packaging_passed']
assert json.loads((OUT / 'captured-abl-validation.json').read_text())['passed']
candidate = hashlib.sha256((OUT / 'recovery-diagnostic-unsigned.img').read_bytes()).hexdigest()
assert candidate == package['candidate_sha256']
old_hash = '19ea35f9ce8b97683c195aa2a26fce811170a04064483c539eddaba1782720d3'
old_previous = 'd4fa4f8b5b50e3c7b9e123ad5cde913d5e5000fee3535620af4e50773b13d407'
previous = json.loads((TOOLS / 'diagnostic-user-v22-tools.json').read_text())['files']
manifest = {'candidate_sha256': candidate, 'sources': previous, 'files': {}}

def write(name, source):
    target = TOOLS / name
    if target.exists():
        assert target.read_bytes() == source.encode(), target
    else:
        target.write_bytes(source.encode())
    py_compile.compile(str(target), doraise=True)
    return hashlib.sha256(target.read_bytes()).hexdigest()

for name, digest in previous.items():
    raw = (TOOLS / name).read_bytes()
    assert hashlib.sha256(raw).hexdigest() == digest, name
    source = raw.decode().replace('v22', 'v23').replace('V22', 'V23')
    source = source.replace('recovery-probe-storage-noled-v23-20260916', 'recovery-probe-storage-host-v23-20260916')
    target = name.replace('v22', 'v23').replace('V22', 'V23')
    if name == 'RecoveryTransitionV22.py':
        assert old_previous in source
        source = source.replace(old_previous, old_hash).replace('verified-v21', 'verified-v22').replace('verified V21', 'verified V22')
    else:
        source = source.replace(old_hash, candidate)
    if name.startswith('Inspect-'):
        anchor = 'for mode, filename in ['
        assert source.count(anchor) == 1
        source = source.replace(anchor, anchor + "('install-diagnostic', 'ram-staging/recovery-diagnostic-staged-usb-v22.img'), ")
    if name == 'Collect-ProbeSerial-v22.py':
        from Patch_StorageHostCollector import patch_collector
        source = patch_collector(source)
    manifest['files'][target] = write(target, source)

for before, after in [('Test-RecoveryTransitionV22.py', 'Test-RecoveryTransitionV23.py'),
                      ('Verify-StorageNoLedReadbacks.py', 'Verify-StorageHostReadbacks.py')]:
    source = (TOOLS / before).read_text().replace('v22', 'v23').replace('V22', 'V23').replace(old_hash, candidate)
    write(after, source)
(TOOLS / 'diagnostic-user-v23-tools.json').write_text(json.dumps(manifest, indent=2) + '\n')
print(json.dumps(manifest, indent=2))
