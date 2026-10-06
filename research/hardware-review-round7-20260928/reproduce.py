"""Offline review assertions, not hardware acceptance. No phone or RF access.
Snapshot production source before compiling; never modify the production tree.
"""
from pathlib import Path
import hashlib,json,subprocess,tempfile,zipfile
HERE=Path(__file__).resolve().parent
ROOT=HERE.parents[1]; DEV=ROOT/'device/hisense/a6l'
PLATFORM=Path('/home/a6l/android/a6l-lineage24/hardware/interfaces')
def extract(source,signature):
    start=source.index(signature); p=source.index('{',start); end=p+1; depth=1
    while depth:
        depth+=(source[end]=='{')-(source[end]=='}'); end+=1
    return source[start:end]
hashes={}; results=[]
with tempfile.TemporaryDirectory(prefix='a6l-review7-') as tmp:
    w=Path(tmp)
    with zipfile.ZipFile(HERE/'reviewed-source.zip','w',zipfile.ZIP_DEFLATED) as z:
        for subtree in ('radio/qmi','radio/hal','radio/tests'):
            for p in (DEV/subtree).rglob('*'):
                if p.suffix not in ('.h','.cc','.cpp','.c') or any(x.startswith('build') for x in p.relative_to(DEV).parts): continue
                data=p.read_bytes(); rel=p.relative_to(ROOT); hashes[str(rel)]=hashlib.sha256(data).hexdigest()
                dest=w/p.relative_to(DEV); dest.parent.mkdir(parents=True,exist_ok=True); dest.write_bytes(data)
                z.writestr(str(rel),data)
        for rel in ('radio/aidl/android/hardware/radio/voice/IRadioVoice.aidl','radio/aidl/android/hardware/radio/voice/LastCallFailCause.aidl','radio/aidl/android/hardware/radio/voice/TtyMode.aidl'):
            p=PLATFORM/rel; data=p.read_bytes(); hashes[str(p)]=hashlib.sha256(data).hexdigest(); z.writestr('platform/'+rel,data)
    (HERE/'source-hashes.json').write_text(json.dumps(hashes,indent=2)+'\n')
    radio=w/'radio'; source=(radio/'hal/RadioMessagingVoice.cpp').read_text()
    methods=[extract(source,'std::optional<uint8_t> A6lRadioVoice::findCall(')]
    methods += [extract(source,'ScopedAStatus A6lRadioVoice::'+n+'(') for n in ('setTtyMode','getTtyMode','sendDtmf','stopDtmf','rejectCall')]
    (w/'voice_methods.inc').write_text('\n\n'.join(methods))
    # Complete member-use inventory supports the constant-cause finding.
    uses=[]
    for p in (radio/'hal').rglob('*'):
        if p.suffix in ('.h','.cpp'):
            for ln,s in enumerate(p.read_text().splitlines(),1):
                if 'mLastCallFailCause' in s: uses.append(f'{p.relative_to(w)}:{ln}: {s.strip()}')
    (HERE/'failcause-member-uses.txt').write_text('\n'.join(uses)+'\n')
    qmi=[radio/'qmi/src'/n for n in ('message.cc','client.cc','services.cc','datacall.cc','multisim.cc','log.cc','sms.cc','ims.cc','rmnet.cc','qrtr_transport.cc')]
    inc=['-I'+str(w),'-I'+str(radio/'qmi/include'),'-I'+str(radio/'tests'),'-I'+str(radio/'tests/hoststub')]
    for name in ('voice','failcause'):
        cmd=['g++','-std=c++17','-O1','-g','-pthread','-fsanitize=address,undefined','-fno-omit-frame-pointer','-no-pie',*inc,HERE/(name+'.cc'),*qmi,radio/'tests/fake_modem.cc']
        if name=='failcause':cmd.append(radio/'hal/ModemCore.cpp')
        exe=w/('review-'+name)
        print('Building '+name,flush=True)
        b=subprocess.run([str(x) for x in [*cmd,'-o',exe]],capture_output=True,text=True,timeout=180)
        (HERE/(name+'-build.log')).write_text(b.stdout+b.stderr)
        if b.returncode:raise RuntimeError(b.stderr)
        r=subprocess.run([str(exe)],capture_output=True,text=True,timeout=30)
        (HERE/(name+'.log')).write_text(r.stdout+r.stderr)
        results.append(dict(test=name,exit_code=r.returncode,stdout=r.stdout))
        (HERE/'results.json').write_text(json.dumps(results,indent=2)+'\n')
        print(r.stdout,flush=True)
        if r.returncode:raise RuntimeError(r.stderr)
    changed=[]
    for p,h in hashes.items():
        f=Path(p) if p.startswith('/') else ROOT/p
        if hashlib.sha256(f.read_bytes()).hexdigest()!=h:changed.append(p)
    (HERE/'changed-during-run.json').write_text(json.dumps(changed,indent=2)+'\n')
    print('ROUND7_REPRODUCTIONS_PASS; source files changed:',len(changed),flush=True)
