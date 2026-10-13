"""Copy the V72 preparation to the laptop only when explicitly run; never launches anything on the phone.
Run inside WSL (uses the Windows OpenSSH client through interop, like the earlier Stage scripts)."""
import hashlib, json, subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
T = ROOT / 'tools'
REMOTE = '/home/pierrelouis/A6L-usb-20260915'
CONFIG = 'C:/Users/Pierre/Desktop/A6L/tools/a6l-laptop-ssh.conf'
SSH = '/mnt/c/Windows/System32/OpenSSH/ssh.exe'
SCP = '/mnt/c/Windows/System32/OpenSSH/scp.exe'
V72 = '6aa00cd02346a3f4312827bccb1db7df1a6ce3dd5147b721d2e43080ccc05ee4'

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

manifest = json.loads((T / 'diagnostic-user-v72-tools.json').read_text())
assert manifest['candidate_sha256'] == V72
for name, digest in manifest['files'].items():
    if name != 'diagnostic-user-v72-tools.json':
        assert sha(T / name) == digest, name
image = ROOT / 'firmware/extracted/recovery-rootedstock-20260921'
assert sha(image / 'recovery-diagnostic-unsigned.img') == V72
assert json.loads((image / 'captured-abl-validation.json').read_text())['passed']
bundle = ROOT / 'firmware/extracted/v72-attended-bundle-20260922'
bundle_files = json.loads((bundle / 'manifest.json').read_text())['files']
for name, digest in bundle_files.items():
    assert sha(bundle / name) == digest, name

files = {name: T / name for name in manifest['files'] if name != 'diagnostic-user-v72-tools.json'}
files['diagnostic-user-v72-tools.json'] = T / 'diagnostic-user-v72-tools.json'
files['Verify-ControlsV72Stage.py'] = T / 'Verify-ControlsV72Stage.py'
files['v72/captured-abl-validation.json'] = image / 'captured-abl-validation.json'
files['v72/bundle/manifest.json'] = bundle / 'manifest.json'
for name in bundle_files:
    files['v72/bundle/' + name] = bundle / name
files['ram-staging/recovery-diagnostic-staged-usb-v72.img'] = image / 'recovery-diagnostic-unsigned.img'
pins = {name: sha(path) for name, path in files.items()}
(T / 'v72-pins.json').write_text(json.dumps(pins, indent=2) + '\n')
files['v72-pins.json'] = T / 'v72-pins.json'

preflight = "import pathlib; r=pathlib.Path('" + REMOTE + "'); assert r.is_dir(); assert not (r/'capture-diagnostic-install-user-v72').exists(); assert not (r/'capture-diagnostic-restore-user-v72').exists(); print('STAGING_ONLY_READY')"
print(run([SSH, '-F', CONFIG, 'a6l-laptop', 'python3 -c "' + preflight + '"']).strip(), flush=True)
for name, path in files.items():
    destination = REMOTE + '/' + name
    check = "import pathlib,hashlib; p=pathlib.Path('" + destination + "'); assert not p.exists() or hashlib.sha256(p.read_bytes()).hexdigest()=='" + sha(path) + "'; p.parent.mkdir(parents=True,exist_ok=True)"
    run([SSH, '-F', CONFIG, 'a6l-laptop', 'python3 -c "' + check + '"'])
    run([SCP, '-F', CONFIG, win(path), 'a6l-laptop:' + destination])
    print('STAGED', name, flush=True)
out = ROOT / 'research/recovery-v72-20260921'; out.mkdir(exist_ok=True)
result = run([SSH, '-F', CONFIG, 'a6l-laptop', 'python3 ' + REMOTE + '/Verify-ControlsV72Stage.py'])
(out / 'laptop-stage-verification.json').write_text(result)
inspect = run([SSH, '-F', CONFIG, 'a6l-laptop', 'python3 ' + REMOTE + '/Inspect-DiagnosticRecovery-user-v72.py'])
(out / 'laptop-offline-inspection.txt').write_text(inspect)
print(result, inspect)
