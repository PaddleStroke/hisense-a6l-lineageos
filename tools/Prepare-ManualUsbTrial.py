"""Prepare the V15 direct-replacement workflow; never accesses USB."""
import hashlib
import json
from pathlib import Path
import py_compile

ROOT = Path(__file__).resolve().parents[1]
TOOLS = ROOT / 'tools'
OUT = ROOT / 'firmware/extracted/recovery-probe-staged-usb-v15-20260916'
package = json.loads((OUT / 'report.json').read_text())
assert package['packaging_passed']
assert json.loads((OUT / 'captured-abl-validation.json').read_text())['passed']
new_hash = hashlib.sha256((OUT / 'recovery-diagnostic-unsigned.img').read_bytes()).hexdigest()
assert new_hash == package['candidate_sha256']
previous = {n: h for n, h in json.loads((TOOLS / 'diagnostic-user-v14-tools.json').read_text())['files'].items() if n not in ['RecoveryTransitionV14.py', 'Collect-ProbeSerial-v14.py']}
replacements = [('DiagnosticRecoveryProtocolV14', 'DiagnosticRecoveryProtocolV15'),
                ('user-v14', 'user-v15'), ('A6L-v14', 'A6L-v15'), ('v14-20260916', 'v15-20260916'),
                ('recovery-diagnostic-staged-usb-v14.img', 'recovery-diagnostic-staged-usb-v15.img'),
                ('Collect-ProbeSerial-v14.py', 'Collect-ProbeSerial-v15.py'),
                ('ca4c452b707524bac03d92626f27322ffd1327646a3eba3b8f735abf72c45971', new_hash)]
manifest = {'candidate_sha256': new_hash, 'sources': previous, 'files': {}}
for name, digest in previous.items():
    raw = (TOOLS / name).read_bytes()
    assert hashlib.sha256(raw).hexdigest() == digest, name
    source, target_name = raw.decode(), name
    for before, after in replacements:
        source, target_name = source.replace(before, after), target_name.replace(before, after)
    source = source.replace('RecoveryTransitionV14', 'RecoveryTransitionV15')
    if name.startswith('Inspect-'):
        anchor = 'for mode, filename in ['
        assert source.count(anchor) == 1
        source = source.replace(anchor, anchor + "('install-diagnostic', 'ram-staging/recovery-diagnostic-staged-usb-v14.img'), ")
    if 'ProbeCapture' in name:
        source = source.replace('250s', '340s').replace("'240'", "'330'").replace('timeout=260', 'timeout=350')
    target = TOOLS / target_name
    assert not target.exists(), target
    target.write_bytes(source.encode())
    py_compile.compile(str(target), doraise=True)
    manifest['files'][target_name] = hashlib.sha256(target.read_bytes()).hexdigest()
for name in ['RecoveryTransitionV15.py', 'Collect-ProbeSerial-v15.py']:
    py_compile.compile(str(TOOLS / name), doraise=True)
    manifest['files'][name] = hashlib.sha256((TOOLS / name).read_bytes()).hexdigest()
(TOOLS / 'diagnostic-user-v15-tools.json').write_text(json.dumps(manifest, indent=2) + '\n')
print(json.dumps(manifest, indent=2))
