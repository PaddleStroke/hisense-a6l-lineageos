"""Verify staged files only. Never open USB or launch a phone operation."""
import hashlib,json
from pathlib import Path
root=Path(__file__).resolve().parent
def check(path,digest):
    assert hashlib.sha256(path.read_bytes()).hexdigest()==digest,str(path)
pins=json.loads((root/'controls-v45-pins.json').read_text())
for name,digest in pins.items():
    assert not Path(name).is_absolute() and '..' not in Path(name).parts
    check(root/name,digest)
counts={}
for version in ['v38','v45']:
    manifest=json.loads((root/('diagnostic-user-'+version+'-tools.json')).read_text())
    for name,digest in manifest['files'].items():check(root/name,digest)
    counts[version+'_tools']=len(manifest['files'])
candidate=root/'ram-staging/recovery-diagnostic-staged-usb-v45.img'
check(candidate,'aae86f0da6f15a0c1f6e425e88806edb165289c459462b232f0b8773f5e4ac14')
check(root/'ram-staging/recovery-diagnostic-staged-usb-v38.img','54b0d7b2502b71d8493e4669f2081e46d9ec4ba7a23c275373d735134977cbbb')
check(root/'stock-recovery/restore-stock-recovery.img','9688e3dfb349186fb60efc057a618aeaf696b030ad52974932ea83a077b31621')
assert json.loads((root/'controls-v45/captured-abl-validation.json').read_text())['passed']
for directory,report in [('controls-v45','qemu-module-report.json'),('android-surface-v44','qemu-report.json')]:
    package=root/directory
    assert json.loads((package/report).read_text())['passed']
    files=json.loads((package/'manifest.json').read_text())['files']
    for name,info in files.items():
        assert not Path(name).is_absolute() and '..' not in Path(name).parts
        check(package/'payload'/name,info['sha256'])
    counts[directory+'_payload']=len(files)
print(json.dumps({'passed':True,'pin_count':len(pins),'verified':counts,'scope':'Offline staged-file verification; no USB access or phone actions'},indent=2))
