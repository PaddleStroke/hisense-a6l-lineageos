"""Prepare hash-pinned V13 tools from the reviewed unprivileged workflow."""
import hashlib
import json
from pathlib import Path
import py_compile

ROOT = Path(__file__).resolve().parents[1]
TOOLS = ROOT / 'tools'
OUT = ROOT / 'firmware/extracted/recovery-probe-staged-usb-v13-20260916'
old_hash = '95c7ccaea2803f31db413ab905e4e9d045f0149d0b98e6b8627715a15486477d'
package = json.loads((OUT / 'report.json').read_text())
assert package['packaging_passed']
assert json.loads((OUT / 'captured-abl-validation.json').read_text())['passed']
new_hash = hashlib.sha256((OUT / 'recovery-diagnostic-unsigned.img').read_bytes()).hexdigest()
assert new_hash == package['candidate_sha256'] and new_hash != old_hash
previous = json.loads((TOOLS / 'a6l-user-tools-v1-manifest.json').read_text())['files']
previous['DiagnosticRecoveryProtocolV11.py'] = 'ecb5e29933930b9d536949c91cb2704d82b50353249098fc60a3d04d52113690'
previous['Test-DiagnosticRecoveryProtocolV11.py'] = hashlib.sha256((TOOLS / 'Test-DiagnosticRecoveryProtocolV11.py').read_bytes()).hexdigest()
replacements = [('DiagnosticRecoveryProtocolV11', 'DiagnosticRecoveryProtocolV13'),
                ('user-v1', 'user-v13'),
                ('recovery-probe-usb-only-20260916', 'recovery-probe-staged-usb-v13-20260916'),
                ('recovery-diagnostic-usb-only.img', 'recovery-diagnostic-staged-usb-v13.img'),
                (old_hash, new_hash)]
manifest = {'candidate_sha256': new_hash, 'sources': previous, 'files': {}}
for name, digest in previous.items():
    raw = (TOOLS / name).read_bytes()
    assert hashlib.sha256(raw).hexdigest() == digest, name
    source, target_name = raw.decode('utf-8'), name
    for before, after in replacements:
        source = source.replace(before, after)
        target_name = target_name.replace(before, after)
    if name.startswith('Inspect-DiagnosticRecovery'):
        anchor = 'for mode, filename in ['
        assert source.count(anchor) == 1
        source = source.replace(anchor, anchor + "('install-diagnostic', 'ram-staging/recovery-diagnostic-usb-only.img'), ")
    if name.startswith('Write-LaptopDiagnosticRecovery'):
        # Confirm user access to the exact EDL node before uploading a programmer.
        anchor = "        sys.path[:0] = [str(kit / 'deps'), str(kit / 'edl')]"
        assert source.count(anchor) == 1
        access = '''        port = Path('/sys/bus/usb/devices/3-2')
        bus = int((port / 'busnum').read_text())
        device = int((port / 'devnum').read_text())
        node = Path(f'/dev/bus/usb/{bus:03d}/{device:03d}')
        fd = os.open(node, os.O_RDWR | os.O_CLOEXEC)
        os.close(fd)
        report['unprivileged_edl_access'] = str(node)
        save()
'''
        source = source.replace(anchor, access + anchor)
    target = TOOLS / target_name
    assert not target.exists(), target
    target.write_bytes(source.encode('utf-8'))
    py_compile.compile(str(target), doraise=True)
    manifest['files'][target_name] = hashlib.sha256(target.read_bytes()).hexdigest()
(TOOLS / 'diagnostic-user-v13-tools.json').write_text(json.dumps(manifest, indent=2) + '\n')
print(json.dumps(manifest, indent=2))
