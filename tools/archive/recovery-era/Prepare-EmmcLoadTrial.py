"""Generate the pinned V35 eMMC load-permission trial from the guarded V35 workflow."""
import hashlib
import json
from pathlib import Path
import py_compile
ROOT=Path(__file__).resolve().parents[1]
T=ROOT/'tools'
OUT=ROOT/'firmware/extracted/recovery-probe-emmc-load-v35-20260917'
package=json.loads((OUT/'report.json').read_text())
assert package['packaging_passed']
assert json.loads((OUT/'captured-abl-validation.json').read_text())['passed']
candidate=hashlib.sha256((OUT/'recovery-diagnostic-unsigned.img').read_bytes()).hexdigest()
assert candidate==package['candidate_sha256']
old='31e451b709ead424d93e4623922c9f6549f5229795853123be006611f7fa0706'
old_previous='365b880a19edbf7945bc82ca672cc853ea90fd059aaf9a480bd7e43039666305'
previous=json.loads((T/'diagnostic-user-v34-tools.json').read_text())['files']
manifest={'candidate_sha256':candidate,'sources':previous,'files':{}}

def write(name,s):
    p=T/name
    assert not p.exists(),p
    p.write_bytes(s.encode())
    py_compile.compile(str(p),doraise=True)
    return hashlib.sha256(p.read_bytes()).hexdigest()

for name,h in previous.items():
    raw=(T/name).read_bytes();assert hashlib.sha256(raw).hexdigest()==h,name
    s=raw.decode().replace('v34','v35').replace('V34','V35')
    s=s.replace('recovery-probe-baseline-v35-20260917','recovery-probe-emmc-load-v35-20260917')
    if name=='RecoveryTransitionV34.py':
        assert old_previous in s
        s=s.replace(old_previous,old).replace('verified-v33','verified-v34').replace('verified V33','verified V34')
    else:
        s=s.replace(old,candidate)
    if name.startswith('Inspect-'):
        anchor='for mode, filename in [';assert s.count(anchor)==1
        s=s.replace(anchor,anchor+"('install-diagnostic', 'ram-staging/recovery-diagnostic-staged-usb-v34.img'), ")
    manifest['files'][name.replace('v34','v35').replace('V34','V35')]=write(name.replace('v34','v35').replace('V34','V35'),s)
for before,after in [('Test-RecoveryTransitionV34.py','Test-RecoveryTransitionV35.py'),('Verify-BaselineReadbacks.py','Verify-EmmcLoadReadbacks.py')]:
    write(after,(T/before).read_text().replace('v34','v35').replace('V34','V35').replace(old,candidate))
(T/'diagnostic-user-v35-tools.json').write_text(json.dumps(manifest,indent=2)+'\n')
print(json.dumps(manifest,indent=2))
