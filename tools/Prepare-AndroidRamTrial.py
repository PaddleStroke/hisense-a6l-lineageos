"""Generate one-shot V38 install/capture/restore tools from pinned V37 tools."""
import hashlib
import json
from pathlib import Path
import py_compile
ROOT=Path(__file__).resolve().parents[1]
T=ROOT/'tools'
OUT=ROOT/'firmware/extracted/recovery-probe-android-ram-v38-20260917'
package=json.loads((OUT/'report.json').read_text())
assert package['packaging_passed']
assert json.loads((OUT/'captured-abl-validation.json').read_text())['passed']
candidate=hashlib.sha256((OUT/'recovery-diagnostic-unsigned.img').read_bytes()).hexdigest()
assert candidate==package['candidate_sha256']
old='7de9a96229140b066aed2724ccf2cb14b1a09cc9e44a12bfb25ee4543761b8e9'
old_previous='fa6319e61da7347b28f76527f8ff75dfeba2a6b3c7067de564ca85fe9123018e'
previous=json.loads((T/'diagnostic-user-v37-tools.json').read_text())['files']
manifest=dict(candidate_sha256=candidate,sources=previous,files={})
def write(name,s):
 p=T/name
 assert not p.exists(),p
 p.write_bytes(s.encode())
 py_compile.compile(str(p),doraise=True)
 return hashlib.sha256(p.read_bytes()).hexdigest()
def version(s): return s.replace('v37','v38').replace('V37','V38')
for name,h in previous.items():
 raw=(T/name).read_bytes()
 assert hashlib.sha256(raw).hexdigest()==h,name
 s=version(raw.decode()).replace('recovery-probe-storage-read-v38-20260917', 'recovery-probe-android-ram-v38-20260917')
 if name=='RecoveryTransitionV37.py':
  s=s.replace(old_previous,old).replace('verified-v36','verified-v37').replace('verified V36','verified V37')
 else: s=s.replace(old,candidate)
 if name.startswith('Inspect-'):
  anchor='for mode, filename in ['
  assert s.count(anchor)==1
  s=s.replace(anchor,anchor+"('install-diagnostic', 'ram-staging/recovery-diagnostic-staged-usb-v37.img'), ")
 if name.startswith('Collect-'):
  s=s.replace('def storage_window_complete(data):','def storage_window_complete(data):\n    data = data.replace(b\'\\r\\n\', b\'\\n\')')
  s=s.replace('return bool(hashes_ok and',"return bool(b'A6L_ANDROID_SERVICE_START' in data and b'A6L_ANDROID_PROPERTY ramdiag=v38' in data\n                and b'A6L_ANDROID_ADB_FUNCTION linked=1' in data and b'A6L_ANDROID_ADBD state=running' in data\n                and hashes_ok and")
 manifest['files'][version(name)]=write(version(name),s)
write('Test-RecoveryTransitionV38.py',version((T/'Test-RecoveryTransitionV37.py').read_text()))
write('Verify-AndroidRamReadbacks.py',version((T/'Verify-StorageReadReadbacks.py').read_text()).replace(old,candidate))
(T/'diagnostic-user-v38-tools.json').write_text(json.dumps(manifest,indent=2)+'\n')
print(json.dumps(manifest,indent=2))
