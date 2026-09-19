"""Generate pinned V32 trial tools from the guarded V31 workflow."""
import hashlib
import json
from pathlib import Path
import py_compile
ROOT=Path(__file__).resolve().parents[1]
T=ROOT/'tools'
OUT=ROOT/'firmware/extracted/recovery-probe-storage-state-v32-20260917'
package=json.loads((OUT/'report.json').read_text())
assert package['packaging_passed']
assert json.loads((OUT/'captured-abl-validation.json').read_text())['passed']
candidate=hashlib.sha256((OUT/'recovery-diagnostic-unsigned.img').read_bytes()).hexdigest()
assert candidate==package['candidate_sha256']
old='0cb8efe9d42e3c1cd8114a6b6d17dde03a12ebb307a78e46a70419d34258d047'
old_previous='81c874f0a9112c09631bcf8f5250190ddb7ecc3f255317ac178f71ef3bbb03e8'
previous=json.loads((T/'diagnostic-user-v31-tools.json').read_text())['files']
manifest={'candidate_sha256':candidate,'sources':previous,'files':{}}
def write(name,s):
    p=T/name
    assert not p.exists(),p
    p.write_bytes(s.encode())
    py_compile.compile(str(p),doraise=True)
    return hashlib.sha256(p.read_bytes()).hexdigest()
for name,h in previous.items():
    raw=(T/name).read_bytes();assert hashlib.sha256(raw).hexdigest()==h,name
    s=raw.decode().replace('v31','v32').replace('V31','V32')
    s=s.replace('recovery-probe-storage-ordered-v32-20260916','recovery-probe-storage-state-v32-20260917')
    if name=='RecoveryTransitionV31.py':
        assert old_previous in s
        s=s.replace(old_previous,old).replace('verified-v30','verified-v31').replace('verified V30','verified V31')
    else:
        s=s.replace(old,candidate)
    if name.startswith('Inspect-'):
        anchor='for mode, filename in [';assert s.count(anchor)==1
        s=s.replace(anchor,anchor+"('install-diagnostic', 'ram-staging/recovery-diagnostic-staged-usb-v31.img'), ")
    manifest['files'][name.replace('v31','v32').replace('V31','V32')]=write(name.replace('v31','v32').replace('V31','V32'),s)
for before,after in [('Test-RecoveryTransitionV31.py','Test-RecoveryTransitionV32.py'),('Verify-StorageOrderedReadbacks.py','Verify-StorageStateReadbacks.py')]:
    write(after,(T/before).read_text().replace('v31','v32').replace('V31','V32').replace(old,candidate))
(T/'diagnostic-user-v32-tools.json').write_text(json.dumps(manifest,indent=2)+'\n')
print(json.dumps(manifest,indent=2))
