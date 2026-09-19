"""Generate the pinned V34 older-kernel trial from the guarded V33 workflow."""
import hashlib
import json
from pathlib import Path
import py_compile
ROOT=Path(__file__).resolve().parents[1]
T=ROOT/'tools'
OUT=ROOT/'firmware/extracted/recovery-probe-baseline-v34-20260917'
package=json.loads((OUT/'report.json').read_text())
assert package['packaging_passed']
assert json.loads((OUT/'captured-abl-validation.json').read_text())['passed']
candidate=hashlib.sha256((OUT/'recovery-diagnostic-unsigned.img').read_bytes()).hexdigest()
assert candidate==package['candidate_sha256']
old='365b880a19edbf7945bc82ca672cc853ea90fd059aaf9a480bd7e43039666305'
old_previous='47a31604c4d41743406201c85ae5ea7eb68999cf3bb0a41756c3a5bf224f9278'
previous=json.loads((T/'diagnostic-user-v33-tools.json').read_text())['files']
manifest={'candidate_sha256':candidate,'sources':previous,'files':{}}

def write(name,s):
    p=T/name
    assert not p.exists(),p
    p.write_bytes(s.encode())
    py_compile.compile(str(p),doraise=True)
    return hashlib.sha256(p.read_bytes()).hexdigest()

for name,h in previous.items():
    raw=(T/name).read_bytes();assert hashlib.sha256(raw).hexdigest()==h,name
    s=raw.decode().replace('v33','v34').replace('V33','V34')
    s=s.replace('recovery-probe-storage-noreset-v34-20260917','recovery-probe-baseline-v34-20260917')
    if name=='RecoveryTransitionV33.py':
        assert old_previous in s
        s=s.replace(old_previous,old).replace('verified-v32','verified-v33').replace('verified V32','verified V33')
    else:
        s=s.replace(old,candidate)
    if name.startswith('Inspect-'):
        anchor='for mode, filename in [';assert s.count(anchor)==1
        s=s.replace(anchor,anchor+"('install-diagnostic', 'ram-staging/recovery-diagnostic-staged-usb-v33.img'), ")
    if name.startswith('Collect-'):
        before="return bool(b'A6L_CMD0_STAGE_FINISHED outcome=' in data and forks and alive and max(alive) >= forks[0] + 36)"
        assert s.count(before)==1
        s=s.replace(before,"return bool(b'A6L_STORAGE_MODULE_LOAD_END result=0 errno=0' in data and b'A6L_STORAGE_SNAPSHOT_DONE' in data and forks and alive and max(alive) >= forks[0] + 20)")
        s=s.replace('time.monotonic() + 75','time.monotonic() + 60').replace('capture_seconds"] = 75','capture_seconds"] = 60')
    manifest['files'][name.replace('v33','v34').replace('V33','V34')]=write(name.replace('v33','v34').replace('V33','V34'),s)
for before,after in [('Test-RecoveryTransitionV33.py','Test-RecoveryTransitionV34.py'),('Verify-StorageNoResetReadbacks.py','Verify-BaselineReadbacks.py')]:
    write(after,(T/before).read_text().replace('v33','v34').replace('V33','V34').replace(old,candidate))
(T/'diagnostic-user-v34-tools.json').write_text(json.dumps(manifest,indent=2)+'\n')
print(json.dumps(manifest,indent=2))
