"""Additional review requested during round 5. Fake evdev/sysfs only."""
from pathlib import Path
import hashlib,json,os,re,subprocess,tempfile,zipfile
HERE=Path(__file__).resolve().parent
ROOT=HERE.parents[1]; DEV=ROOT/'device/hisense/a6l'
PLATFORM=Path('/home/a6l/android/a6l-lineage24')
KERNEL=Path('/home/a6l/kernel/a6l-baseline-7.2')
def extract(src,sig):
    a=src.index(sig);b=src.index('{',a)+1;depth=1
    while depth:
        depth+=(src[b]=='{')-(src[b]=='}');b+=1
    return src[a:b]
def run(cmd):
    p=subprocess.run([str(x) for x in cmd],text=True,capture_output=True,timeout=90,
      env=dict(os.environ,ASAN_OPTIONS='detect_leaks=0'))
    assert p.returncode==0,(p.returncode,p.stdout,p.stderr)
    return p.stdout+p.stderr
paths=[DEV/'kernel/a6l-gpio-vibrator/a6l_gpio_vib.c',
 DEV/'eink/switcher/native/a6l_dualux.c',DEV/'eink/switcher/native/dualux_logic.c',DEV/'eink/switcher/native/dualux_logic.h',
 DEV/'rom/rom.mk',DEV/'rom/init/init.qcom.rc',DEV/'rom/bin/a6l-modules.sh',
 PLATFORM/'vendor/qcom/opensource/vibrator/aidl/VibratorOL/Vibrator.cpp',
 PLATFORM/'vendor/qcom/opensource/vibrator/aidl/Vibrator.cpp',
 PLATFORM/'vendor/qcom/opensource/vibrator/vibrator-vendor-product.mk',
 PLATFORM/'system/core/healthd/BatteryMonitor.cpp',KERNEL/'drivers/input/ff-memless.c']
data={str(p):p.read_bytes() for p in paths}
hashes={p:hashlib.sha256(d).hexdigest() for p,d in data.items()}
(HERE/'extended-source-hashes.json').write_text(json.dumps(hashes,indent=2)+'\n')
with zipfile.ZipFile(HERE/'extended-reviewed-source.zip','w',zipfile.ZIP_DEFLATED) as z:
    for p,d in data.items():
        path=Path(p)
        base=DEV if path.is_relative_to(DEV) else PLATFORM if path.is_relative_to(PLATFORM) else KERNEL
        prefix='device' if base==DEV else 'platform' if base==PLATFORM else 'kernel'
        z.writestr(prefix+'/'+str(path.relative_to(base)),d)
results=[]
with tempfile.TemporaryDirectory(prefix='a6l-review5-extended-') as tmp:
    w=Path(tmp)
    vib=data[str(paths[7])].decode();dualux=data[str(paths[1])].decode()
    methods=[extract(vib,s) for s in ('InputFFDevice::InputFFDevice()', 'bool InputFFDevice::isPresent()', 'int InputFFDevice::play(')]
    (w/'vibrator_methods.inc').write_text('\n'.join(methods))
    (w/'soc.inc').write_text('\n'.join(line for line in vib.splitlines() if line.startswith('#define ') and '_CPU_' in line))
    (w/'backlight_method.inc').write_text(extract(dualux,'static void enforce_backlight(void)'))
    for path in paths[2:4]:(w/path.name).write_bytes(data[str(path)])
    (w/'input').mkdir();(w/'input/event0').touch()
    flags=['-g','-O1','-fsanitize=address,undefined','-fno-omit-frame-pointer','-no-pie','-I'+str(w)]
    for name,cmd,args in (
        ('vibrator',['g++','-std=c++17','-pthread',*flags,HERE/'vibrator.cc','-o',w/'vibrator'],[w/'input']),
        ('backlight',['gcc','-std=gnu11',*flags,HERE/'backlight.c',w/'dualux_logic.c','-lm','-o',w/'backlight'],[])):
        (HERE/(name+'-build.log')).write_text(run(cmd))
        out=run([w/name,*args]);(HERE/(name+'.log')).write_text(out)
        print(out,flush=True);results.append(dict(test=name,output=out.strip()))
changed=[p for p,h in hashes.items() if hashlib.sha256(Path(p).read_bytes()).hexdigest()!=h]
assert not changed,changed
(HERE/'extended-results.json').write_text(json.dumps(results,indent=2)+'\n')
(HERE/'extended-changed-during-run.json').write_text(json.dumps(changed)+'\n')
print('EXTENDED_REPRODUCTIONS_PASS')
