"""Prepare the V14 direct-replacement workflow; never accesses USB."""
import hashlib
import json
from pathlib import Path
import py_compile

ROOT = Path(__file__).resolve().parents[1]
TOOLS = ROOT / 'tools'
OUT = ROOT / 'firmware/extracted/recovery-probe-staged-usb-v14-20260916'
package = json.loads((OUT / 'report.json').read_text())
assert package['packaging_passed']
assert json.loads((OUT / 'captured-abl-validation.json').read_text())['passed']
new_hash = hashlib.sha256((OUT / 'recovery-diagnostic-unsigned.img').read_bytes()).hexdigest()
assert new_hash == package['candidate_sha256']
previous = json.loads((TOOLS / 'diagnostic-user-v13-tools.json').read_text())['files']
replacements = [('DiagnosticRecoveryProtocolV13', 'DiagnosticRecoveryProtocolV14'),
                ('user-v13', 'user-v14'), ('v13-20260916', 'v14-20260916'),
                ('recovery-diagnostic-staged-usb-v13.img', 'recovery-diagnostic-staged-usb-v14.img'),
                ('Collect-ProbeSerial-v1.py', 'Collect-ProbeSerial-v14.py'),
                ('f302bc49910f0c1595590b5dc7ac062f86681ff9b26233142e93cb8e501c661e', new_hash)]
manifest = {'candidate_sha256': new_hash, 'sources': previous, 'files': {}}
for name, digest in previous.items():
    raw = (TOOLS / name).read_bytes()
    assert hashlib.sha256(raw).hexdigest() == digest, name
    source, target_name = raw.decode(), name
    for before, after in replacements:
        source, target_name = source.replace(before, after), target_name.replace(before, after)
    if name.startswith('Write-'):
        source = source.replace('import argparse', 'import argparse\nfrom RecoveryTransitionV14 import verify_transition, RECOVERIES', 1)
        source = source.replace('and digest != RECOVERY_HASH:', 'and digest not in RECOVERIES:')
        source = source.replace('Recovery differs from the verified stock restore image', 'Recovery is not an exact allowed predecessor')
        old = "        if args.mode == 'install-diagnostic' and any((args.output / 'misc-bcb.bin').read_bytes()):\n            raise ValueError('Diagnostic installation requires an entirely zero 4 KiB boot message')"
        assert old in source
        source = source.replace(old, "        if args.mode == 'install-diagnostic':\n            report['transition'] = verify_transition(report['regions']['recovery']['sha256'],\n                (args.output / 'misc-bcb.bin').read_bytes(), (args.output / 'devinfo.bin').read_bytes())")
    if name.startswith('Inspect-'):
        anchor = 'for mode, filename in ['
        assert source.count(anchor) == 1
        source = source.replace(anchor, anchor + "('install-diagnostic', 'ram-staging/recovery-diagnostic-staged-usb-v13.img'), ")
    if name.startswith('Run-'):
        anchor = '    try:\n'
        # Insert at main try, not the nested USB scan or subprocess helper.
        marker = "    try:\n        " + ("install =" if 'ProbeCapture' in name else 'run(')
        assert source.count(marker) == 1
        start = source.index(marker) + len('    try:\n')
        source = source[:start] + "        inhibition = run(['/usr/bin/gnome-session-inhibit', '--list'])['stdout']\n        if not all(word in inhibition for word in ['A6L-v14', 'suspend', 'idle']):\n            raise RuntimeError('V14 requires an active desktop idle/suspend inhibitor')\n" + source[start:]
        source = source.replace('spare/GPT/stock/empty boot message', 'spare/GPT/known predecessor/exact boot message')
    target = TOOLS / target_name
    assert not target.exists(), target
    target.write_bytes(source.encode())
    py_compile.compile(str(target), doraise=True)
    manifest['files'][target_name] = hashlib.sha256(target.read_bytes()).hexdigest()
for name in ['RecoveryTransitionV14.py', 'Collect-ProbeSerial-v14.py']:
    py_compile.compile(str(TOOLS / name), doraise=True)
    manifest['files'][name] = hashlib.sha256((TOOLS / name).read_bytes()).hexdigest()
(TOOLS / 'diagnostic-user-v14-tools.json').write_text(json.dumps(manifest, indent=2) + '\n')
print(json.dumps(manifest, indent=2))
