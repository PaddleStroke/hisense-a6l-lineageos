"""Read-only verification of the next RAM diagnostic's laptop staging."""
import hashlib,json
from pathlib import Path
root=Path(__file__).resolve().parent
pins=json.loads((root/'graphics-v42-pins.json').read_text())
def check(path,digest):
    assert hashlib.sha256(path.read_bytes()).hexdigest()==digest,str(path)
for name,digest in pins.items():check(root/name,digest)
old=json.loads((root/'diagnostic-user-v38-tools.json').read_text())['files']
for name,digest in old.items():check(root/name,digest)
pkg=root/'android-graphics-v42'
assert json.loads((pkg/'qemu-report.json').read_text())['passed']
files=json.loads((pkg/'manifest.json').read_text())['files']
for name,info in files.items():
    assert not Path(name).is_absolute() and '..' not in Path(name).parts
    check(pkg/'payload'/name,info['sha256'])
print(json.dumps({'pins':pins,'old_files_verified':len(old),'payload_files_verified':len(files),
                  'qemu_passed':True,'phone_test_started':False},indent=2))
