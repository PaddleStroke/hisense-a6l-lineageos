"""Generate the guarded V36 workflow, accepting only stock or exact V35 predecessor."""
import hashlib
import json
from pathlib import Path
import py_compile

ROOT = Path(__file__).resolve().parents[1]
T = ROOT / 'tools'
OUT = ROOT / 'firmware/extracted/recovery-probe-kernel72-v36-20260917'
package = json.loads((OUT / 'report.json').read_text())
assert package['packaging_passed']
assert json.loads((OUT / 'captured-abl-validation.json').read_text())['passed']
candidate = hashlib.sha256((OUT / 'recovery-diagnostic-unsigned.img').read_bytes()).hexdigest()
assert candidate == package['candidate_sha256']
old = '129621db2ac07c8c955426a5a974a4ffc8ba587eb883d0e6c76e6c01a2f76c00'
old_previous = '31e451b709ead424d93e4623922c9f6549f5229795853123be006611f7fa0706'
previous = json.loads((T / 'diagnostic-user-v35-tools.json').read_text())['files']
manifest = dict(candidate_sha256=candidate, sources=previous, files={})


def write(name, s):
    p = T / name
    assert not p.exists(), p
    p.write_bytes(s.encode())
    py_compile.compile(str(p), doraise=True)
    return hashlib.sha256(p.read_bytes()).hexdigest()


def version(s):
    return s.replace('v35', 'v36').replace('V35', 'V36')


for name, h in previous.items():
    raw = (T / name).read_bytes()
    assert hashlib.sha256(raw).hexdigest() == h, name
    s = version(raw.decode()).replace('recovery-probe-emmc-load-v36-20260917', 'recovery-probe-kernel72-v36-20260917')
    if name == 'RecoveryTransitionV35.py':
        assert old_previous in s
        s = s.replace(old_previous, old).replace('verified-v34', 'verified-v35').replace('verified V34', 'verified V35')
    else:
        s = s.replace(old, candidate)
    if name.startswith('Inspect-'):
        anchor = 'for mode, filename in ['
        assert s.count(anchor) == 1
        s = s.replace(anchor, anchor + "('install-diagnostic', 'ram-staging/recovery-diagnostic-staged-usb-v35.img'), ")
    manifest['files'][version(name)] = write(version(name), s)

write('Test-RecoveryTransitionV36.py', version((T / 'Test-RecoveryTransitionV35.py').read_text()))
write('Verify-Kernel72Readbacks.py', version((T / 'Verify-EmmcLoadReadbacks.py').read_text()).replace(old, candidate))
(T / 'diagnostic-user-v36-tools.json').write_text(json.dumps(manifest, indent=2) + '\n')
print(json.dumps(manifest, indent=2))
