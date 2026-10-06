"""Offline round 11. Rerunning replaces the reviewed snapshot and evidence."""
from pathlib import Path
import hashlib,json,subprocess,tempfile,zipfile
HERE=Path(__file__).resolve().parent
ROOT=HERE.parents[1];DEV=ROOT/'device/hisense/a6l'
ANDROID=Path('/home/a6l/android/a6l-lineage24')
JAVA=ANDROID/'prebuilts/jdk/jdk21/linux-x86/bin'
PARSER=ANDROID/'frameworks/opt/telephony/src/java/com/android/internal/telephony/NitzData.java'
hashes={};results=[]
def run(name,cmd,timeout=120):
    r=subprocess.run([str(x) for x in cmd],capture_output=True,text=True,timeout=timeout)
    (HERE/(name+'.log')).write_text(r.stdout+r.stderr)
    results.append(dict(test=name,exit_code=r.returncode,stdout=r.stdout))
    (HERE/'results.json').write_text(json.dumps(results,indent=2)+'\n')
    print(name+': '+r.stdout,flush=True)
    if r.returncode:raise RuntimeError(r.stderr)
with tempfile.TemporaryDirectory(prefix='a6l-review11-') as tmp:
    w=Path(tmp)
    with zipfile.ZipFile(HERE/'reviewed-source.zip','w',zipfile.ZIP_DEFLATED) as z:
        for subtree in ('radio/qmi','radio/hal','radio/tests'):
            for p in (DEV/subtree).rglob('*'):
                if p.suffix not in ('.h','.cc','.cpp','.c') or any(x.startswith('build') for x in p.relative_to(DEV).parts):continue
                data=p.read_bytes();rel=p.relative_to(ROOT);hashes[str(rel)]=hashlib.sha256(data).hexdigest()
                dest=w/p.relative_to(DEV);dest.parent.mkdir(parents=True,exist_ok=True);dest.write_bytes(data)
                z.writestr(str(rel),data)
        data=PARSER.read_bytes();hashes[str(PARSER)]=hashlib.sha256(data).hexdigest()
        z.writestr('platform/NitzData.java',data);(w/'NitzData.java').write_bytes(data)
    (HERE/'source-hashes.json').write_text(json.dumps(hashes,indent=2)+'\n')
    radio=w/'radio'
    inc=['-I'+str(radio/'qmi/include'),'-I'+str(radio/'tests'),'-I'+str(radio/'tests/hoststub')]
    cmd=['g++','-std=c++17','-O1','-g','-pthread','-fsanitize=address,undefined','-fno-omit-frame-pointer','-no-pie',*inc]
    cmd += [*sorted((radio/'qmi/src').glob('*.cc')),radio/'hal/ModemCore.cpp',radio/'tests/fake_modem.cc',HERE/'radio.cc','-o',w/'radio-test']
    run('radio-build',cmd,180)
    run('radio',[w/'radio-test',w/'nitz.txt'],45)
    (HERE/'nitz.txt').write_bytes((w/'nitz.txt').read_bytes())
    stubs={
        'VisibleForTesting.java':'package com.android.internal.annotations; public @interface VisibleForTesting { enum Visibility { PACKAGE } Visibility visibility(); }',
        'Rlog.java':'package com.android.telephony; public class Rlog { public static void e(String t,String m){} }',
        'ServiceStateTracker.java':'package com.android.internal.telephony; public class ServiceStateTracker { public static final String LOG_TAG="NitzTest"; }'
    }
    for n,s in stubs.items():(w/n).write_text(s)
    run('java-build',[JAVA/'javac','-d',w,w/'NitzData.java',*[w/n for n in stubs],HERE/'CheckNitz.java'])
    run('java',[JAVA/'java','-cp',w,'CheckNitz',w/'nitz.txt'])
    changed=[]
    for key,h in hashes.items():
        p=Path(key) if key.startswith('/') else ROOT/key
        if hashlib.sha256(p.read_bytes()).hexdigest()!=h:changed.append(key)
    (HERE/'changed-during-run.json').write_text(json.dumps(changed,indent=2)+'\n')
    print('ROUND11_REPRODUCTIONS_PASS; source files changed:',len(changed),flush=True)
