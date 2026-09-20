"""Copy the V68 preparation to the laptop only when explicitly run; never launches anything on the phone.
Run inside WSL (uses the Windows OpenSSH client through interop, like the earlier Stage scripts)."""
import hashlib, json, subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
T = ROOT / 'tools'
REMOTE = '/home/pierrelouis/A6L-usb-20260915'
CONFIG = 'C:/Users/Pierre/Desktop/A6L/tools/a6l-laptop-ssh.conf'
SSH = '/mnt/c/Windows/System32/OpenSSH/ssh.exe'
SCP = '/mnt/c/Windows/System32/OpenSSH/scp.exe'
V68 = '2448b101eb780b1630cc6fd7181315da50211de2504b08cd6dad9e34d44a7520'

def run(argv):
    result = subprocess.run(argv, capture_output=True, text=True, timeout=300, stdin=subprocess.DEVNULL)
    assert result.returncode == 0, (argv, result.stdout, result.stderr)
    return result.stdout
def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()
def win(path):
    text = str(path)
    assert text.startswith('/mnt/c/'), text
    return 'C:/' + text[len('/mnt/c/'):]

manifest = json.loads((T / 'diagnostic-user-v68-tools.json').read_text())
assert manifest['candidate_sha256'] == V68
for name, digest in manifest['files'].items():
    if name != 'diagnostic-user-v68-tools.json':
        assert sha(T / name) == digest, name
image = ROOT / 'firmware/extracted/recovery-v68-candidate-20260920'
assert sha(image / 'recovery-diagnostic-unsigned.img') == V68
assert json.loads((image / 'captured-abl-validation.json').read_text())['passed']
bundle = ROOT / 'firmware/extracted/v68-attended-bundle-20260920'
bundle_files = json.loads((bundle / 'manifest.json').read_text())['files']
for name, digest in bundle_files.items():
    assert sha(bundle / name) == digest, name

files = {name: T / name for name in manifest['files'] if name != 'diagnostic-user-v68-tools.json'}
files['diagnostic-user-v68-tools.json'] = T / 'diagnostic-user-v68-tools.json'
files['Verify-ControlsV68Stage.py'] = T / 'Verify-ControlsV68Stage.py'
files['v68/captured-abl-validation.json'] = image / 'captured-abl-validation.json'
files['v68/bundle/manifest.json'] = bundle / 'manifest.json'
for name in bundle_files:
    files['v68/bundle/' + name] = bundle / name
files['ram-staging/recovery-diagnostic-staged-usb-v68.img'] = image / 'recovery-diagnostic-unsigned.img'
pins = {name: sha(path) for name, path in files.items()}
(T / 'v68-pins.json').write_text(json.dumps(pins, indent=2) + '\n')
files['v68-pins.json'] = T / 'v68-pins.json'

preflight = "import pathlib; r=pathlib.Path('" + REMOTE + "'); assert r.is_dir(); assert not (r/'capture-diagnostic-install-user-v68').exists(); assert not (r/'capture-diagnostic-restore-user-v68').exists(); print('STAGING_ONLY_READY')"
print(run([SSH, '-F', CONFIG, 'a6l-laptop', 'python3 -c "' + preflight + '"']).strip(), flush=True)
for name, path in files.items():
    destination = REMOTE + '/' + name
    check = "import pathlib,hashlib; p=pathlib.Path('" + destination + "'); assert not p.exists() or hashlib.sha256(p.read_bytes()).hexdigest()=='" + sha(path) + "'; p.parent.mkdir(parents=True,exist_ok=True)"
    run([SSH, '-F', CONFIG, 'a6l-laptop', 'python3 -c "' + check + '"'])
    run([SCP, '-F', CONFIG, win(path), 'a6l-laptop:' + destination])
    print('STAGED', name, flush=True)
out = ROOT / 'research/recovery-v68-20260920'; out.mkdir(exist_ok=True)
result = run([SSH, '-F', CONFIG, 'a6l-laptop', 'python3 ' + REMOTE + '/Verify-ControlsV68Stage.py'])
(out / 'laptop-stage-verification.json').write_text(result)
inspect = run([SSH, '-F', CONFIG, 'a6l-laptop', 'python3 ' + REMOTE + '/Inspect-DiagnosticRecovery-user-v68.py'])
(out / 'laptop-offline-inspection.txt').write_text(inspect)
print(result, inspect)
