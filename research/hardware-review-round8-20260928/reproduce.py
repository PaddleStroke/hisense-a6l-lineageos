"""Offline review, snapshots current source; rerunning replaces evidence. No phone access."""
from pathlib import Path
import hashlib,json,subprocess,tempfile,zipfile
HERE=Path(__file__).resolve().parent
ROOT=HERE.parents[1]; DEV=ROOT/'device/hisense/a6l'
hashes={};results=[]
with tempfile.TemporaryDirectory(prefix='a6l-review8-') as tmp:
    w=Path(tmp)
    with zipfile.ZipFile(HERE/'reviewed-source.zip','w',zipfile.ZIP_DEFLATED) as z:
        for subtree in ('radio/qmi','radio/hal','radio/tests'):
            for p in (DEV/subtree).rglob('*'):
                if p.suffix not in ('.h','.cc','.cpp','.c') or any(x.startswith('build') for x in p.relative_to(DEV).parts):continue
                data=p.read_bytes();rel=p.relative_to(ROOT);hashes[str(rel)]=hashlib.sha256(data).hexdigest()
                dest=w/p.relative_to(DEV);dest.parent.mkdir(parents=True,exist_ok=True);dest.write_bytes(data)
                z.writestr(str(rel),data)
        p=Path('/home/a6l/android/a6l-lineage24/hardware/interfaces/radio/aidl/android/hardware/radio/network/IRadioNetworkIndication.aidl')
        data=p.read_bytes();hashes[str(p)]=hashlib.sha256(data).hexdigest();z.writestr('platform/IRadioNetworkIndication.aidl',data)
    (HERE/'source-hashes.json').write_text(json.dumps(hashes,indent=2)+'\n')
    radio=w/'radio'
    source=(radio/'hal/RadioNetworkData.cpp').read_text()
    start=source.index('void A6lRadioNetwork::onNitz(');end=source.index('\n}',start)+2
    (w/'nitz_method.inc').write_text(source[start:end])
    qmi=[radio/'qmi/src'/n for n in ('message.cc','client.cc','services.cc','datacall.cc','multisim.cc','log.cc','sms.cc','ims.cc','rmnet.cc','qrtr_transport.cc')]
    inc=['-I'+str(w),'-I'+str(radio/'qmi/include'),'-I'+str(radio/'tests')]
    for name in ('data','nitz'):
        sources=[HERE/(name+'.cc')]
        if name=='data': sources += [*qmi,radio/'tests/fake_modem.cc']
        exe=w/name
        cmd=['g++','-std=c++17','-O1','-g','-pthread','-fsanitize=address,undefined','-fno-omit-frame-pointer','-no-pie',*inc,*sources,'-o',exe]
        print('Building '+name,flush=True)
        b=subprocess.run([str(x) for x in cmd],capture_output=True,text=True,timeout=180)
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
    print('ROUND8_REPRODUCTIONS_PASS; source files changed:',len(changed),flush=True)
