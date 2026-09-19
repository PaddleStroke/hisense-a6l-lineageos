"""Generate isolated V46 recovery/capture tools from hash-pinned V45 sources."""
import hashlib
import json
import py_compile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
T = ROOT / 'tools'
IMAGE = ROOT / 'firmware/extracted/recovery-controls-v46-20260918'
PACKAGE = ROOT / 'firmware/extracted/controls-v46-prep-20260918'
V45 = 'aae86f0da6f15a0c1f6e425e88806edb165289c459462b232f0b8773f5e4ac14'
V46 = 'ce3727dda592065becb883ef3fe663cb3579290edb841854e01f4fc49d02a3c6'

assert json.loads((IMAGE / 'report.json').read_text())['candidate_sha256'] == V46
assert hashlib.sha256((IMAGE / 'recovery-diagnostic-unsigned.img').read_bytes()).hexdigest() == V46
assert json.loads((IMAGE / 'captured-abl-validation.json').read_text())['passed']
pins = json.loads((T / 'controls-v45-pins.json').read_text())
sources = {}
manifest = {'candidate_sha256': V46, 'previous_sha256': V45, 'files': {}, 'sources': sources}

def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def source(name):
    path = T / name
    actual = digest(path)
    if name in pins:
        assert actual == pins[name], name
    sources[name] = actual
    return path.read_text()

def put(name, text):
    path = T / name
    if not path.exists() or path.read_text() != text:
        path.write_text(text)
    py_compile.compile(str(path), doraise=True)
    manifest['files'][name] = digest(path)

def v46(text):
    return text.replace('V45', 'V46').replace('v45', 'v46').replace(V45, V46)

for old, new in [
    ('DiagnosticRecoveryProtocolV45.py', 'DiagnosticRecoveryProtocolV46.py'),
    ('Write-LaptopDiagnosticRecovery-user-v45.py', 'Write-LaptopDiagnosticRecovery-user-v46.py'),
    ('Inspect-DiagnosticRecovery-user-v45.py', 'Inspect-DiagnosticRecovery-user-v46.py'),
    ('Run-LaptopDiagnosticInstall-user-v45.py', 'Run-LaptopDiagnosticInstall-user-v46.py'),
    ('Run-LaptopDiagnosticRestore-user-v45.py', 'Run-LaptopDiagnosticRestore-user-v46.py'),
    ('Verify-ControlsV45Readbacks.py', 'Verify-ControlsV46Readbacks.py'),
    ('Run-LaptopControlsCapture-v45.py', 'Run-LaptopControlsCapture-v46.py'),
    ('Launch-ControlsV45.py', 'Launch-ControlsV46.py'),
    ('Launch-ControlsV45Install.py', 'Launch-ControlsV46Install.py'),
]:
    put(new, v46(source(old)))

inspect = T / 'Inspect-DiagnosticRecovery-user-v46.py'
text = inspect.read_text()
anchor = "('install-diagnostic', 'ram-staging/recovery-diagnostic-staged-usb-v38.img'),"
assert anchor in text
text = text.replace(anchor, anchor + " ('install-diagnostic', 'ram-staging/recovery-diagnostic-staged-usb-v45.img'),", 1)
inspect.write_text(text)
py_compile.compile(str(inspect), doraise=True)
manifest['files']['Inspect-DiagnosticRecovery-user-v46.py'] = digest(inspect)

transition = source('RecoveryTransitionV45.py')
transition = transition.replace("PREVIOUS = '54b0d7b2502b71d8493e4669f2081e46d9ec4ba7a23c275373d735134977cbbb'",
                                f"PREVIOUS = '{V45}'")
transition = transition.replace("RECOVERIES = {STOCK: 'stock', PREVIOUS: 'verified-v38'}",
                                "RECOVERIES = {PREVIOUS: 'verified-v45'}")
transition = transition.replace('Existing recovery is neither exact stock nor the verified V38 image',
                                'Existing recovery is not the verified V45 predecessor')
put('RecoveryTransitionV46.py', transition)

test_transition = v46(source('Test-RecoveryTransitionV45.py'))
test_transition = test_transition.replace('for image in (policy.STOCK, policy.PREVIOUS):',
                                          'for image in (policy.PREVIOUS,):')
test_transition = test_transition.replace('policy.STOCK, bytes(4096), info',
                                          'policy.PREVIOUS, bytes(4096), info')
put('Test-RecoveryTransitionV46.py', test_transition)
test_protocol = v46(source('Test-DiagnosticRecoveryProtocolV45.py'))
test_protocol = test_protocol.replace('recovery-controls-v46-20260917', 'recovery-controls-v46-20260918')
put('Test-DiagnosticRecoveryProtocolV46.py', test_protocol)

put('diagnostic-user-v46-tools.json', json.dumps(manifest, indent=2) + '\n')
print(json.dumps({'prepared': len(manifest['files']), 'candidate_sha256': V46,
                  'previous_sha256': V45, 'scope': 'Offline V46 tools only'}, indent=2))
