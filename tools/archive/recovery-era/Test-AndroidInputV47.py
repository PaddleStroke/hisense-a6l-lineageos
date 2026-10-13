"""Test real Android graphics services plus scoped simpleDRM adaptations in diskless QEMU."""
import argparse,gzip,hashlib,json,re,subprocess,time,os,socket,struct
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1];A=Path('/home/a6l/android/a6l-lineage24');P=A/'out/target/product/a6l'
ap=argparse.ArgumentParser();ap.add_argument('--attempt',type=int,required=True);n=ap.parse_args().attempt
OUT=Path(f'/home/a6l/kernel/input-v47-r{n}');OUT.mkdir(exist_ok=False)
ARCH=ROOT/f'firmware/extracted/android-input-v47-20260918-r{n}';ARCH.mkdir(exist_ok=False)
files={};payload=OUT/'payload';sha=lambda x:hashlib.sha256(x).hexdigest()
def put(name,src,mode=0o644):
    data=src.read_bytes();p=payload/name;p.parent.mkdir(parents=True,exist_ok=True);p.write_bytes(data)
    files[name]={'sha256':sha(data),'bytes':len(data),'mode':mode,'source':str(src)}
def content(name,data):
    p=payload/name;p.parent.mkdir(parents=True,exist_ok=True);p.write_text(data)
    files[name]={'sha256':sha(p.read_bytes()),'bytes':p.stat().st_size,'mode':0o644,'source':'fixture configuration'}
put('bin/a6l_graphics_services',P/'system/bin/a6l_input_services',0o755)
put('a6l_simplefb.ko',ROOT/'firmware/extracted/android-display-v40-20260917-r1/a6l_simplefb.ko',0o400)
touch=ROOT/'firmware/extracted/controls-v45-prep-20260917/payload/edt-ft5x06.ko'
assert sha(touch.read_bytes())=='b138586b7e8b3d77e0fb9d1e66b6627bb6e94dec8c363a83686dd93c9e3984d5'
put('edt-ft5x06.ko',touch,0o400)
put('root/system/bin/linker64',P/'recovery/root/system/bin/linker64',0o755)
seeds=[('system','bin/servicemanager'),('system','bin/hwservicemanager'),('system','bin/a6l_input_client'),('system','bin/surfaceflinger'),('system','lib64/libEGL_angle.so'),('system','lib64/libGLESv1_CM_angle.so'),('system','lib64/libGLESv2_angle.so'),('vendor','lib64/hw/vulkan.pastel.so'),
       ('vendor','bin/hw/android.hardware.graphics.allocator-service.minigbm'),
       ('vendor','bin/hw/android.hardware.composer.hwc3-service.drm'),('vendor','lib64/hw/mapper.minigbm.so')]
for group,path in seeds:
    name=f'root/{group}/{path}';put(name,(A/'out/soong/.intermediates/system/hwservicemanager/hwservicemanager/android_arm64_armv8-a/hwservicemanager' if path=='bin/hwservicemanager' else P/group/path),0o755 if path.startswith('bin/') else 0o644)
    queue=[name];seen=set()
    while queue:
        current=queue.pop()
        if current in seen:continue
        seen.add(current)
        text=subprocess.check_output(['readelf','-l','-d',str(payload/current)],text=True)
        for interp in re.findall(r'Requesting program interpreter: (.*?)\]',text):
            assert interp in ['/system/bin/linker64','/system/bin/bootstrap/linker64'],interp
            dest='root'+interp
            if dest not in files:
                put(dest,P/interp.lstrip('/'),0o755)
        for lib in re.findall(r'\(NEEDED\).*\[(.*?)\]',text):
            name=f'root/{group}/lib64/{lib}'
            if name in files:continue
            candidates=[P/group/'lib64'/lib,P/'system/lib64'/lib,P/'system_ext/lib64'/lib,
                        P/'apex/com.android.runtime/lib64/bionic'/lib,P/'system/lib64/bootstrap'/lib,
                        P/'apex/com.android.os.statsd/lib64'/lib]
            found=next((f for f in candidates if f.is_file()),None);assert found,(group,lib)
            put(name,found);queue.append(name)
