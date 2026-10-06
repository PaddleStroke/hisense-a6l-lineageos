"""Offline GNSS review; current-source snapshot. Rerunning replaces evidence."""
from pathlib import Path
import hashlib,json,subprocess,tempfile,zipfile
HERE=Path(__file__).resolve().parent;ROOT=HERE.parents[1];GNSS=ROOT/'device/hisense/a6l/gnss'
hashes={}
with tempfile.TemporaryDirectory(prefix='a6l-review10-') as tmp:
    w=Path(tmp)
    paths=list((GNSS/'lib').glob('*.h'))+list((GNSS/'lib').glob('*.cpp'))+[GNSS/'hal/Gnss.cpp']
    with zipfile.ZipFile(HERE/'reviewed-source.zip','w',zipfile.ZIP_DEFLATED) as z:
        for p in paths:
            data=p.read_bytes();key=str(p.relative_to(ROOT));hashes[key]=hashlib.sha256(data).hexdigest()
            z.writestr(key,data);(w/p.name).write_bytes(data)
    (HERE/'source-hashes.json').write_text(json.dumps(hashes,indent=2)+'\n')
    exe=w/'gnss'
    cmd=['g++','-std=c++17','-O1','-g','-pthread','-fsanitize=address,undefined','-fno-omit-frame-pointer','-no-pie','-I'+str(w),str(HERE/'gnss.cc')]
    cmd += [str(w/n) for n in ('qmi.cpp','loc_v02.cpp','loc_client.cpp','nmea.cpp','android_map.cpp','qrtr_transport.cpp')]+['-o',str(exe)]
    b=subprocess.run(cmd,capture_output=True,text=True,timeout=120)
    (HERE/'build.log').write_text(b.stdout+b.stderr)
    if b.returncode:raise RuntimeError(b.stderr)
    r=subprocess.run([str(exe)],capture_output=True,text=True,timeout=20)
    (HERE/'run.log').write_text(r.stdout+r.stderr)
    (HERE/'results.json').write_text(json.dumps(dict(exit_code=r.returncode,stdout=r.stdout),indent=2)+'\n')
    print(r.stdout,flush=True)
    if r.returncode:raise RuntimeError(r.stderr)
    changed=[key for key,h in hashes.items() if hashlib.sha256((ROOT/key).read_bytes()).hexdigest()!=h]
    (HERE/'changed-during-run.json').write_text(json.dumps(changed,indent=2)+'\n')
    print('ROUND10_REPRODUCTIONS_PASS; source files changed:',len(changed),flush=True)
