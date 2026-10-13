"""Generate pinned V28 trial tools from the guarded V27 workflow."""
import hashlib
import json
from pathlib import Path
import py_compile
ROOT=Path(__file__).resolve().parents[1]
T=ROOT/'tools'
OUT=ROOT/'firmware/extracted/recovery-probe-storage-hold-v28-20260916'
package=json.loads((OUT/'report.json').read_text())
assert package['packaging_passed']
assert json.loads((OUT/'captured-abl-validation.json').read_text())['passed']
candidate=hashlib.sha256((OUT/'recovery-diagnostic-unsigned.img').read_bytes()).hexdigest()
assert candidate==package['candidate_sha256']
old='a52505613120c8635d67947b76f937b6615b96665a408e75a119843183fcbdf6'
old_previous='fdf8ee6e41c49b598b6b03ffec00ab564bc87af0913122b58b2ad5cb3987dcfc'
previous=json.loads((T/'diagnostic-user-v27-tools.json').read_text())['files']
manifest={'candidate_sha256':candidate,'sources':previous,'files':{}}
def write(name,s):
    p=T/name
    assert not p.exists(),p
    p.write_bytes(s.encode())
    py_compile.compile(str(p),doraise=True)
    return hashlib.sha256(p.read_bytes()).hexdigest()
for name,h in previous.items():
    raw=(T/name).read_bytes();assert hashlib.sha256(raw).hexdigest()==h,name
    s=raw.decode().replace('v27','v28').replace('V27','V28')
    s=s.replace('recovery-probe-storage-nosdio-v28-20260916','recovery-probe-storage-hold-v28-20260916')
    if name=='RecoveryTransitionV27.py':
        assert old_previous in s
        s=s.replace(old_previous,old).replace('verified-v26','verified-v27').replace('verified V26','verified V27')
    else:
        s=s.replace(old,candidate)
    if name.startswith('Inspect-'):
        anchor='for mode, filename in [';assert s.count(anchor)==1
        s=s.replace(anchor,anchor+"('install-diagnostic', 'ram-staging/recovery-diagnostic-staged-usb-v27.img'), ")
    manifest['files'][name.replace('v27','v28').replace('V27','V28')]=write(name.replace('v27','v28').replace('V27','V28'),s)
for before,after in [('Test-RecoveryTransitionV27.py','Test-RecoveryTransitionV28.py'),('Verify-StorageNoSdioReadbacks.py','Verify-StorageHoldReadbacks.py')]:
    write(after,(T/before).read_text().replace('v27','v28').replace('V27','V28').replace(old,candidate))
(T/'diagnostic-user-v28-tools.json').write_text(json.dumps(manifest,indent=2)+'\n')
print(json.dumps(manifest,indent=2))
