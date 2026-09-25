"""Copy the V74 candidate preparation to the laptop only when explicitly run; never launches anything on the phone.
Run inside WSL (Windows OpenSSH through interop). Agent v74img, 23 Sep 2026. Stages:
  laptop root: v74c (install V74) + v74r (rollback to V71) tool sets, manifests, Verify-ControlsV74CStage.py, v74c-pins.json
  ram-staging/recovery-diagnostic-staged-usb-v74c.img (V74) and -v74r.img (V71, for the rollback set)
  v74/image/: recovery-diagnostic-unsigned.img, report.json, captured-abl-validation.json, SHA256SUMS"""
import hashlib, json, subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
T = ROOT / 'tools'
REMOTE = '/home/pierrelouis/A6L-usb-20260915'
CONFIG = 'C:/Users/Pierre/Desktop/A6L/tools/a6l-laptop-ssh.conf'
SSH = '/mnt/c/Windows/System32/OpenSSH/ssh.exe'
SCP = '/mnt/c/Windows/System32/OpenSSH/scp.exe'
V74 = '24ede49b0f34b10f216617cb007c757e495fa2df1b6cd1ddd0afd63f9819da62'
V71 = '417164b78c3bc7c43eba3039caab240194b5a69000b78737c50c3503f398bcf9'

def run(argv):
    result = subprocess.run(argv, capture_output=True, text=True, timeout=600, stdin=subprocess.DEVNULL)
    assert result.returncode == 0, (argv, result.stdout, result.stderr)
    return result.stdout
def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()
def win(path):
    text = str(path)
    assert text.startswith('/mnt/c/'), text
    return 'C:/' + text[len('/mnt/c/'):]

image = ROOT / 'firmware/extracted/recovery-v74-candidate-20260923'
old = ROOT / 'firmware/extracted/recovery-v71-candidate-20260921'
assert sha(image / 'recovery-diagnostic-unsigned.img') == V74 and sha(old / 'recovery-diagnostic-unsigned.img') == V71
assert json.loads((image / 'captured-abl-validation.json').read_text())['passed']
(image / 'SHA256SUMS').write_text(''.join(f'{sha(image / n)}  {n}\n' for n in ['recovery-diagnostic-unsigned.img', 'report.json', 'captured-abl-validation.json']))
files = {}
for tag, cand in [('v74c', V74), ('v74r', V71)]:
    manifest = json.loads((T / f'diagnostic-user-{tag}-tools.json').read_text())
    assert manifest['candidate_sha256'] == cand
    for name, digest in manifest['files'].items():
        assert sha(T / name) == digest, name
        files[name] = T / name
    files[f'diagnostic-user-{tag}-tools.json'] = T / f'diagnostic-user-{tag}-tools.json'
files['Verify-ControlsV74CStage.py'] = T / 'Verify-ControlsV74CStage.py'
files['ram-staging/recovery-diagnostic-staged-usb-v74c.img'] = image / 'recovery-diagnostic-unsigned.img'
files['ram-staging/recovery-diagnostic-staged-usb-v74r.img'] = old / 'recovery-diagnostic-unsigned.img'
for n in ['recovery-diagnostic-unsigned.img', 'report.json', 'captured-abl-validation.json', 'SHA256SUMS']:
    files['v74/image/' + n] = image / n
pins = {name: sha(path) for name, path in files.items()}
(T / 'v74c-pins.json').write_text(json.dumps(pins, indent=2) + '\n')
files['v74c-pins.json'] = T / 'v74c-pins.json'

preflight = "import pathlib; r=pathlib.Path('" + REMOTE + "'); assert r.is_dir(); assert not any((r/n).exists() for n in ['capture-diagnostic-install-user-v74c','capture-diagnostic-restore-user-v74c','capture-diagnostic-install-user-v74r','capture-diagnostic-restore-user-v74r']); print('STAGING_ONLY_READY')"
print(run([SSH, '-F', CONFIG, 'a6l-laptop', 'python3 -c "' + preflight + '"']).strip(), flush=True)
for name, path in files.items():
    destination = REMOTE + '/' + name
    check = "import pathlib,hashlib; p=pathlib.Path('" + destination + "'); assert not p.exists() or hashlib.sha256(p.read_bytes()).hexdigest()=='" + sha(path) + "'; p.parent.mkdir(parents=True,exist_ok=True)"
    run([SSH, '-F', CONFIG, 'a6l-laptop', 'python3 -c "' + check + '"'])
    run([SCP, '-F', CONFIG, win(path), 'a6l-laptop:' + destination])
    print('STAGED', name, flush=True)
out = ROOT / 'research/recovery-v74-20260923'; out.mkdir(exist_ok=True)
result = run([SSH, '-F', CONFIG, 'a6l-laptop', 'python3 ' + REMOTE + '/Verify-ControlsV74CStage.py'])
(out / 'laptop-stage-verification.json').write_text(result)
inspect = ''
for tag in ['v74c', 'v74r']:
    inspect += run([SSH, '-F', CONFIG, 'a6l-laptop', 'python3 ' + REMOTE + f'/Inspect-DiagnosticRecovery-user-{tag}.py'])
(out / 'laptop-offline-inspection.txt').write_text(inspect)
print(result, inspect)
