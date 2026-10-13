"""Copy V46 preparation to the laptop only when explicitly run; never launch it."""
import hashlib
import json
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
T = ROOT / 'tools'
OUT = ROOT / 'research/combined-controls-v46-20260918'
REMOTE = '/home/pierrelouis/A6L-usb-20260915'
CONFIG = str(T / 'a6l-laptop-ssh.conf')
SSH = 'C:/Windows/System32/OpenSSH/ssh.exe'
SCP = 'C:/Windows/System32/OpenSSH/scp.exe'
V46 = 'ce3727dda592065becb883ef3fe663cb3579290edb841854e01f4fc49d02a3c6'

def run(argv):
    result = subprocess.run(argv, capture_output=True, text=True, timeout=180)
    assert result.returncode == 0, (argv, result.stdout, result.stderr)
    return result.stdout

def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

manifest = json.loads((T / 'diagnostic-user-v46-tools.json').read_text())
assert manifest['candidate_sha256'] == V46
assert manifest['previous_sha256'] == 'aae86f0da6f15a0c1f6e425e88806edb165289c459462b232f0b8773f5e4ac14'
for name, digest in manifest['files'].items():
    assert sha(T / name) == digest, name

for name in ['Run-ControlsV46.py', 'Create-V46InputNodes.py', 'Verify-ControlsV46Stage.py']:
    assert (T / name).is_file(), name
package = ROOT / 'firmware/extracted/controls-v46-prep-20260918'
for name in ['manifest.json', 'qemu-module-report.json']:
    assert (package / name).is_file(), name
assert json.loads((package / 'qemu-module-report.json').read_text())['passed']
payload_manifest = json.loads((package / 'manifest.json').read_text())['files']
assert payload_manifest
for name, info in payload_manifest.items():
    assert sha(package / 'payload' / name) == info['sha256'], name
image = ROOT / 'firmware/extracted/recovery-controls-v46-20260918'
assert sha(image / 'recovery-diagnostic-unsigned.img') == V46
assert json.loads((image / 'captured-abl-validation.json').read_text())['passed']

files = {name: T / name for name in manifest['files']}
for name in ['diagnostic-user-v46-tools.json', 'Run-ControlsV46.py', 'Create-V46InputNodes.py', 'Verify-ControlsV46Stage.py']:
    files[name] = T / name
for name in ['manifest.json', 'qemu-module-report.json']:
    files['controls-v46/' + name] = package / name
for path in (package / 'payload').iterdir():
    assert path.is_file()
    files['controls-v46/payload/' + path.name] = path
files['controls-v46/captured-abl-validation.json'] = image / 'captured-abl-validation.json'
files['ram-staging/recovery-diagnostic-staged-usb-v46.img'] = image / 'recovery-diagnostic-unsigned.img'
pins = {name: sha(path) for name, path in files.items()}
v45_pins = json.loads((T / 'controls-v45-pins.json').read_text())
for name in ['Wait-AndroidRamReady.py', 'diagnostic-user-v38-tools.json']:
    assert sha(T / name) == v45_pins[name], name
    pins[name] = v45_pins[name]
(T / 'controls-v46-pins.json').write_text(json.dumps(pins, indent=2) + '\n')
files['controls-v46-pins.json'] = T / 'controls-v46-pins.json'

preflight = "import pathlib; r=pathlib.Path('" + REMOTE + "'); assert not (r/'capture-diagnostic-install-user-v46').exists(); assert not (r/'capture-diagnostic-restore-user-v46').exists(); assert not (r/'capture-controls-user-v46').exists(); print('STAGING_ONLY_READY')"
run([SSH, '-F', CONFIG, 'a6l-laptop', "python3 -c \"" + preflight + "\""])
for name, path in files.items():
    destination = REMOTE + '/' + name
    check = "import pathlib,hashlib; p=pathlib.Path('" + destination + "'); assert not p.exists() or hashlib.sha256(p.read_bytes()).hexdigest()=='" + sha(path) + "'; p.parent.mkdir(parents=True,exist_ok=True)"
    run([SSH, '-F', CONFIG, 'a6l-laptop', 'python3 -c "' + check + '"'])
    run([SCP, '-F', CONFIG, str(path), 'a6l-laptop:' + destination])
    print('STAGED', name, flush=True)
OUT.mkdir(exist_ok=False)
result = run([SSH, '-F', CONFIG, 'a6l-laptop', 'python3 ' + REMOTE + '/Verify-ControlsV46Stage.py'])
(OUT / 'laptop-stage-verification.json').write_text(result)
inspect = run([SSH, '-F', CONFIG, 'a6l-laptop', 'python3 ' + REMOTE + '/Inspect-DiagnosticRecovery-user-v46.py'])
(OUT / 'laptop-offline-inspection.txt').write_text(inspect)
print(result, inspect)
