"""Generate pinned V24 trial tools; preserve the guarded V23 workflow."""
import hashlib
import json
from pathlib import Path
import py_compile
ROOT=Path(__file__).resolve().parents[1]
T=ROOT/'tools'
OUT=ROOT/'firmware/extracted/recovery-probe-storage-scan-v24-20260916'
package=json.loads((OUT/'report.json').read_text())
assert package['packaging_passed']
assert json.loads((OUT/'captured-abl-validation.json').read_text())['passed']
candidate=hashlib.sha256((OUT/'recovery-diagnostic-unsigned.img').read_bytes()).hexdigest()
assert candidate==package['candidate_sha256']
old='fcc665755df0dfb3324f606701f8072dcdda3cef98227e416c593ba9ff113afa'
old_previous='19ea35f9ce8b97683c195aa2a26fce811170a04064483c539eddaba1782720d3'
previous=json.loads((T/'diagnostic-user-v23-tools.json').read_text())['files']
manifest={'candidate_sha256':candidate,'sources':previous,'files':{}}
def write(name,s):
    p=T/name
    assert not p.exists(),p
    p.write_bytes(s.encode())
    py_compile.compile(str(p),doraise=True)
    return hashlib.sha256(p.read_bytes()).hexdigest()
for name,h in previous.items():
    raw=(T/name).read_bytes();assert hashlib.sha256(raw).hexdigest()==h,name
    s=raw.decode().replace('v23','v24').replace('V23','V24')
    s=s.replace('recovery-probe-storage-host-v24-20260916','recovery-probe-storage-scan-v24-20260916')
    if name=='RecoveryTransitionV23.py':
        assert old_previous in s
        s=s.replace(old_previous,old).replace('verified-v22','verified-v23').replace('verified V22','verified V23')
    else:
        s=s.replace(old,candidate)
    if name.startswith('Inspect-'):
        anchor='for mode, filename in [';assert s.count(anchor)==1
        s=s.replace(anchor,anchor+"('install-diagnostic', 'ram-staging/recovery-diagnostic-staged-usb-v23.img'), ")
    if name=='Collect-ProbeSerial-v23.py':
        assert s.count('forks[0] + 20')==1 and s.count('time.monotonic() + 55')==1
        s=s.replace('forks[0] + 20','forks[0] + 28').replace('time.monotonic() + 55','time.monotonic() + 65').replace('capture_seconds"] = 55','capture_seconds"] = 65')
    manifest['files'][name.replace('v23','v24').replace('V23','V24')]=write(name.replace('v23','v24').replace('V23','V24'),s)
for before,after in [('Test-RecoveryTransitionV23.py','Test-RecoveryTransitionV24.py'),('Verify-StorageHostReadbacks.py','Verify-StorageScanReadbacks.py')]:
    write(after,(T/before).read_text().replace('v23','v24').replace('V23','V24').replace(old,candidate))
(T/'diagnostic-user-v24-tools.json').write_text(json.dumps(manifest,indent=2)+'\n')
print(json.dumps(manifest,indent=2))