put('root/system/etc/selinux/plat_service_contexts',P/'system/etc/selinux/plat_service_contexts')
put('root/system/etc/selinux/plat_hwservice_contexts',P/'system/etc/selinux/plat_hwservice_contexts')
manifest='''<manifest version="1.0" type="device">
  <hal format="aidl"><name>android.hardware.graphics.allocator</name><version>3</version><fqname>IAllocator/default</fqname></hal>
  <hal format="aidl"><name>android.hardware.graphics.composer3</name><version>5</version><fqname>IComposer/default</fqname></hal>
  <hal format="native"><name>mapper</name><version>5.0</version><interface><instance>minigbm</instance></interface></hal>
</manifest>
'''
content('root/vendor/etc/vintf/manifest.xml',manifest)
content('root/system/etc/vintf/manifest.xml','<manifest version="1.0" type="framework"><hal format="hidl"><name>android.hidl.manager</name><transport>hwbinder</transport><version>1.2</version><interface><name>IServiceManager</name><instance>default</instance></interface></hal></manifest>\n')
for name in ['generic_ft5x06__8d_', 'A6L_Input_Fixture']:
    content('root/system/usr/idc/'+name+'.idc','device.internal = 1\ntouch.deviceType = touchScreen\ntouch.orientationAware = 1\n')
for name,info in files.items():
    p=ARCH/'payload'/name;p.parent.mkdir(parents=True,exist_ok=True);p.write_bytes((payload/name).read_bytes())
(ARCH/'manifest.json').write_text(json.dumps({'files':files,'scope':'Private RAM root, private Binder, real Android AIDL allocator/stable mapper/DRM composer; no storage device nodes or persistent mounts'},indent=2)+'\n')
for name in ['input_services.c','input_client.cpp','input_policy.h','input_fixture.c','private_properties.h']:
    src=ROOT/'device/hisense/a6l/diagnostic'/name;assert src.read_bytes()==(A/'device/hisense/a6l/diagnostic'/name).read_bytes();(ARCH/name).write_bytes(src.read_bytes())
build_recipe=ROOT/'device/hisense/a6l/Android.bp'
assert build_recipe.read_bytes()==(A/'device/hisense/a6l/Android.bp').read_bytes()
(ARCH/'Android.bp').write_bytes(build_recipe.read_bytes())
for name in ['drv.c','dumb_driver.c','cros_gralloc/cros_gralloc_driver.cc']:
    src=A/'external/minigbm'/name
    dest=ARCH/'minigbm-source'/name;dest.parent.mkdir(parents=True,exist_ok=True);dest.write_bytes(src.read_bytes())
