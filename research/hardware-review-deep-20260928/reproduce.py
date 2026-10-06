"""Offline review only: temporary source snapshot, fake-device executables, no ROM build.

Assertions reproduce defects. They are not desired feature acceptance results.
"""
from pathlib import Path
import hashlib, json, os, subprocess, tempfile, sys
HERE=Path(__file__).resolve().parent
ROOT=HERE.parents[1]
DEVICE=ROOT/'device/hisense/a6l'
hashes={}
selected=sys.argv[1:]
OUT=HERE/('run-'+'-'.join(selected)) if selected else HERE
OUT.mkdir(exist_ok=True)
def run(cmd,**kwargs):
    r=subprocess.run([str(x) for x in cmd],capture_output=True,text=True,timeout=240,**kwargs)
    return r
with tempfile.TemporaryDirectory(prefix='a6l-deep-review-') as tmp:
    work=Path(tmp)
    for subtree in ('radio/qmi','radio/tests','radio/hal','gnss/lib','hals/sensors/stk3338'):
        for p in (DEVICE/subtree).rglob('*'):
            if p.suffix not in ('.h','.cc','.cpp','.c'):continue
            if any(x.startswith('build') for x in p.relative_to(DEVICE).parts):continue
            data=p.read_bytes();hashes[str(p.relative_to(ROOT))]=hashlib.sha256(data).hexdigest()
            dest=work/p.relative_to(DEVICE);dest.parent.mkdir(parents=True,exist_ok=True);dest.write_bytes(data)
    (OUT/'source-hashes.json').write_text(json.dumps(hashes,indent=2)+'\n')
    radio=work/'radio';gnss=work/'gnss/lib';sensor=work/'hals/sensors/stk3338'
    common=['-O1','-g','-pthread','-fsanitize=address,undefined','-fno-omit-frame-pointer']
    inc=['-I'+str(radio/'qmi/include'),'-I'+str(radio/'tests')]
    qmi=[radio/'qmi/src'/x for x in ('message.cc','client.cc','services.cc','datacall.cc','multisim.cc','log.cc','sms.cc','ims.cc','rmnet.cc','qrtr_transport.cc')]
    voice_source=(radio/'hal/RadioMessagingVoice.cpp').read_text()
    methods=[]
    for name in ('dial','emergencyDial'):
        start=voice_source.index('ScopedAStatus A6lRadioVoice::'+name+'(')
        pos=voice_source.index('{',start);depth=1;end=pos+1
        while depth:
            depth+=(voice_source[end]=='{')-(voice_source[end]=='}');end+=1
        methods.append(voice_source[start:end])
    (work/'voice_methods.inc').write_text('\n'.join(methods))
    jobs=[
      ('sensors',['gcc','-std=gnu11',*common,'-DA6L_HOST_TEST','-I'+str(sensor),'-I'+str(sensor/'tests/include'),HERE/'sensors_review.c',sensor/'a6l_magcal.c','-lm']),
      ('sms',['g++','-std=c++17',*common,*inc,'-I'+str(radio/'tests/hoststub'),HERE/'sms_review.cc',*qmi,radio/'tests/fake_modem.cc',radio/'hal/ModemCore.cpp']),
      ('data',['g++','-std=c++17',*common,*inc,HERE/'data_generation_review.cc',*qmi,radio/'tests/fake_modem.cc']),
      ('gnss',['g++','-std=c++17',*common,'-I'+str(gnss),HERE/'gnss_review.cc',*(gnss/x for x in ('loc_client.cpp','loc_v02.cpp','qmi.cpp','nmea.cpp','android_map.cpp','qrtr_transport.cpp'))]),
      ('cache',['g++','-std=c++17',*common,*inc,'-I'+str(radio/'tests/hoststub'),HERE/'cache_review.cc',*qmi,radio/'tests/fake_modem.cc',radio/'hal/ModemCore.cpp']),
      ('voice',['g++','-std=c++17',*common,'-I'+str(work),HERE/'voice_contract_review.cc']),
    ]
    if selected:jobs=[j for j in jobs if j[0] in selected]
    assert jobs
    logs=[]
    env=os.environ.copy();env.update(A6L_SYSROOT=str(work/'fake-sys'),A6L_DEVROOT=str(work/'fake-dev'),A6L_DATADIR=str(work/'fake-data'))
    for name,cmd in jobs:
        print('Building '+name+' review...',flush=True);binary=work/('review-'+name)
        build=run([*cmd,'-o',binary]);(OUT/(name+'-build.txt')).write_text(build.stdout+build.stderr)
        if build.returncode: print(build.stderr,flush=True);raise RuntimeError(name+' build failed')
        r=run([binary],env=env);(OUT/(name+'-stderr.txt')).write_text(r.stderr)
        logs.append(r.stdout);(OUT/'results.txt').write_text('\n'.join(logs))
        print(r.stdout,flush=True)
        if r.returncode:print(r.stderr,flush=True);raise RuntimeError(name+' reproduction failed')
    changed=[p for p,h in hashes.items() if hashlib.sha256((ROOT/p).read_bytes()).hexdigest()!=h]
    (OUT/'changed-during-run.json').write_text(json.dumps(changed,indent=2)+'\n')
    print('Completed '+str(len(jobs))+' harnesses. Changed source inputs: '+str(len(changed)),flush=True)
