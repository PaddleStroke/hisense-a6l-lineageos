"""Generate pinned V30 trial tools from the guarded V29 workflow."""
import hashlib
import json
from pathlib import Path
import py_compile
ROOT=Path(__file__).resolve().parents[1]
T=ROOT/'tools'
OUT=ROOT/'firmware/extracted/recovery-probe-storage-commit-v30-20260916'
package=json.loads((OUT/'report.json').read_text())
assert package['packaging_passed']
assert json.loads((OUT/'captured-abl-validation.json').read_text())['passed']
candidate=hashlib.sha256((OUT/'recovery-diagnostic-unsigned.img').read_bytes()).hexdigest()
assert candidate==package['candidate_sha256']
old='95b98dd09d5f12703553640ecb0cf6aa109e3ef6ad7bbd98698385d2dfc9d7b6'
old_previous='da70c8383db73370f75af2838f2701efcab5901a183d8318b7a67c760f3104b6'
previous=json.loads((T/'diagnostic-user-v29-tools.json').read_text())['files']
manifest={'candidate_sha256':candidate,'sources':previous,'files':{}}
def write(name,s):
    p=T/name
    assert not p.exists(),p
    p.write_bytes(s.encode())
    py_compile.compile(str(p),doraise=True)
    return hashlib.sha256(p.read_bytes()).hexdigest()
for name,h in previous.items():
    raw=(T/name).read_bytes();assert hashlib.sha256(raw).hexdigest()==h,name
    s=raw.decode().replace('v29','v30').replace('V29','V30')
    s=s.replace('recovery-probe-storage-stages-v30-20260916','recovery-probe-storage-commit-v30-20260916')
    if name=='RecoveryTransitionV29.py':
        assert old_previous in s
        s=s.replace(old_previous,old).replace('verified-v28','verified-v29').replace('verified V28','verified V29')
    else:
        s=s.replace(old,candidate)
    if name.startswith('Collect-'):
        assert 'forks[0] + 32' in s
        s=s.replace('forks[0] + 32', 'forks[0] + 36')
        s=s.replace('time.monotonic() + 65','time.monotonic() + 75').replace('capture_seconds"] = 65','capture_seconds"] = 75')
    if name.startswith('Inspect-'):
        anchor='for mode, filename in [';assert s.count(anchor)==1
        s=s.replace(anchor,anchor+"('install-diagnostic', 'ram-staging/recovery-diagnostic-staged-usb-v29.img'), ")
    manifest['files'][name.replace('v29','v30').replace('V29','V30')]=write(name.replace('v29','v30').replace('V29','V30'),s)
for before,after in [('Test-RecoveryTransitionV29.py','Test-RecoveryTransitionV30.py'),('Verify-StorageStagesReadbacks.py','Verify-StorageCommitReadbacks.py')]:
    write(after,(T/before).read_text().replace('v29','v30').replace('V29','V30').replace(old,candidate))
(T/'diagnostic-user-v30-tools.json').write_text(json.dumps(manifest,indent=2)+'\n')
print(json.dumps(manifest,indent=2))