(ARCH/'minigbm.patch').write_bytes(subprocess.check_output(['git','diff'],cwd=A/'external/minigbm'))
(ARCH/'drm-hwcomposer.patch').write_bytes(subprocess.check_output(['git','diff'],cwd=A/'external/drm_hwcomposer'))
(ARCH/'skia.patch').write_bytes(subprocess.check_output(['git','diff'],cwd=A/'external/skia'))
(ARCH/'ComposerClient.cpp').write_bytes((A/'external/drm_hwcomposer/hwc3/ComposerClient.cpp').read_bytes())
for log in Path('/home/a6l/logs').glob('build-android-input-v47*.log'):(ARCH/log.name).write_bytes(log.read_bytes())
fixture=P/'system/bin/a6l_input_fixture'
assert fixture.is_file()
script=OUT/'qemu.sh';script.write_text('''#!/system/bin/sh
/system/bin/toybox mknod /dev/v47-console c 204 64
exec > /dev/v47-console 2>&1
echo A6L_QEMU_GRAPHICS_START
/system/bin/toybox mkdir -p /tmp/a6l-v47
/system/bin/toybox cp -R /v47/. /tmp/a6l-v47/
/system/bin/toybox stat -c '%a %n' /tmp/a6l-v47/root /tmp/a6l-v47/root/system/bin/a6l_input_client
/system/bin/toybox chmod -R a+rX /tmp/a6l-v47/root
/system/bin/toybox insmod /tmp/a6l-v47/a6l_simplefb.ko
echo A6L_QEMU_GRAPHICS_MODULE_EXIT=$?
/v47/input-fixture > /tmp/fixture.log 2>&1 &
fixture_pid=$!
while ! /system/bin/toybox grep -q A6L_INPUT_FIXTURE_CREATED /tmp/fixture.log; do /system/bin/toybox sleep 0.1; done
ready_before=$(/system/bin/getprop servicemanager.ready)
(
  while [ ! -f /tmp/a6l-v47/root/logs/client.log ]; do /system/bin/toybox sleep 0.1; done
  /system/bin/toybox tail -n +1 -f /tmp/a6l-v47/root/logs/client.log
) &
tail_pid=$!
/tmp/a6l-v47/bin/a6l_graphics_services
probe_exit=$?
kill "$tail_pid"
if [ "$probe_exit" -ne 0 ]; then kill "$fixture_pid"; fi
echo A6L_QEMU_GRAPHICS_EXIT=$probe_exit
/system/bin/toybox cat /tmp/a6l-v47/root/logs/client.log
ready_after=$(/system/bin/getprop servicemanager.ready)
if [ "$ready_before" = "$ready_after" ]; then echo A6L_QEMU_READY_RESTORED=1; fi
for f in /tmp/a6l-v47/root/logs/client.log /tmp/a6l-v47/root/logs/*; do
 echo LOGFILE:$f
 /system/bin/toybox cat "$f"
done
/system/bin/toybox dmesg
wait "$fixture_pid"
echo A6L_QEMU_INPUT_FIXTURE_EXIT=$?
/system/bin/toybox cat /tmp/fixture.log
echo A6L_QEMU_GRAPHICS_DONE
''')
rc=OUT/'init.rc';rc.write_text((ROOT/'device/hisense/a6l/diagnostic/android-init.rc').read_text()+'''
on early-init
    start graphicstest
service graphicstest /system/bin/sh /v47/qemu.sh
    user root
    group root
    disabled
    oneshot
    seclabel u:r:su:s0
''')
v38=ROOT/'firmware/extracted/android-ram-v38-20260917-r2'
lines=(v38/'ramdisk.list').read_text().splitlines()
lines=[f'file /system/etc/init/hw/init.rc {rc} 0644 0 0' if l.startswith('file /system/etc/init/hw/init.rc ') else l for l in lines]
dirs={'/v47'}
for name in files:dirs.update('/v47/'+str(p) for p in Path(name).parents if str(p)!='.')
lines += [f'dir {p} 0755 0 0' for p in sorted(dirs)]
lines += [f'file /v47/{name} {payload/name} {i["mode"]:04o} 0 0' for name,i in files.items()]
lines += [f'file /v47/qemu.sh {script} 0755 0 0',f'file /v47/input-fixture {fixture} 0755 0 0']
recipe=OUT/'ramdisk.list';recipe.write_text('\n'.join(lines)+'\n')
data=subprocess.check_output(['/home/a6l/kernel/out-a6l-android-init/usr/gen_init_cpio','-t','1789344000',str(recipe)])
(OUT/'ramdisk.gz').write_bytes(gzip.compress(data,mtime=0))
dtb=ROOT/'firmware/extracted/android-display-v40-20260917-r1/positive/virt.dtb'
sock=Path(f'/tmp/v47-{os.getpid()}.sock')
args=['qemu-system-aarch64','-machine','virt,gic-version=3','-cpu','cortex-a53','-smp','2','-m','2048','-nodefaults','-nographic','-monitor','none','-serial','stdio','-nic','none','-no-reboot','-qmp',f'unix:{sock},server=on,wait=off','-dtb',str(dtb),'-kernel',str(ROOT/'firmware/extracted/android-init-kernel-20260917/Image'),'-initrd',str(OUT/'ramdisk.gz'),'-append','console=ttyAMA0,115200 loglevel=1 drm.debug=0x1ff panic=0 androidboot.selinux=permissive androidboot.init_rc=/system/etc/init/hw/init.rc']
sampled=False
with (OUT/'console.log').open('wb') as f:
    proc=subprocess.Popen(args,stdout=f,stderr=subprocess.STDOUT)
    try:
        deadline=time.monotonic()+10
        while not sock.exists() and time.monotonic()<deadline:time.sleep(.05)
        with socket.socket(socket.AF_UNIX,socket.SOCK_STREAM) as conn:
            conn.settimeout(5);conn.connect(str(sock));stream=conn.makefile('rwb',buffering=0);stream.readline()
            def qmp(command,arguments=None):
                req={'execute':command}
                if arguments:req['arguments']=arguments
                stream.write(json.dumps(req).encode()+b'\n')
                while True:
                    msg=json.loads(stream.readline());assert 'error' not in msg,msg
                    if 'return' in msg:return msg['return']
            qmp('qmp_capabilities');deadline=time.monotonic()+220
            while time.monotonic()<deadline:
                data=(OUT/'console.log').read_bytes()
                if not sampled and b'A6L_INPUT_FRAME_VISIBLE' in data:
                    qmp('stop');qmp('pmemsave',{'val':0x9d400000,'size':1080*2340*4+4096,'filename':str(OUT/'framebuffer.bin')});qmp('cont');sampled=True
                if b'A6L_QEMU_GRAPHICS_DONE' in data or b'Kernel panic' in data or proc.poll() is not None:break
                time.sleep(.1)
    finally:
        if proc.poll() is None:proc.kill();proc.wait(timeout=5)
        sock.unlink(missing_ok=True)
