"""Copy the V75-usb recovery install/rollback kit to the laptop only when explicitly run; never launches anything on the phone.
Run inside WSL (Windows OpenSSH through interop). Agent usbrec, 26 Sep 2026, on the model of Stage-V74C.py. Stages:
  laptop root: v75u (install V75-usb over V74) + v75r (rollback to V74) tool sets, manifests, Verify-ControlsV75UStage.py,
               Verify-ControlsV75RStage.py (same content; Launch-ControlsV75RInstall.py calls that name), Test-V75UChain.py, v75u-pins.json
  ram-staging/recovery-diagnostic-staged-usb-v75u.img (V75-usb) and -v75r.img (V74, rollback)
  v75usb/image/: recovery-diagnostic-unsigned.img, report.json, captured-abl-validation.json, SHA256SUMS (already there since 24 Sep: must match)
  v75usb/KIT-SHA256SUMS: every staged file, paths relative to ~/A6L-usb-20260915 (cd there; sha256sum -c v75usb/KIT-SHA256SUMS)
The tools live at the laptop root like every earlier kit (they use a6l-recovery-kit/, stock-recovery/, ram-staging/, v38 tools there)."""
import hashlib, json, subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
T = ROOT / 'tools'
REMOTE = '/home/pierrelouis/A6L-usb-20260915'
CONFIG = 'C:/Users/Pierre/Desktop/A6L/tools/a6l-laptop-ssh.conf'
SSH = '/mnt/c/Windows/System32/OpenSSH/ssh.exe'
SCP = '/mnt/c/Windows/System32/OpenSSH/scp.exe'
V75U = '8ecb8e9e4c5289cfe63c23eb32b749178935a6588673cea3a1b1ebee3c94b304'
V74 = '24ede49b0f34b10f216617cb007c757e495fa2df1b6cd1ddd0afd63f9819da62'

def run(argv):
    result = subprocess.run(argv, capture_output=True, text=True, timeout=600, stdin=subprocess.DEVNULL)
    assert result.returncode == 0, (argv, result.stdout, result.stderr)
    return result.stdout.replace('\r', '')
def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()
def win(path):
    text = str(path)
    assert text.startswith('/mnt/c/'), text
    return 'C:/' + text[len('/mnt/c/'):]

image = ROOT / 'firmware/extracted/recovery-v75usb-candidate-20260925'
old = ROOT / 'firmware/extracted/recovery-v74-candidate-20260923'
assert sha(image / 'recovery-diagnostic-unsigned.img') == V75U and sha(old / 'recovery-diagnostic-unsigned.img') == V74
assert json.loads((image / 'captured-abl-validation.json').read_text())['passed']
files = {}
for tag, cand in [('v75u', V75U), ('v75r', V74)]:
    manifest = json.loads((T / f'diagnostic-user-{tag}-tools.json').read_text())
    assert manifest['candidate_sha256'] == cand
    for name, digest in manifest['files'].items():
        assert sha(T / name) == digest, name
        files[name] = T / name
    files[f'diagnostic-user-{tag}-tools.json'] = T / f'diagnostic-user-{tag}-tools.json'
assert (T / 'Verify-ControlsV75UStage.py').read_bytes() == (T / 'Verify-ControlsV75RStage.py').read_bytes()
for n in ['Verify-ControlsV75UStage.py', 'Verify-ControlsV75RStage.py', 'Test-V75UChain.py']:
    files[n] = T / n
files['ram-staging/recovery-diagnostic-staged-usb-v75u.img'] = image / 'recovery-diagnostic-unsigned.img'
files['ram-staging/recovery-diagnostic-staged-usb-v75r.img'] = old / 'recovery-diagnostic-unsigned.img'
for n in ['recovery-diagnostic-unsigned.img', 'report.json', 'captured-abl-validation.json', 'SHA256SUMS']:
    files['v75usb/image/' + n] = image / n
pins = {name: sha(path) for name, path in files.items()}
(T / 'v75u-pins.json').write_text(json.dumps(pins, indent=2) + '\n')
files['v75u-pins.json'] = T / 'v75u-pins.json'
kitsums = T / 'v75u-KIT-SHA256SUMS'
kitsums.write_text(''.join(f'{sha(p)}  {n}\n' for n, p in sorted(files.items())))
files['v75usb/KIT-SHA256SUMS'] = kitsums

preflight = "import pathlib; r=pathlib.Path('" + REMOTE + "'); assert r.is_dir(); assert not any((r/n).exists() for n in ['capture-diagnostic-install-user-v75u','capture-diagnostic-restore-user-v75u','capture-diagnostic-install-user-v75r','capture-diagnostic-restore-user-v75r']); print('STAGING_ONLY_READY')"
print(run([SSH, '-F', CONFIG, 'a6l-laptop', 'python3 -c "' + preflight + '"']).strip(), flush=True)
for name, path in files.items():
    destination = REMOTE + '/' + name
    check = ("import pathlib,hashlib; p=pathlib.Path('" + destination + "'); same=p.exists() and hashlib.sha256(p.read_bytes()).hexdigest()=='" + sha(path) +
             "'; assert same or not p.exists(), 'different file exists'; p.parent.mkdir(parents=True,exist_ok=True); print('SAME' if same else 'NEW')")
    state = run([SSH, '-F', CONFIG, 'a6l-laptop', 'python3 -c "' + check + '"']).strip()
    if state != 'SAME':
        run([SCP, '-F', CONFIG, win(path), 'a6l-laptop:' + destination])
    print('STAGED' if state != 'SAME' else 'ALREADY', name, flush=True)
out = ROOT / 'research/usbrec-20260926'; out.mkdir(exist_ok=True)
checks = {}
for label, cmd in [('verify_v75u', 'python3 Verify-ControlsV75UStage.py'), ('verify_v75r', 'python3 Verify-ControlsV75RStage.py'),
                   ('kit_sums', 'sha256sum -c v75usb/KIT-SHA256SUMS && echo KIT_SHA_OK'),
                   ('image_sums', 'cd v75usb/image && sha256sum -c --ignore-missing SHA256SUMS && echo IMAGE_SHA_OK'),
                   ('inspect_v75u', 'python3 Inspect-DiagnosticRecovery-user-v75u.py'), ('inspect_v75r', 'python3 Inspect-DiagnosticRecovery-user-v75r.py'),
                   ('chain', 'python3 -B Test-V75UChain.py')]:
    r = subprocess.run([SSH, '-F', CONFIG, 'a6l-laptop', 'cd ' + REMOTE + ' && ' + cmd], capture_output=True, text=True, timeout=300, stdin=subprocess.DEVNULL)
    checks[label] = {'rc': r.returncode, 'out': (r.stdout + r.stderr).replace('\r', '')[-3000:]}
    print('==', label, 'rc', r.returncode, flush=True); print(checks[label]['out'][-1200:], flush=True)
(out / 'laptop-stage-checks.json').write_text(json.dumps(checks, indent=2) + '\n')
print('STAGE_V75U', 'PASS' if all(c['rc'] == 0 for c in checks.values()) else 'FAIL')
