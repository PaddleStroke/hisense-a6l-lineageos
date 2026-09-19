#!/usr/bin/env python3
"""Create fresh V10 coordinators with only version/path/hash substitutions."""
import hashlib
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TOOLS = ROOT / 'tools'
OUT = ROOT / 'firmware/extracted/recovery-probe-visible-userspace-20260915'
old_hash = 'd252fa62e3803c877844d6644200d4b91626b93424352291604c1ca50a02a5a1'
package = json.loads((OUT / 'report.json').read_text())
assert package['packaging_passed']
assert json.loads((OUT / 'captured-abl-validation.json').read_text())['passed']
new_hash = hashlib.sha256((OUT / 'recovery-diagnostic-unsigned.img').read_bytes()).hexdigest()
assert new_hash == package['candidate_sha256'] and new_hash != old_hash
previous = json.loads((TOOLS / 'diagnostic-v9-tools.json').read_text())
replacements = [('DiagnosticRecoveryProtocolV8', 'DiagnosticRecoveryProtocolV9'),
                ('DiagnosticV9', 'DiagnosticV10'), ('-v9', '-v10'),
                ('keep-boot-domains', 'visible-userspace'), (old_hash, new_hash)]
manifest = {}
for name, digest in previous.items():
    raw = (TOOLS / name).read_bytes()
    assert hashlib.sha256(raw).hexdigest() == digest, name
    source = raw.decode('utf-8')
    target_name = name
    for before, after in replacements:
        source = source.replace(before, after)
        target_name = target_name.replace(before, after)
    # Also reject V9, which was the current allowed image in the prior preflight.
    if name == 'Inspect-DiagnosticRecovery-v9.py':
        anchor = "for mode, filename in ["
        assert source.count(anchor) == 1
        source = source.replace(anchor, anchor + "('install-diagnostic', 'ram-staging/recovery-diagnostic-keep-boot-domains.img'), ")
    target = TOOLS / target_name
    assert not target.exists(), target
    target.write_bytes(source.encode('utf-8'))
    manifest[target_name] = hashlib.sha256(target.read_bytes()).hexdigest()
(TOOLS / 'diagnostic-v10-tools.json').write_text(json.dumps(manifest, indent=2) + '\n')
print(json.dumps({'candidate_sha256': new_hash, 'tools': manifest}, indent=2))
