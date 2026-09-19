"""Generate pinned V27 trial tools from the guarded V26 workflow."""
import hashlib
import json
from pathlib import Path
import py_compile
ROOT=Path(__file__).resolve().parents[1]
T=ROOT/'tools'
OUT=ROOT/'firmware/extracted/recovery-probe-storage-nosdio-v27-20260916'
package=json.loads((OUT/'report.json').read_text())
assert package['packaging_passed']
assert json.loads((OUT/'captured-abl-validation.json').read_text())['passed']
candidate=hashlib.sha256((OUT/'recovery-diagnostic-unsigned.img').read_bytes()).hexdigest()
assert candidate==package['candidate_sha256']
old='fdf8ee6e41c49b598b6b03ffec00ab564bc87af0913122b58b2ad5cb3987dcfc'
old_previous='911de6c83d41a2be646a70e2b850f7d2aa795e937262c1b44c74ee2259e575e7'
previous=json.loads((T/'diagnostic-user-v26-tools.json').read_text())['files']
manifest={'candidate_sha256':candidate,'sources':previous,'files':{}}
def write(name,s):
    p=T/name
    assert not p.exists(),p
    p.write_bytes(s.encode())
    py_compile.compile(str(p),doraise=True)
    return hashlib.sha256(p.read_bytes()).hexdigest()
for name,h in previous.items():
    raw=(T/name).read_bytes();assert hashlib.sha256(raw).hexdigest()==h,name
    s=raw.decode().replace('v26','v27').replace('V26','V27')
    s=s.replace('recovery-probe-storage-request-v27-20260916','recovery-probe-storage-nosdio-v27-20260916')
    if name=='RecoveryTransitionV26.py':
        assert old_previous in s
        s=s.replace(old_previous,old).replace('verified-v25','verified-v26').replace('verified V25','verified V26')
    else:
        s=s.replace(old,candidate)
    if name.startswith('Inspect-'):
        anchor='for mode, filename in [';assert s.count(anchor)==1
        s=s.replace(anchor,anchor+"('install-diagnostic', 'ram-staging/recovery-diagnostic-staged-usb-v26.img'), ")
    if name == 'Collect-ProbeSerial-v26.py':
        s=(T/'Collect-ProbeSerial-v26-repeat.py').read_text()
        assert 'return now < (boot_deadline if boot_deadline is not None else menu_deadline)' in s
        s=s.replace('choices=range(10, 601)','choices=range(10, 3601)')
    if name == 'Run-LaptopProbeCapture-user-v26.py':
        s=s.replace("'340s'", "'1880s'").replace('timeout=350', 'timeout=1890').replace("'330'", "'1800'")
    manifest['files'][name.replace('v26','v27').replace('V26','V27')]=write(name.replace('v26','v27').replace('V26','V27'),s)
for before,after in [('Test-RecoveryTransitionV26.py','Test-RecoveryTransitionV27.py'),('Verify-StorageRequestReadbacks.py','Verify-StorageNoSdioReadbacks.py')]:
    write(after,(T/before).read_text().replace('v26','v27').replace('V26','V27').replace(old,candidate))
(T/'diagnostic-user-v27-tools.json').write_text(json.dumps(manifest,indent=2)+'\n')
print(json.dumps(manifest,indent=2))
