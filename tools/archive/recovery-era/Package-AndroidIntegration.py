"""Package V39 live ADB payload and a separate diskless QEMU test ramdisk."""
import argparse
import gzip
import hashlib
import json
from pathlib import Path
import re
import subprocess

ROOT=Path(__file__).resolve().parents[1]
A=Path('/home/a6l/android/a6l-lineage24')
PRODUCT=A/'out/target/product/a6l'
def sha(b):return hashlib.sha256(b).hexdigest()
ap=argparse.ArgumentParser();ap.add_argument('--attempt',type=int,required=True);n=ap.parse_args().attempt
out=ROOT/f'firmware/extracted/android-integration-v39-20260917-r{n}';out.mkdir(exist_ok=False)
tree=Path(f'/home/a6l/kernel/android-integration-v39-r{n}');tree.mkdir(exist_ok=False)
payload=tree/'payload';payload.mkdir()
log=Path('/home/a6l/logs/build-android-integration-v39.log').read_bytes()
assert b'#### build completed successfully' in log[-16384:]
files={}
def put(name,source,mode):
    data=source.read_bytes();p=payload/name;p.parent.mkdir(parents=True,exist_ok=True);p.write_bytes(data);p.chmod(mode)
    files[name]={'sha256':sha(data),'bytes':len(data),'mode':mode,'source':str(source)}
put('bin/a6l_integration_probe',PRODUCT/'system/bin/a6l_integration_probe',0o755)
recovery=PRODUCT/'recovery/root/system'
for name in ['servicemanager','a6l_binder_client']:put('bin/'+name,recovery/'bin'/name,0o755)
queue=list(files);done=set()
while queue:
    name=queue.pop()
    if name in done:continue
    done.add(name)
    text=subprocess.check_output(['readelf','-l','-d',str(payload/name)],text=True)
    for lib in re.findall(r'\(NEEDED\).*\[(.*?)\]',text):
        target='lib64/'+lib
        if target not in files:put(target,recovery/target,0o644);queue.append(target)
    for interp in re.findall(r'Requesting program interpreter: (.*?)\]',text):
        assert interp=='/system/bin/linker64'
        old=ROOT/'firmware/extracted/android-ram-v38-20260917-r2/report.json'
        expected=json.loads(old.read_text())['files'][interp]['sha256']
        assert sha((recovery/'bin/linker64').read_bytes())==expected
for name in files:
    dest=out/'payload'/name;dest.parent.mkdir(parents=True,exist_ok=True);dest.write_bytes((payload/name).read_bytes())
(out/'manifest.json').write_text(json.dumps({'files':files,'scope':'RAM-only V39 payload, V38 recovery unchanged'},indent=2)+'\n')
(out/'build.log').write_bytes(log)
for name in ['integration_probe.c','binder_client.cpp']:
    p=ROOT/'device/hisense/a6l/diagnostic'/name
    assert p.read_bytes()==(A/'device/hisense/a6l/diagnostic'/name).read_bytes()
    (out/name).write_bytes(p.read_bytes())
listing=(ROOT/'firmware/extracted/android-ram-v38-20260917-r2/ramdisk.list').read_text()
script=tree/'qemu.sh';script.write_text('''#!/system/bin/sh
exec > /dev/kmsg 2>&1
/system/bin/toybox cp -R /v39 /tmp/a6l-v39
(
  /system/bin/toybox sleep 4
  for p in /proc/[0-9]*; do
    name=$(/system/bin/toybox cat "$p/comm" 2>/dev/null)
    if [ "$name" = servicemanager ]; then
      echo A6L_QEMU_SM_WAIT
      /system/bin/toybox cat "$p/wchan" "$p/stack"
    fi
  done
) &
/tmp/a6l-v39/bin/a6l_integration_probe --binder
echo A6L_QEMU_BINDER_EXIT=$?
/tmp/a6l-v39/bin/a6l_integration_probe --filesystems
echo A6L_QEMU_NO_EMMC_EXIT=$?
echo A6L_QEMU_INTEGRATION_FINISHED
''')
rc=tree/'init.rc';rc.write_text((ROOT/'device/hisense/a6l/diagnostic/android-init.rc').read_text()+'''
on early-init
    start qemuprobe

service qemuprobe /system/bin/sh /v39/qemu.sh
    user root
    group root
    disabled
    oneshot
    seclabel u:r:su:s0
''')
lines=listing.splitlines()
lines=[f'file /system/etc/init/hw/init.rc {rc} 0644 0 0' if x.startswith('file /system/etc/init/hw/init.rc ') else x for x in lines]
dirs={'/v39'}
for name in files:
    dirs.update('/v39/'+str(p) for p in Path(name).parents if str(p)!='.')
lines += [f'dir {d} 0755 0 0' for d in sorted(dirs)]
lines += [f'file /v39/{name} {payload/name} {v["mode"]:04o} 0 0' for name,v in sorted(files.items())]
lines += [f'file /v39/qemu.sh {script} 0755 0 0']
recipe=tree/'ramdisk.list';recipe.write_text('\n'.join(lines)+'\n')
cpio=subprocess.check_output(['/home/a6l/kernel/out-a6l-android-init/usr/gen_init_cpio','-t','1789344000',str(recipe)])
ram=gzip.compress(cpio,mtime=0);(out/'qemu-ramdisk.cpio.gz').write_bytes(ram)
(out/'qemu-ramdisk.list').write_bytes(recipe.read_bytes())
print(json.dumps({'files':len(files),'payload_bytes':sum(v['bytes'] for v in files.values()),'qemu_ramdisk_sha256':sha(ram),'output':str(out)}))
