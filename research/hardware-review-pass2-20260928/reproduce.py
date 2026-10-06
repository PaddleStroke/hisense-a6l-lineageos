"""Second-pass offline review. Snapshot current source; build only review binaries.

No Android build, phone access, real network configuration, or production edits.
Assertions describe defects, not desired behavior. WSL python3 + gcc/g++ required.
"""
from pathlib import Path
import hashlib
import json
import os
import shutil
import subprocess
import tempfile

HERE=Path(__file__).resolve().parent
ROOT=HERE.parents[1]
DEVICE=ROOT/'device/hisense/a6l'
hashes={}
output=[]

def run(cmd, **kwargs):
    result=subprocess.run(list(map(str,cmd)),capture_output=True,text=True,timeout=240,**kwargs)
    if result.returncode:
        print(result.stdout,flush=True);print(result.stderr,flush=True)
        raise RuntimeError(f'command failed ({result.returncode}): {cmd[0]}')
    return result

with tempfile.TemporaryDirectory(prefix='a6l-review-pass2-') as tmp:
    work=Path(tmp)
    for subtree in ('radio/qmi','radio/tests','radio/hal','gnss/lib','kvoice/q6voiced','eink/switcher/native'):
        for source in (DEVICE/subtree).rglob('*'):
            if source.suffix not in ('.cc','.cpp','.c','.h'): continue
            if any(part.startswith('build') for part in source.relative_to(DEVICE).parts): continue
            data=source.read_bytes()
            hashes[str(source.relative_to(ROOT))]=hashlib.sha256(data).hexdigest()
            dest=work/source.relative_to(DEVICE)
            dest.parent.mkdir(parents=True,exist_ok=True)
            dest.write_bytes(data)
    radio=work/'radio'; gnss=work/'gnss/lib'; voice=work/'kvoice/q6voiced'; dx=work/'eink/switcher/native'
    qmi=[radio/'qmi/src'/name for name in ('message.cc','client.cc','services.cc','datacall.cc','multisim.cc','log.cc','sms.cc','ims.cc')]
    common=['-O1','-g','-pthread','-fsanitize=address,undefined','-fno-omit-frame-pointer']
    inc=['-I'+str(radio/'qmi/include'),'-I'+str(radio/'tests')]
    jobs=[
        ('radio',['g++','-std=c++17',*common,*inc,HERE/'radio_review.cc',*qmi,radio/'tests/fake_modem.cc']),
        ('core',['g++','-std=c++17',*common,*inc,'-I'+str(radio/'tests/hoststub'),HERE/'core_review.cc',
                 *qmi,radio/'qmi/src/rmnet.cc',radio/'qmi/src/qrtr_transport.cc',radio/'hal/ModemCore.cpp',radio/'tests/fake_modem.cc']),
        ('gnss',['g++','-std=c++17',*common,'-I'+str(gnss),HERE/'gnss_review.cc',
                 *(gnss/x for x in ('loc_client.cpp','loc_v02.cpp','qmi.cpp','nmea.cpp','android_map.cpp','qrtr_transport.cpp'))]),
        ('voice',['gcc','-std=gnu11',*common,'-I'+str(voice),HERE/'voice_review.c']),
        ('dualux',['gcc','-std=gnu11',*common,'-I'+str(dx),HERE/'dualux_review.c',dx/'dualux_logic.c','-lm']),
    ]
    active=work/'active';active.write_text('1\n')
    input_file=work/'evdev';input_file.write_bytes(b'')
    for name,cmd in jobs:
        print(f'Building {name} review harness...',flush=True)
        binary=work/('review-'+name)
        built=run([*cmd,'-o',binary])
        (HERE/(name+'-build.txt')).write_text(built.stderr)
        args=[active] if name=='voice' else [input_file] if name=='dualux' else []
        result=run([binary,*args])
        output.append(result.stdout.strip())
        (HERE/(name+'-stderr.txt')).write_text(result.stderr)
        print(result.stdout,flush=True)
        (HERE/'results.txt').write_text('\n'.join(output)+'\n')
    changed=[p for p,h in hashes.items() if hashlib.sha256((ROOT/p).read_bytes()).hexdigest()!=h]
    (HERE/'source-hashes.json').write_text(json.dumps(hashes,indent=2)+'\n')
    (HERE/'changed-during-run.json').write_text(json.dumps(changed,indent=2)+'\n')
    print(f'Completed {len(jobs)} isolated ASan/UBSan harnesses. Source files changed during run: {len(changed)}',flush=True)
