"""Generate pinned V26 trial tools from the guarded V25 workflow."""
import hashlib
import json
from pathlib import Path
import py_compile
ROOT=Path(__file__).resolve().parents[1]
T=ROOT/'tools'
OUT=ROOT/'firmware/extracted/recovery-probe-storage-request-v26-20260916'
package=json.loads((OUT/'report.json').read_text())
assert package['packaging_passed']
assert json.loads((OUT/'captured-abl-validation.json').read_text())['passed']
candidate=hashlib.sha256((OUT/'recovery-diagnostic-unsigned.img').read_bytes()).hexdigest()
assert candidate==package['candidate_sha256']
old='911de6c83d41a2be646a70e2b850f7d2aa795e937262c1b44c74ee2259e575e7'
old_previous='6cc9c9940c068057ed58f3a210213b444ef2321eee01e1aa96b082e275a28731'
previous=json.loads((T/'diagnostic-user-v25-tools.json').read_text())['files']
manifest={'candidate_sha256':candidate,'sources':previous,'files':{}}
def write(name,s):
    p=T/name
    assert not p.exists(),p
    p.write_bytes(s.encode())
    py_compile.compile(str(p),doraise=True)
    return hashlib.sha256(p.read_bytes()).hexdigest()
for name,h in previous.items():
    raw=(T/name).read_bytes();assert hashlib.sha256(raw).hexdigest()==h,name
    s=raw.decode().replace('v25','v26').replace('V25','V26')
    s=s.replace('recovery-probe-storage-cmd0-v26-20260916','recovery-probe-storage-request-v26-20260916')
    if name=='RecoveryTransitionV25.py':
        assert old_previous in s
        s=s.replace(old_previous,old).replace('verified-v24','verified-v25').replace('verified V24','verified V25')
    else:
        s=s.replace(old,candidate)
    if name.startswith('Inspect-'):
        anchor='for mode, filename in [';assert s.count(anchor)==1
        s=s.replace(anchor,anchor+"('install-diagnostic', 'ram-staging/recovery-diagnostic-staged-usb-v25.img'), ")
    manifest['files'][name.replace('v25','v26').replace('V25','V26')]=write(name.replace('v25','v26').replace('V25','V26'),s)
for before,after in [('Test-RecoveryTransitionV25.py','Test-RecoveryTransitionV26.py'),('Verify-StorageCmd0Readbacks.py','Verify-StorageRequestReadbacks.py')]:
    write(after,(T/before).read_text().replace('v25','v26').replace('V25','V26').replace(old,candidate))
(T/'diagnostic-user-v26-tools.json').write_text(json.dumps(manifest,indent=2)+'\n')
print(json.dumps(manifest,indent=2))
