"""Offline current-source snapshot. Rerunning replaces this round's evidence."""
from pathlib import Path
import hashlib,json,subprocess,tempfile,zipfile
HERE=Path(__file__).resolve().parent;ROOT=HERE.parents[1]
GNSS=ROOT/'device/hisense/a6l/gnss'
AIDL=Path('/home/a6l/android/a6l-lineage24/hardware/interfaces/gnss/aidl/android/hardware/gnss/GnssLocation.aidl')
hashes={};results=[]
with tempfile.TemporaryDirectory(prefix='a6l-review12-') as tmp:
    w=Path(tmp)
    paths=[GNSS/'lib'/n for n in ('qmi.cpp','qmi.h','loc_v02.cpp','loc_v02.h','android_map.cpp','android_map.h')]+[GNSS/'hal/Gnss.cpp']
    with zipfile.ZipFile(HERE/'reviewed-source.zip','w',zipfile.ZIP_DEFLATED) as z:
        for p in paths:
            data=p.read_bytes();key=str(p.relative_to(ROOT));hashes[key]=hashlib.sha256(data).hexdigest()
            z.writestr(key,data);(w/p.name).write_bytes(data)
        data=AIDL.read_bytes();hashes[str(AIDL)]=hashlib.sha256(data).hexdigest();z.writestr('platform/GnssLocation.aidl',data)
    (HERE/'source-hashes.json').write_text(json.dumps(hashes,indent=2)+'\n')
    cmd=['g++','-std=c++17','-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer','-no-pie','-I'+str(w),str(HERE/'accuracy.cc')]
    cmd += [str(w/n) for n in ('qmi.cpp','loc_v02.cpp','android_map.cpp')]+['-o',str(w/'accuracy')]
    for name,command in [('build',cmd),('run',[str(w/'accuracy')])]:
        r=subprocess.run(command,capture_output=True,text=True,timeout=120)
        (HERE/(name+'.log')).write_text(r.stdout+r.stderr)
        results.append(dict(test=name,exit_code=r.returncode,stdout=r.stdout))
        (HERE/'results.json').write_text(json.dumps(results,indent=2)+'\n')
        print(r.stdout,flush=True)
        if r.returncode:raise RuntimeError(r.stderr)
    changed=[]
    for key,h in hashes.items():
        p=Path(key) if key.startswith('/') else ROOT/key
        if hashlib.sha256(p.read_bytes()).hexdigest()!=h:changed.append(key)
    (HERE/'changed-during-run.json').write_text(json.dumps(changed,indent=2)+'\n')
    print('ROUND12_REPRODUCTIONS_PASS; source files changed:',len(changed),flush=True)
