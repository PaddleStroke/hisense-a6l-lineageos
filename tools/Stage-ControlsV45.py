"""Copy verified V45 preparation to laptop; never launch installation or capture."""
import hashlib,json,subprocess
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1];T=ROOT/'tools'
OUT=ROOT/'research/combined-controls-v45-20260917';OUT.mkdir(exist_ok=True)
REMOTE='/home/pierrelouis/A6L-usb-20260915'
CONFIG=str(T/'a6l-laptop-ssh.conf')
SSH='C:/Windows/System32/OpenSSH/ssh.exe';SCP='C:/Windows/System32/OpenSSH/scp.exe'
def run(argv):
    p=subprocess.run(argv,capture_output=True,text=True,timeout=180)
    assert p.returncode==0,(argv,p.stdout,p.stderr)
    return p.stdout
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
manifest=json.loads((T/'diagnostic-user-v45-tools.json').read_text())
fixed='Test-DiagnosticRecoveryProtocolV45.py'
manifest['files'][fixed]=sha(T/fixed)
(T/'diagnostic-user-v45-tools.json').write_text(json.dumps(manifest,indent=2)+'\n')
files={name:T/name for name in manifest['files']}
for name in ['diagnostic-user-v45-tools.json','Run-ControlsV45.py','Run-AndroidSurfaceOnV45.py','Capture-TouchEvents.py','Verify-ControlsV45Stage.py']:
    files[name]=T/name
package=ROOT/'firmware/extracted/controls-v45-prep-20260917'
for name in ['manifest.json','qemu-module-report.json']:
    files['controls-v45/'+name]=package/name
for p in (package/'payload').iterdir():files['controls-v45/payload/'+p.name]=p
image=ROOT/'firmware/extracted/recovery-controls-v45-20260917'
files['controls-v45/captured-abl-validation.json']=image/'captured-abl-validation.json'
files['ram-staging/recovery-diagnostic-staged-usb-v45.img']=image/'recovery-diagnostic-unsigned.img'
pins={n:sha(p) for n,p in files.items()}
old=json.loads((T/'surface-v44-pins.json').read_text())
for name in ['Wait-AndroidRamReady.py','diagnostic-user-v38-tools.json','android-surface-v44/manifest.json','android-surface-v44/qemu-report.json']:
    pins[name]=old[name]
(T/'controls-v45-pins.json').write_text(json.dumps(pins,indent=2)+'\n')
files['controls-v45-pins.json']=T/'controls-v45-pins.json'
# Abort if this version has ever been launched; existing staging is reusable only
# when each existing file already equals the requested hash.
preflight="import pathlib; r=pathlib.Path('"+REMOTE+"'); assert not (r/'capture-diagnostic-install-user-v45').exists(); assert not (r/'capture-controls-user-v45').exists(); print('STAGING_ONLY_READY')"
run([SSH,'-F',CONFIG,'a6l-laptop',"python3 -c \""+preflight+"\""])
for name,p in files.items():
    destination=REMOTE+'/'+name
    check="import pathlib,hashlib; p=pathlib.Path('"+destination+"'); assert not p.exists() or hashlib.sha256(p.read_bytes()).hexdigest()=='"+sha(p)+"'; p.parent.mkdir(parents=True,exist_ok=True)"
    run([SSH,'-F',CONFIG,'a6l-laptop','python3 -c "'+check+'"'])
    run([SCP,'-F',CONFIG,str(p),'a6l-laptop:'+destination])
    print('STAGED',name,flush=True)
result=run([SSH,'-F',CONFIG,'a6l-laptop','python3 '+REMOTE+'/Verify-ControlsV45Stage.py'])
(OUT/'laptop-stage-verification.json').write_text(result)
result2=run([SSH,'-F',CONFIG,'a6l-laptop','python3 '+REMOTE+'/Inspect-DiagnosticRecovery-user-v45.py'])
(OUT/'laptop-offline-inspection.txt').write_text(result2)
print(result,result2)