data=(OUT/'console.log').read_bytes();checks={
 'module':b'A6L_QEMU_GRAPHICS_MODULE_EXIT=0' in data,'exit_zero':b'A6L_QEMU_GRAPHICS_EXIT=0' in data,
 'services':b'A6L_INPUT_SERVICES_PASS client=1 services_alive=5 private_properties=1 namespace_cleanup=1' in data,
 'ready_property_restored':b'A6L_QEMU_READY_RESTORED=1' in data,
 'no_panic':b'Kernel panic' not in data}
checks['present']=b'A6L_INPUT_PASS ' in data and b'two_finger=1' in data
checks['system_input_identity']=b'A6L_INPUT_START uid=1000 dump_only=0' in data
checks['root_dump_helper']=b'A6L_INPUT_START uid=0 dump_only=1' in data
checks['fixture']=b'A6L_QEMU_INPUT_FIXTURE_EXIT=0' in data and b'A6L_INPUT_FIXTURE_PASS' in data
motions=[(int(a),int(n),float(x),float(y)) for a,n,x,y in re.findall(rb'A6L_INPUT_MOTION action=(\d+) pointers=(\d+) x=([\d.]+) y=([\d.]+)',data)]
checks['four_targets']=all(any(a==0 and n==1 and abs(x-tx)<2 and abs(y-ty)<2 for a,n,x,y in motions) for tx,ty in [(180,300),(900,300),(180,2040),(900,2040)])
checks['two_finger_delivery']=any(n==2 for a,n,x,y in motions)
checks['sampled']=sampled
checks['software_vulkan']=b'GLES: ' in data and b'ANGLE' in data and b'SwiftShader' in data
checks['client_composition']=b'usesClientComposition=true' in data
checks['no_composer_commit_failures']=b'Failed to commit frames: 0' in data and not re.search(rb'Failed to (?:test )?commit frames: [1-9]',data)
if sampled:
    frame=(OUT/'framebuffer.bin').read_bytes();(ARCH/'framebuffer.bin').write_bytes(frame)
    def pixel(x,y):return struct.unpack_from('<I',frame,(y*1080+x)*4)[0]
    # First pointer ends at (380,900). The visible yellow marker must follow it.
    checks['touch_driven_scanout']=pixel(380,900)==0xffffff00
    checks['outside_frame_untouched']=not any(frame[1080*2340*4:])
report={'passed':all(checks.values()),'checks':checks,'command':args,'console_sha256':sha(data),'payload_files':len(files),'payload_bytes':sum(x['bytes'] for x in files.values())}
(ARCH/'qemu-report.json').write_text(json.dumps(report,indent=2)+'\n');(ARCH/'qemu-console.log').write_bytes(data)
print(json.dumps(report,indent=2));print(data[-8000:].decode(errors='replace'));assert report['passed']
