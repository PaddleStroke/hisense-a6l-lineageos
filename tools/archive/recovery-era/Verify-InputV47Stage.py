"""Verify staged V47 RAM input experiment; no USB or phone operation."""
import hashlib,json
from pathlib import Path
root=Path(__file__).resolve().parent
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
pins=json.loads((root/'input-v47-pins.json').read_text())
for name,digest in pins.items():
    assert not Path(name).is_absolute() and '..' not in Path(name).parts
    assert sha(root/name)==digest,name
package=root/'android-input-v47'
assert json.loads((package/'qemu-report.json').read_text())['passed']
files=json.loads((package/'manifest.json').read_text())['files']
for name,info in files.items():
    assert not Path(name).is_absolute() and '..' not in Path(name).parts
    assert sha(package/'payload'/name)==info['sha256'],name
assert sha(root/'ram-staging/recovery-diagnostic-staged-usb-v46.img')=='ce3727dda592065becb883ef3fe663cb3579290edb841854e01f4fc49d02a3c6'
old=json.loads((root/'diagnostic-user-v38-tools.json').read_text())['files']
for name,digest in old.items():assert sha(root/name)==digest,name
print(json.dumps({'passed':True,'pin_count':len(pins),'payload_files':len(files),'shared_tools':len(old),'scope':'File-only validation; no phone actions'},indent=2))
