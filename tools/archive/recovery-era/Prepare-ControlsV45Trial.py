"""Generate offline V45 install/restore/capture tools from pinned known tools."""
import hashlib,json,py_compile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1];T=ROOT/'tools';OUT=ROOT/'firmware/extracted/recovery-controls-v45-20260917'
old='54b0d7b2502b71d8493e4669f2081e46d9ec4ba7a23c275373d735134977cbbb'
prior='7de9a96229140b066aed2724ccf2cb14b1a09cc9e44a12bfb25ee4543761b8e9'
report=json.loads((OUT/'report.json').read_text());new=report['candidate_sha256']
assert hashlib.sha256((OUT/'recovery-diagnostic-unsigned.img').read_bytes()).hexdigest()==new
assert json.loads((OUT/'captured-abl-validation.json').read_text())['passed']
previous=json.loads((T/'diagnostic-user-v38-tools.json').read_text())['files']
manifest={'candidate_sha256':new,'previous_sha256':old,'files':{},'sources':{}}
def put(name,text):
    p=T/name;assert not p.exists(),p;p.write_text(text);py_compile.compile(str(p),doraise=True)
    manifest['files'][name]=hashlib.sha256(p.read_bytes()).hexdigest()
for name in ['DiagnosticRecoveryProtocolV38.py','RecoveryTransitionV38.py','Write-LaptopDiagnosticRecovery-user-v38.py','Inspect-DiagnosticRecovery-user-v38.py','Run-LaptopDiagnosticInstall-user-v38.py','Run-LaptopDiagnosticRestore-user-v38.py']:
    raw=(T/name).read_bytes();assert hashlib.sha256(raw).hexdigest()==previous[name],name
    manifest['sources'][name]=previous[name]
    s=raw.decode().replace('V38','V45').replace('v38','v45')
    if name=='RecoveryTransitionV38.py':s=s.replace(prior,old).replace('V37','V38').replace('v37','v38')
    else:s=s.replace(old,new)
    if name.startswith('Inspect-'):
        anchor='for mode, filename in [';assert s.count(anchor)==1
        s=s.replace(anchor,anchor+"('install-diagnostic', 'ram-staging/recovery-diagnostic-staged-usb-v38.img'), ")
    put(name.replace('V38','V45').replace('v38','v45'),s)
for oldname,newname in [('Test-RecoveryTransitionV38.py','Test-RecoveryTransitionV45.py'),('Test-DiagnosticRecoveryProtocolV38.py','Test-DiagnosticRecoveryProtocolV45.py'),('Verify-AndroidRamReadbacks.py','Verify-ControlsV45Readbacks.py')]:
    s=(T/oldname).read_text().replace('V38','V45').replace('v38','v45').replace(old,new).replace('recovery-probe-android-ram-v45-20260917','recovery-controls-v45-20260917');put(newname,s)
# Reuse short authenticated readiness. Preserve V38 userspace identifiers.
s=(T/'Run-LaptopSurfaceCapture-v44.py').read_text().replace('V44','V45').replace('v44','v45').replace('surface','controls').replace('Surface','Controls')
s=s.replace('capture-diagnostic-install-user-v38','capture-diagnostic-install-user-v45').replace(old,new)
s=s.replace('Reuse V38 recovery for V45 RAM graphics services; one bootloader restart, no image writes','Test installed V45 combined controls with unchanged V38 RAM userspace; one bootloader restart, no image writes')
put('Run-LaptopControlsCapture-v45.py',s)
put('Launch-ControlsV45.py',(T/'Launch-SurfaceV44.py').read_text().replace('V44','V45').replace('v44','v45').replace('Surface','Controls').replace('surface','controls').replace('A6L RAM Android graphics test','A6L combined controls test'))
put('Launch-ControlsV45Install.py','''"""Launch the checked V45 install only when explicitly invoked while attended."""
import json,os,subprocess
from pathlib import Path
root=Path(__file__).resolve().parent
assert not (root/'capture-diagnostic-install-user-v45').exists()
subprocess.run(['/usr/bin/python3',str(root/'Verify-ControlsV45Stage.py')],check=True)
env=dict(os.environ,DISPLAY=':0',XDG_RUNTIME_DIR='/run/user/1000',DBUS_SESSION_BUS_ADDRESS='unix:path=/run/user/1000/bus')
with (root/'controls-v45-install-launch.log').open('x') as log:
    p=subprocess.Popen(['/usr/bin/gnome-session-inhibit','--app-id','A6L-v45-install','--reason','A6L diagnostic installation','--inhibit','suspend:idle','/usr/bin/python3',str(root/'Run-LaptopDiagnosticInstall-user-v45.py')],cwd=root,env=env,stdin=subprocess.DEVNULL,stdout=log,stderr=subprocess.STDOUT,start_new_session=True)
print(json.dumps({'launched_pid':p.pid}))
''')
(T/'diagnostic-user-v45-tools.json').write_text(json.dumps(manifest,indent=2)+'\n')
print('V45_TOOLS_PREPARED',len(manifest['files']),'not run')
