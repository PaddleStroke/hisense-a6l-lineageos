"""Generate pinned V29 trial tools from the guarded V28 workflow."""
import hashlib
import json
from pathlib import Path
import py_compile
ROOT=Path(__file__).resolve().parents[1]
T=ROOT/'tools'
OUT=ROOT/'firmware/extracted/recovery-probe-storage-stages-v29-20260916'
package=json.loads((OUT/'report.json').read_text())
assert package['packaging_passed']
assert json.loads((OUT/'captured-abl-validation.json').read_text())['passed']
candidate=hashlib.sha256((OUT/'recovery-diagnostic-unsigned.img').read_bytes()).hexdigest()
assert candidate==package['candidate_sha256']
old='da70c8383db73370f75af2838f2701efcab5901a183d8318b7a67c760f3104b6'
old_previous='a52505613120c8635d67947b76f937b6615b96665a408e75a119843183fcbdf6'
previous=json.loads((T/'diagnostic-user-v28-tools.json').read_text())['files']
manifest={'candidate_sha256':candidate,'sources':previous,'files':{}}
def write(name,s):
    p=T/name
    assert not p.exists(),p
    p.write_bytes(s.encode())
    py_compile.compile(str(p),doraise=True)
    return hashlib.sha256(p.read_bytes()).hexdigest()
for name,h in previous.items():
    raw=(T/name).read_bytes();assert hashlib.sha256(raw).hexdigest()==h,name
    s=raw.decode().replace('v28','v29').replace('V28','V29')
    s=s.replace('recovery-probe-storage-hold-v29-20260916','recovery-probe-storage-stages-v29-20260916')
    if name=='RecoveryTransitionV28.py':
        assert old_previous in s
        s=s.replace(old_previous,old).replace('verified-v27','verified-v28').replace('verified V27','verified V28')
    else:
        s=s.replace(old,candidate)
    if name.startswith('Collect-'):
        a='return bool(forks and alive and max(alive) >= forks[0] + 28)'
        b="return bool(b'A6L_CMD0_STAGE_FINISHED outcome=' in data and forks and alive and max(alive) >= forks[0] + 32)"
        assert s.count(a)==1
        s=s.replace(a,b)
    if name.startswith('Inspect-'):
        anchor='for mode, filename in [';assert s.count(anchor)==1
        s=s.replace(anchor,anchor+"('install-diagnostic', 'ram-staging/recovery-diagnostic-staged-usb-v28.img'), ")
    manifest['files'][name.replace('v28','v29').replace('V28','V29')]=write(name.replace('v28','v29').replace('V28','V29'),s)
for before,after in [('Test-RecoveryTransitionV28.py','Test-RecoveryTransitionV29.py'),('Verify-StorageHoldReadbacks.py','Verify-StorageStagesReadbacks.py')]:
    write(after,(T/before).read_text().replace('v28','v29').replace('V28','V29').replace(old,candidate))
(T/'diagnostic-user-v29-tools.json').write_text(json.dumps(manifest,indent=2)+'\n')
print(json.dumps(manifest,indent=2))
