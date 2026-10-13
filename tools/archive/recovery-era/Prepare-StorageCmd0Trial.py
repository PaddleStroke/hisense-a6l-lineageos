"""Generate pinned V25 trial tools from the guarded V24 workflow."""
import hashlib
import json
from pathlib import Path
import py_compile
ROOT=Path(__file__).resolve().parents[1]
T=ROOT/'tools'
OUT=ROOT/'firmware/extracted/recovery-probe-storage-cmd0-v25-20260916'
package=json.loads((OUT/'report.json').read_text())
assert package['packaging_passed']
assert json.loads((OUT/'captured-abl-validation.json').read_text())['passed']
candidate=hashlib.sha256((OUT/'recovery-diagnostic-unsigned.img').read_bytes()).hexdigest()
assert candidate==package['candidate_sha256']
old='6cc9c9940c068057ed58f3a210213b444ef2321eee01e1aa96b082e275a28731'
old_previous='fcc665755df0dfb3324f606701f8072dcdda3cef98227e416c593ba9ff113afa'
previous=json.loads((T/'diagnostic-user-v24-tools.json').read_text())['files']
manifest={'candidate_sha256':candidate,'sources':previous,'files':{}}
def write(name,s):
    p=T/name
    assert not p.exists(),p
    p.write_bytes(s.encode())
    py_compile.compile(str(p),doraise=True)
    return hashlib.sha256(p.read_bytes()).hexdigest()
for name,h in previous.items():
    raw=(T/name).read_bytes();assert hashlib.sha256(raw).hexdigest()==h,name
    s=raw.decode().replace('v24','v25').replace('V24','V25')
    s=s.replace('recovery-probe-storage-scan-v25-20260916','recovery-probe-storage-cmd0-v25-20260916')
    if name=='RecoveryTransitionV24.py':
        assert old_previous in s
        s=s.replace(old_previous,old).replace('verified-v23','verified-v24').replace('verified V23','verified V24')
    else:
        s=s.replace(old,candidate)
    if name.startswith('Inspect-'):
        anchor='for mode, filename in [';assert s.count(anchor)==1
        s=s.replace(anchor,anchor+"('install-diagnostic', 'ram-staging/recovery-diagnostic-staged-usb-v24.img'), ")
    manifest['files'][name.replace('v24','v25').replace('V24','V25')]=write(name.replace('v24','v25').replace('V24','V25'),s)
for before,after in [('Test-RecoveryTransitionV24.py','Test-RecoveryTransitionV25.py'),('Verify-StorageScanReadbacks.py','Verify-StorageCmd0Readbacks.py')]:
    write(after,(T/before).read_text().replace('v24','v25').replace('V24','V25').replace(old,candidate))
(T/'diagnostic-user-v25-tools.json').write_text(json.dumps(manifest,indent=2)+'\n')
print(json.dumps(manifest,indent=2))
