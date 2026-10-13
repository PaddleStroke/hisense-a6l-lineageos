"""Generate pinned V33 trial tools from the guarded V32 workflow."""
import hashlib
import json
from pathlib import Path
import py_compile
ROOT=Path(__file__).resolve().parents[1]
T=ROOT/'tools'
OUT=ROOT/'firmware/extracted/recovery-probe-storage-noreset-v33-20260917'
package=json.loads((OUT/'report.json').read_text())
assert package['packaging_passed']
assert json.loads((OUT/'captured-abl-validation.json').read_text())['passed']
candidate=hashlib.sha256((OUT/'recovery-diagnostic-unsigned.img').read_bytes()).hexdigest()
assert candidate==package['candidate_sha256']
old='47a31604c4d41743406201c85ae5ea7eb68999cf3bb0a41756c3a5bf224f9278'
old_previous='0cb8efe9d42e3c1cd8114a6b6d17dde03a12ebb307a78e46a70419d34258d047'
previous=json.loads((T/'diagnostic-user-v32-tools.json').read_text())['files']
manifest={'candidate_sha256':candidate,'sources':previous,'files':{}}
def write(name,s):
    p=T/name
    assert not p.exists(),p
    p.write_bytes(s.encode())
    py_compile.compile(str(p),doraise=True)
    return hashlib.sha256(p.read_bytes()).hexdigest()
for name,h in previous.items():
    raw=(T/name).read_bytes();assert hashlib.sha256(raw).hexdigest()==h,name
    s=raw.decode().replace('v32','v33').replace('V32','V33')
    s=s.replace('recovery-probe-storage-state-v33-20260917','recovery-probe-storage-noreset-v33-20260917')
    if name=='RecoveryTransitionV32.py':
        assert old_previous in s
        s=s.replace(old_previous,old).replace('verified-v31','verified-v32').replace('verified V31','verified V32')
    else:
        s=s.replace(old,candidate)
    if name.startswith('Inspect-'):
        anchor='for mode, filename in [';assert s.count(anchor)==1
        s=s.replace(anchor,anchor+"('install-diagnostic', 'ram-staging/recovery-diagnostic-staged-usb-v32.img'), ")
    manifest['files'][name.replace('v32','v33').replace('V32','V33')]=write(name.replace('v32','v33').replace('V32','V33'),s)
for before,after in [('Test-RecoveryTransitionV32.py','Test-RecoveryTransitionV33.py'),('Verify-StorageStateReadbacks.py','Verify-StorageNoResetReadbacks.py')]:
    write(after,(T/before).read_text().replace('v32','v33').replace('V32','V33').replace(old,candidate))
(T/'diagnostic-user-v33-tools.json').write_text(json.dumps(manifest,indent=2)+'\n')
print(json.dumps(manifest,indent=2))
