"""Generate V20 guarded recovery tools from the pinned V19 workflow."""
import hashlib
import json
from pathlib import Path
import py_compile

ROOT = Path(__file__).resolve().parents[1]
TOOLS = ROOT / 'tools'
OUT = ROOT / 'firmware/extracted/recovery-probe-storage-trace-v20-20260916'
package = json.loads((OUT / 'report.json').read_text())
assert package['packaging_passed']
assert json.loads((OUT / 'captured-abl-validation.json').read_text())['passed']
candidate = hashlib.sha256((OUT / 'recovery-diagnostic-unsigned.img').read_bytes()).hexdigest()
assert candidate == package['candidate_sha256']
old_hash = '9c092eb41e8c894bff5aea6a1df442582cb4378d782e48d3883aafa1d04bed4e'
old_previous = 'a983ef25bdca4a06dae88ab74a4db8582b0a4507f01e14ad6bb2a9354c123ee6'
previous = json.loads((TOOLS / 'diagnostic-user-v19-tools.json').read_text())['files']
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
    source = raw.decode().replace('v19', 'v20').replace('V19', 'V20')
    source = source.replace('recovery-probe-storage-module-v20-20260916', 'recovery-probe-storage-trace-v20-20260916')
    target = name.replace('v19', 'v20').replace('V19', 'V20')
    if name == 'RecoveryTransitionV19.py':
        assert old_previous in source
        source = source.replace(old_previous, old_hash).replace('verified-v18', 'verified-v19').replace('verified V18', 'verified V19')
    else:
        source = source.replace(old_hash, candidate)
    if name.startswith('Inspect-'):
        anchor = 'for mode, filename in ['
        assert source.count(anchor) == 1
        source = source.replace(anchor, anchor + "('install-diagnostic', 'ram-staging/recovery-diagnostic-staged-usb-v19.img'), ")
    if name == 'Collect-ProbeSerial-v19.py':
        assert source.count('int(v) >= 32') == 2
        source = source.replace('int(v) >= 32', 'int(v) >= 40')
        source = source.replace('boot_deadline = time.monotonic() + 45', 'boot_deadline = time.monotonic() + 55')
        source = source.replace('report["post_selection_capture_seconds"] = 45', 'report["post_selection_capture_seconds"] = 55')
    manifest['files'][target] = write(target, source)

for before, after in [('Test-RecoveryTransitionV19.py', 'Test-RecoveryTransitionV20.py'),
                      ('Verify-StorageModuleReadbacks.py', 'Verify-StorageTraceReadbacks.py')]:
    source = (TOOLS / before).read_text().replace('v19', 'v20').replace('V19', 'V20').replace(old_hash, candidate)
    write(after, source)
(TOOLS / 'diagnostic-user-v20-tools.json').write_text(json.dumps(manifest, indent=2) + '\n')
print(json.dumps(manifest, indent=2))
