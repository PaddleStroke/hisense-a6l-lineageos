"""Copy the BootM1 preparation to the laptop only when explicitly run; never launches anything on the phone.
Run inside WSL (uses the Windows OpenSSH client through interop, like the earlier Stage scripts)."""
import hashlib, json, subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
T = ROOT / 'tools'
REMOTE = '/home/pierrelouis/A6L-usb-20260915'
CONFIG = 'C:/Users/Pierre/Desktop/A6L/tools/a6l-laptop-ssh.conf'
SSH = '/mnt/c/Windows/System32/OpenSSH/ssh.exe'
SCP = '/mnt/c/Windows/System32/OpenSSH/scp.exe'
BootM1 = '259c43e3f4c32ab95db02467d1bd34f5f0b525414f074583a715acc131f8237e'

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

manifest = json.loads((T / 'diagnostic-user-bootm1-tools.json').read_text())
assert manifest['candidate_sha256'] == BootM1
for name, digest in manifest['files'].items():
    if name != 'diagnostic-user-bootm1-tools.json':
        assert sha(T / name) == digest, name
image = ROOT / 'firmware/extracted/boot-magisk-20260921'
assert sha(image / 'boot-magisk.img') == BootM1 and sha(image / 'restore-stock-boot.img') == '55ad4747ea8d83a32edb09eafb01772a3cb170378331cdea3d45cd5a8cb34dbc'
files = {name: T / name for name in manifest['files']}
files['diagnostic-user-bootm1-tools.json'] = T / 'diagnostic-user-bootm1-tools.json'
files['Verify-ControlsBootM1Stage.py'] = T / 'Verify-ControlsBootM1Stage.py'
files['ram-staging/boot-magisk-staged-bootm1.img'] = image / 'boot-magisk.img'
files['stock-boot/restore-stock-boot.img'] = image / 'restore-stock-boot.img'
pins = {name: sha(path) for name, path in files.items()}
(T / 'bootm1-pins.json').write_text(json.dumps(pins, indent=2) + '\n')
files['bootm1-pins.json'] = T / 'bootm1-pins.json'

preflight = "import pathlib; r=pathlib.Path('" + REMOTE + "'); assert r.is_dir(); assert not (r/'capture-diagnostic-install-user-bootm1').exists(); assert not (r/'capture-diagnostic-restore-user-bootm1').exists(); print('STAGING_ONLY_READY')"
print(run([SSH, '-F', CONFIG, 'a6l-laptop', 'python3 -c "' + preflight + '"']).strip(), flush=True)
for name, path in files.items():
    destination = REMOTE + '/' + name
    check = "import pathlib,hashlib; p=pathlib.Path('" + destination + "'); assert not p.exists() or hashlib.sha256(p.read_bytes()).hexdigest()=='" + sha(path) + "'; p.parent.mkdir(parents=True,exist_ok=True)"
    run([SSH, '-F', CONFIG, 'a6l-laptop', 'python3 -c "' + check + '"'])
    run([SCP, '-F', CONFIG, win(path), 'a6l-laptop:' + destination])
    print('STAGED', name, flush=True)
out = ROOT / 'research/recovery-bootm1-20260921'; out.mkdir(exist_ok=True)
result = run([SSH, '-F', CONFIG, 'a6l-laptop', 'python3 ' + REMOTE + '/Verify-ControlsBootM1Stage.py'])
(out / 'laptop-stage-verification.json').write_text(result)
inspect = run([SSH, '-F', CONFIG, 'a6l-laptop', 'python3 ' + REMOTE + '/Inspect-DiagnosticRecovery-user-bootm1.py'])
(out / 'laptop-offline-inspection.txt').write_text(inspect)
print(result, inspect)
