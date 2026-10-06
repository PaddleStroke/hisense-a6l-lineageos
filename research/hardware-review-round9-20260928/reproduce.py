"""Offline exact-method review. Rerunning replaces the evidence with current source."""
from pathlib import Path
import hashlib,json,subprocess,tempfile,zipfile
HERE=Path(__file__).resolve().parent;ROOT=HERE.parents[1];DEV=ROOT/'device/hisense/a6l'
def extract(s,sig):
    a=s.index(sig);b=s.index('{',a);i=b+1;depth=1
    while depth:depth+=(s[i]=='{')-(s[i]=='}');i+=1
    return s[a:i]
rels=['audio/patches/0001-a6l-primary-capture-pcm.patch','audio/patches/0002-a6l-call-route-mute.patch',
      'audio/route/a6l_audio_route.c','audio/audio_policy_configuration.xml',
      'eink/switcher/native/a6l_dualux.c','eink/switcher/native/dualux_logic.c','eink/switcher/native/dualux_logic.h']
paths=[DEV/r for r in rels]+[ROOT/'tools/rom-v2-pipeline.sh',
 Path('/home/a6l/android/a6l-lineage24/hardware/interfaces/audio/aidl/default/primary/StreamPrimary.cpp')]
hashes={};results=[]
with tempfile.TemporaryDirectory(prefix='a6l-review9-') as tmp:
    w=Path(tmp)
    with zipfile.ZipFile(HERE/'reviewed-source.zip','w',zipfile.ZIP_DEFLATED) as z:
        for p in paths:
            data=p.read_bytes();key=str(p.relative_to(ROOT)) if p.is_relative_to(ROOT) else str(p)
            hashes[key]=hashlib.sha256(data).hexdigest();z.writestr(key.lstrip('/'),data)
            (w/p.name).write_bytes(data)
    (HERE/'source-hashes.json').write_text(json.dumps(hashes,indent=2)+'\n')
    patch=(w/'0002-a6l-call-route-mute.patch').read_text()
    added='\n'.join(line[1:] for line in patch.splitlines() if line.startswith('+') and not line.startswith('+++'))
    route=(w/'a6l_audio_route.c').read_text()
    methods=[extract(added,s) for s in ('std::string a6lOutName(','std::string a6lInName(','void ModulePrimary::a6lPublishCallRoute(')]
    methods += [extract(route,'struct route_state')+';',extract(route,'struct route_choice')+';',extract(route,'static void decide(')]
    (w/'audio_methods.inc').write_text('\n\n'.join(methods))
    (w/'frontlight_method.inc').write_text(extract((w/'a6l_dualux.c').read_text(),'static void enforce_frontlight('))
    for name in ('audio','frontlight'):
        exe=w/name
        cmd=['g++' if name=='audio' else 'gcc','-std=c++17' if name=='audio' else '-std=gnu11','-O1','-g',
             '-fsanitize=address,undefined','-fno-omit-frame-pointer','-no-pie','-I'+str(w),str(HERE/(name+('.cc' if name=='audio' else '.c')))]
        if name=='frontlight':cmd += [str(w/'dualux_logic.c'),'-lm']
        cmd += ['-o',str(exe)]
        b=subprocess.run(cmd,capture_output=True,text=True,timeout=60)
        (HERE/(name+'-build.log')).write_text(b.stdout+b.stderr)
        if b.returncode:raise RuntimeError(b.stderr)
        r=subprocess.run([str(exe)],capture_output=True,text=True,timeout=15)
        (HERE/(name+'.log')).write_text(r.stdout+r.stderr)
        results.append(dict(test=name,exit_code=r.returncode,stdout=r.stdout))
        (HERE/'results.json').write_text(json.dumps(results,indent=2)+'\n');print(r.stdout,flush=True)
        if r.returncode:raise RuntimeError(r.stderr)
    changed=[key for p,key in zip(paths,hashes) if hashlib.sha256(p.read_bytes()).hexdigest()!=hashes[key]]
    (HERE/'changed-during-run.json').write_text(json.dumps(changed,indent=2)+'\n')
    print('ROUND9_REPRODUCTIONS_PASS; source files changed:',len(changed),flush=True)
