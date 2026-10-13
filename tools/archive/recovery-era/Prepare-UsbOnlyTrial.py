#!/usr/bin/env python3
"""Prepare fresh V12 tools without changing guarded flash geometry or protocol."""
import hashlib
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TOOLS = ROOT / 'tools'
OUT = ROOT / 'firmware/extracted/recovery-probe-usb-only-20260916'
old_hash = '54ec8ad057bbb4bd52c63ad2c82ee68efd0a4828ff674cfdc51acbb41a12259a'
package = json.loads((OUT / 'report.json').read_text())
assert package['packaging_passed']
assert json.loads((OUT / 'captured-abl-validation.json').read_text())['passed']
new_hash = hashlib.sha256((OUT / 'recovery-diagnostic-unsigned.img').read_bytes()).hexdigest()
assert new_hash == package['candidate_sha256'] and new_hash != old_hash
previous = json.loads((TOOLS / 'diagnostic-v11-tools.json').read_text())
replacements = [('DiagnosticRecoveryProtocolV10', 'DiagnosticRecoveryProtocolV11'),
                ('DiagnosticV11', 'DiagnosticV12'), ('-v11', '-v12'),
                ('recovery-probe-regulator-constraints-20260916', 'recovery-probe-usb-only-20260916'),
                ('regulator-constraints', 'usb-only'), (old_hash, new_hash)]
manifest = {}
for name, digest in previous.items():
    raw = (TOOLS / name).read_bytes()
    assert hashlib.sha256(raw).hexdigest() == digest, name
    source = raw.decode('utf-8')
    target_name = name
    for before, after in replacements:
        source = source.replace(before, after)
        target_name = target_name.replace(before, after)
    if name == 'Inspect-DiagnosticRecovery-v11.py':
        anchor = 'for mode, filename in ['
        assert source.count(anchor) == 1
        source = source.replace(anchor, anchor + "('install-diagnostic', 'ram-staging/recovery-diagnostic-regulator-constraints.img'), ")
    target = TOOLS / target_name
    assert not target.exists(), target
    target.write_bytes(source.encode('utf-8'))
    manifest[target_name] = hashlib.sha256(target.read_bytes()).hexdigest()
(TOOLS / 'diagnostic-v12-tools.json').write_text(json.dumps(manifest, indent=2) + '\n')
print(json.dumps({'candidate_sha256': new_hash, 'tool_count': len(manifest)}, indent=2))
