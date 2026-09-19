"""Generate pinned V31 trial tools from the guarded V30 workflow."""
import hashlib
import json
from pathlib import Path
import py_compile
ROOT=Path(__file__).resolve().parents[1]
T=ROOT/'tools'
OUT=ROOT/'firmware/extracted/recovery-probe-storage-ordered-v31-20260916'
package=json.loads((OUT/'report.json').read_text())
assert package['packaging_passed']
assert json.loads((OUT/'captured-abl-validation.json').read_text())['passed']
candidate=hashlib.sha256((OUT/'recovery-diagnostic-unsigned.img').read_bytes()).hexdigest()
assert candidate==package['candidate_sha256']
old='81c874f0a9112c09631bcf8f5250190ddb7ecc3f255317ac178f71ef3bbb03e8'
old_previous='95b98dd09d5f12703553640ecb0cf6aa109e3ef6ad7bbd98698385d2dfc9d7b6'
previous=json.loads((T/'diagnostic-user-v30-tools.json').read_text())['files']
manifest={'candidate_sha256':candidate,'sources':previous,'files':{}}
def write(name,s):
    p=T/name
    assert not p.exists(),p
    p.write_bytes(s.encode())
    py_compile.compile(str(p),doraise=True)
    return hashlib.sha256(p.read_bytes()).hexdigest()
for name,h in previous.items():
    raw=(T/name).read_bytes();assert hashlib.sha256(raw).hexdigest()==h,name
    s=raw.decode().replace('v30','v31').replace('V30','V31')
    s=s.replace('recovery-probe-storage-commit-v31-20260916','recovery-probe-storage-ordered-v31-20260916')
    if name=='RecoveryTransitionV30.py':
        assert old_previous in s
        s=s.replace(old_previous,old).replace('verified-v29','verified-v30').replace('verified V29','verified V30')
    else:
        s=s.replace(old,candidate)
    if name.startswith('Inspect-'):
        anchor='for mode, filename in [';assert s.count(anchor)==1
        s=s.replace(anchor,anchor+"('install-diagnostic', 'ram-staging/recovery-diagnostic-staged-usb-v30.img'), ")
    manifest['files'][name.replace('v30','v31').replace('V30','V31')]=write(name.replace('v30','v31').replace('V30','V31'),s)
for before,after in [('Test-RecoveryTransitionV30.py','Test-RecoveryTransitionV31.py'),('Verify-StorageCommitReadbacks.py','Verify-StorageOrderedReadbacks.py')]:
    write(after,(T/before).read_text().replace('v30','v31').replace('V30','V31').replace(old,candidate))
(T/'diagnostic-user-v31-tools.json').write_text(json.dumps(manifest,indent=2)+'\n')
print(json.dumps(manifest,indent=2))
