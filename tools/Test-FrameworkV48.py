"""Diskless QEMU ART/framework bootstrap; no phone or physical block devices."""
import argparse,gzip,hashlib,json,os,re,shutil,subprocess,time,xml.etree.ElementTree as ET,zipfile,io
from pathlib import Path
R=Path(__file__).resolve().parents[1];A=Path('/home/a6l/android/a6l-lineage24');P=A/'out/target/product/a6l'
ap=argparse.ArgumentParser();ap.add_argument('--attempt',required=True,type=int);ap.add_argument('--rooted',action='store_true');ap.add_argument('--runtime-kernel',action='store_true');ap.add_argument('--apex-service',action='store_true');ap.add_argument('--native-bootstrap',action='store_true');ap.add_argument('--applications',action='store_true');ap.add_argument('--health',action='store_true');ap.add_argument('--bpf',action='store_true');ap.add_argument('--hint-compat',action='store_true');ap.add_argument('--erofs',action='store_true',help='V70+: phone-style delivery: one read-only EROFS payload + tmpfs overlay, phone kernel candidate, 6 GiB');ap.add_argument('--series',type=int,help='V57+: explicit test series number (requires --hint-compat)');opts=ap.parse_args();n=opts.attempt
assert not opts.runtime_kernel or opts.rooted, 'Runtime kernel follow-up requires the normal-root harness'
assert not opts.apex_service or opts.runtime_kernel, 'Real APEX test requires the runtime kernel'
assert not opts.native_bootstrap or opts.apex_service, 'Native bootstrap requires real APEX activation'
assert not opts.applications or opts.native_bootstrap, 'System apps require native bootstrap'
assert not opts.health or opts.applications, 'Health follow-up requires the system apps fixture'
assert not opts.bpf or opts.health, 'BPF follow-up requires Health test'
assert not opts.hint_compat or opts.bpf, 'Hint compatibility test requires BPF/Health fixture'
assert not opts.series or (opts.hint_compat and opts.series>=57), 'Series numbers extend the V56 fixture'
version=opts.series if opts.series else 56 if opts.hint_compat else (55 if opts.bpf else (54 if opts.health else (53 if opts.applications else (52 if opts.native_bootstrap else (51 if opts.apex_service else (50 if opts.runtime_kernel else (49 if opts.rooted else 48)))))))
import glob
assert not opts.erofs or (opts.series or 0)>=70,'--erofs needs --series>=70'
kernel_archive=R/(sorted(glob.glob(str(R/'firmware/extracted/phone-kernel-v67-candidate-*')))[-1] if opts.erofs else sorted(glob.glob(str(R/'firmware/extracted/framework-kernel-v59-*')))[-1] if (opts.series or 0)>=59 else 'firmware/extracted/framework-kernel-v50-20260918' if opts.runtime_kernel else 'firmware/extracted/android-init-kernel-20260917')
O=Path(f'/home/a6l/kernel/framework-v{version}-r{n}');O.mkdir(exist_ok=False)
import datetime
stamp='20260918' if version<57 else datetime.date.today().strftime('%Y%m%d')
archive=R/f'firmware/extracted/android-framework-v{version}-{stamp}-r{n}';archive.mkdir(exist_ok=False)
entries={};sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
def put(name,p,mode=None):
    if p.is_symlink():entries[name]=('slink',os.readlink(p),0o777);return
    assert p.is_file(),p
    entries[name]=('file',str(p),mode if mode is not None else (0o755 if p.stat().st_mode&0o111 else 0o644))
def tree(prefix,src,skip=()):
    for p in sorted(src.rglob('*')):
        rel=p.relative_to(src)
        if any(part in skip for part in rel.parts) or p.name.endswith('.fsv_meta'):continue
        if p.is_file() or p.is_symlink():put(prefix+'/'+str(rel),p)
def textfile(name,data,mode=0o644):
    p=O/'generated'/name;p.parent.mkdir(parents=True,exist_ok=True);p.write_text(data);put(name,p,mode)
base=R/'firmware/extracted/android-input-v47-20260918-r7'
for name,info in json.loads((base/'manifest.json').read_text())['files'].items():
    if name.startswith('root/'):
        assert sha(base/'payload'/name)==info['sha256'];put(name,base/'payload'/name,info['mode'])
tree('root/system/lib64',P/'system/lib64')
tree('root/system/framework',P/'system/framework',('arm',))
tree('root/system/etc',P/'system/etc',('init','security'))
put('root/system/etc/init/netbpfload.rc',P/'system/etc/init/netbpfload.rc')
tree('root/system/usr',P/'system/usr')
tree('root/system/fonts',P/'system/fonts')
if opts.applications:
    for directory in ['app','priv-app']:
        tree('root/system/'+directory,P/'system'/directory)
for partition in ['system_ext','product']:
    source=P/partition if (P/partition).is_dir() else P/'system'/partition
    for directory in ['framework','lib64','etc','fonts','overlay']:
        if (source/directory).is_dir():tree('root/'+partition+'/'+directory,source/directory,('init',))
    if opts.applications:
        for directory in ['app','priv-app']:
            tree('root/'+partition+'/'+directory,source/directory)
# Extract the actual packages: loose product/apex trees omit private dependencies.
cache=Path('/home/a6l/kernel/framework-apex-v48');cache.mkdir(exist_ok=True)
apexes=[]
for p in sorted((P/'system/apex').iterdir()):
    name=p.name.rsplit('.',1)[0];digest=sha(p);folder=cache/(name+'-'+digest[:16])
    src=folder/'root';marker=folder/'complete.json'
    if not marker.exists():
        folder.mkdir(exist_ok=False)
        original=p
        if p.suffix=='.capex':
            original=folder/'original.apex'
            with zipfile.ZipFile(p) as z:original.write_bytes(z.read('original_apex'))
        cmd=[str(A/'out/host/linux-x86/bin/deapexer'),'--debugfs_path',str(A/'out/host/linux-x86/bin/debugfs_static'),'--fsckerofs_path',str(A/'out/host/linux-x86/bin/fsck.erofs'),'extract',str(original),str(src)]
        r=subprocess.run(cmd,capture_output=True,text=True)
        (folder/'extract.log').write_text(r.stdout+r.stderr);assert r.returncode==0,(name,r.stderr)
        marker.write_text(json.dumps({'package':str(p),'sha256':digest}))
    assert json.loads(marker.read_text())['sha256']==digest
    apexes.append(name);tree('root/apex/'+name,src,('lib','bin32'))
    if opts.apex_service:
        # The original signed APEX embedded in a CAPEX is preserved byte for byte.
        original=folder/'original.apex' if p.suffix=='.capex' else p
        put('root/system/apex/'+name+'.apex',original)
def manifest_version(data):
    def vint(pos):
        v=shift=0
        while True:
            b=data[pos];pos+=1;v|=(b&127)<<shift
            if b<128:return v,pos
            shift+=7
    pos=0
    while pos<len(data):
        tag,pos=vint(pos);field,wire=tag>>3,tag&7
        if wire==0:
            v,pos=vint(pos)
            if field==2:return v
        elif wire==2:
            size,pos=vint(pos);pos+=size
        elif wire in (1,5):pos+=8 if wire==1 else 4
        else:raise ValueError(wire)
    raise ValueError('APEX manifest version missing')
info=ET.Element('apex-info-list')
for name in apexes:
    package=next(p for p in (P/'system/apex').iterdir() if p.name.rsplit('.',1)[0]==name)
    with zipfile.ZipFile(package) as z:
        manifest_data=z.read('apex_manifest.pb')
    dest=O/'apex-manifests'/name/'apex_manifest.pb';dest.parent.mkdir(parents=True,exist_ok=True)
    dest.write_bytes(manifest_data);put('root/apex/'+name+'/apex_manifest.pb',dest)
    version=manifest_version(manifest_data)
    ET.SubElement(info,'apex-info',moduleName=name,modulePath='/system/apex/'+package.name,
                  preinstalledModulePath='/system/apex/'+package.name,versionCode=str(version),
                  versionName=str(version),isFactory='true',isActive='true',lastUpdateMillis='0')
textfile('root/apex/apex-info-list.xml',ET.tostring(info,encoding='unicode'))
put('root/system/bin/a6l-guard.sh',R/'device/hisense/a6l/diagnostic/a6l-guard.sh',0o755)
entries['root/system/bin/tr']=('slink','toybox',0o777)
for name in ['app_process64','sh','toybox','logcat','linker64','installd']:
    put('root/system/bin/'+name,P/'system/bin'/name)
for name in ['timeout','cat','mkdir','sleep','chmod','chown','grep','sed']:
    entries['root/system/bin/'+name]=('slink','toybox',0o777)
if opts.apex_service:
    for name in ['apexd','service','getprop','toolbox']:
        put('root/system/bin/'+name,P/'system/bin'/name)
    for name in ['mknod','rm','cp','tee']:
        entries['root/system/bin/'+name]=('slink','toybox',0o777)
    tree('root/system/etc/security',P/'system/etc/security')
    put('root/system/bin/framework-apex-v51.sh',R/'device/hisense/a6l/diagnostic/framework-apex-v51.sh',0o755)
    textfile('root/system/etc/a6l-expected-apexes.txt','\n'.join(apexes)+'\n')
if opts.native_bootstrap:
    for name in ['aconfigd-system','hw/android.system.suspend-service']:
        put('root/system/bin/'+name,P/'system/bin'/name)
    entries['root/system/bin/ls']=('slink','toybox',0o777)
    put('root/system/bin/framework-native-v52.sh',R/'device/hisense/a6l/diagnostic/framework-native-v52.sh',0o755)
    # This diagnostic vendor has no vendor aconfig declarations. Generate its
    # valid empty storage with the actual build tool, never relabel system flags.
    aconfig=A/'out/host/linux-x86/bin/aconfig'
    vendor_flags=O/'vendor-aconfig';vendor_flags.mkdir()
    vendor_cache=vendor_flags/'aconfig_flags.pb'
    subprocess.run([str(aconfig),'dump','--cache',str(P/'system/etc/aconfig_flags.pb'),
                    '--filter','container:vendor','--format','protobuf','--out',str(vendor_cache)],check=True)
    assert vendor_cache.read_bytes()==b'', 'Minimal VM vendor unexpectedly has flags; review its declarations'
    put('root/vendor/etc/aconfig_flags.pb',vendor_cache)
    for filename,filetype in [('package.map','package_map'),('flag.map','flag_map'),('flag.val','flag_val'),('flag.info','flag_info')]:
        output=vendor_flags/filename
        subprocess.run([str(aconfig),'create-storage','--container','vendor','--file',filetype,
                        '--cache',str(vendor_cache),'--out',str(output)],check=True)
        put('root/vendor/etc/aconfig/'+filename,output)
entries['root/system/bin/dalvikvm']=('slink','/apex/com.android.art/bin/dalvikvm64',0o777)
if opts.health:
    # Stage the genuine vendor HAL and its ELF dependency closure. Existing
    # validated graphics vendor libraries remain pinned to the V47 fixture.
    health='root/vendor/bin/hw/android.hardware.health-service.example'
    put(health,P/'vendor/bin/hw/android.hardware.health-service.example',0o755)
    queue=[health];seen=set()
    while queue:
        current=queue.pop()
        if current in seen:continue
        seen.add(current)
        dynamic=subprocess.check_output(['readelf','-d',entries[current][1]],text=True)
        for lib in re.findall(r'\(NEEDED\).*\[(.*?)\]',dynamic):
            name='root/vendor/lib64/'+lib
            if name in entries:continue
            candidates=[P/'vendor/lib64'/lib,P/'system/lib64'/lib,P/'system/lib64/bootstrap'/lib]
            found=next((p for p in candidates if p.is_file()),None)
            assert found,('Missing Health HAL dependency',lib)
            put(name,found.resolve());queue.append(name)
    put('root/vendor/etc/vintf/manifest/android.hardware.health-service.example.xml',
        A/'hardware/interfaces/health/aidl/default/android.hardware.health-service.example.xml')
    put('root/system/bin/framework-health-v54.sh',R/'device/hisense/a6l/diagnostic/framework-health-v54.sh',0o755)
    put('root/system/bin/dumpsys',P/'system/bin/dumpsys')
if opts.series and opts.series>=58:
    for name in ['vold','idmap2d']:put('root/system/bin/'+name,P/'system/bin'/name)
    put('root/system/bin/framework-storage-v58.sh',R/'device/hisense/a6l/diagnostic/framework-storage-v58.sh',0o755)
if opts.series and opts.series>=59:
    for name in ['netd','iptables','ip','tc','ndc','a6l_socket_exec']:put('root/system/bin/'+name,P/'system/bin'/name)
    for name in ['ip6tables','iptables-restore','ip6tables-restore','iptables-save','ip6tables-save']:
        entries['root/system/bin/'+name]=('slink','iptables',0o777)
    put('root/system/bin/framework-netd-v59.sh',R/'device/hisense/a6l/diagnostic/framework-netd-v59.sh',0o755)
if opts.series and opts.series>=60:
    put('root/system/bin/audioserver',P/'system/bin/audioserver')
    for name in ['killall','tail','head']:entries['root/system/bin/'+name]=('slink','toybox',0o777)
    put('root/system/bin/framework-media-v60.sh',R/'device/hisense/a6l/diagnostic/framework-media-v60.sh',0o755)
if opts.series and opts.series>=61:
    put('root/vendor/apex/com.android.hardware.audio.apex',P/'vendor/apex/com.android.hardware.audio.apex')
    # AOSP generic audio policy: gives the example HAL its 'primary' (default) and r_submix modules.
    policy=A/'frameworks/av/services/audiopolicy/config'
    # The APEX VINTF declares default, r_submix, bluetooth, stub and usb modules, and audioserver waits
    # for every declared instance; include each module so the example HAL registers all of them.
    generic=(policy/'audio_policy_configuration_generic.xml').read_text()
    marker='<xi:include href="r_submix_audio_policy_configuration.xml"/>'
    assert generic.count(marker)==1
    extra=['bluetooth_audio_policy_configuration_7_0.xml','usb_audio_policy_configuration.xml','stub_audio_policy_configuration.xml']
    textfile('root/vendor/etc/audio_policy_configuration.xml',generic.replace(marker,marker+''.join('\n        <xi:include href="%s"/>'%n for n in extra)))
    for name in extra:put('root/vendor/etc/'+name,policy/name)
    for name in ['primary_audio_policy_configuration.xml','r_submix_audio_policy_configuration.xml','audio_policy_volumes.xml','default_volume_tables.xml','surround_sound_configuration_5_0.xml']:
        put('root/vendor/etc/'+name,policy/name)
    put('root/vendor/etc/audio_effects_config.xml',A/'hardware/interfaces/audio/aidl/default/audio_effects_config.xml')
if opts.series and opts.series>=62:
    for name in ['keystore2','gatekeeperd']:put('root/system/bin/'+name,P/'system/bin'/name)
    put('root/system/bin/framework-security-v62.sh',R/'device/hisense/a6l/diagnostic/framework-security-v62.sh',0o755)
if opts.series and opts.series>=63:
    # Genuine AOSP software ("nonsecure") KeyMint/SharedSecret/SecureClock HAL: keystore2 panics without a TEE-level KeyMint.
    keymint='root/vendor/bin/hw/android.hardware.security.keymint-service.nonsecure'
    put(keymint,P/'vendor/bin/hw/android.hardware.security.keymint-service.nonsecure',0o755)
    for lib in re.findall(r'\(NEEDED\).*\[(.*?)\]',subprocess.check_output(['readelf','-d',entries[keymint][1]],text=True)):
        if 'root/vendor/lib64/'+lib not in entries and (P/'vendor/lib64'/lib).is_file():put('root/vendor/lib64/'+lib,(P/'vendor/lib64'/lib).resolve())
    for name in ['keymint','secureclock','sharedsecret']:
        put(f'root/vendor/etc/vintf/manifest/android.hardware.security.{name}-service.xml',P/f'vendor/etc/vintf/manifest/android.hardware.security.{name}-service.xml')
if opts.series and opts.series>=64:
    # Recreate init.rc's own /data directory layout (paths, modes, owners) instead of guessing names one crash at a time.
    lines=['#!/system/bin/sh','# Generated from system/core/rootdir/init.rc mkdir entries; diskless VM only.','grep -q virt /proc/device-tree/model || exit 97']
    for m in re.finditer(r'^\s*mkdir (/data/\S+)[ \t]+(\d+)[ \t]+(\w+)[ \t]+(\w+)',(A/'system/core/rootdir/init.rc').read_text(),re.M):
        path,mode,owner,group=m.groups()
        lines.append(f'mkdir -p {path} && chmod {mode} {path} && chown {owner}:{group} {path} || echo A6L_DATADIR_FAILED path={path}')
    assert len(lines)>60,len(lines)
    lines+=['mkdir -p /data/misc/profiles/cur/0 /data/misc/profiles/ref /data/data /data/user /data/user_de/0 /data/media/0','[ -e /data/user/0 ] || ln -s /data/data /data/user/0  # init.rc: symlink /data/data /data/user/0','chmod 0771 /data/data','chown system:system /data/misc/profiles/cur/0 /data/data /data/user_de/0','echo A6L_DATADIRS_DONE count=%d'%(len(lines)-3)]
    textfile('root/system/bin/framework-datadirs-v64.sh','\n'.join(lines)+'\n',0o755)
    put('root/system/bin/vold_prepare_subdirs',P/'system/bin/vold_prepare_subdirs')
if opts.bpf:
    put('root/system/bin/bpfloader',P/'system/bin/bpfloader')
    put('root/vendor/etc/bpf/filterPowerSupplyEvents.o',P/'vendor/etc/bpf/filterPowerSupplyEvents.o')
    put('root/system/bin/framework-bpf-v55.sh',R/'device/hisense/a6l/diagnostic/framework-bpf-v55.sh',0o755)
    entries['root/system/bin/mount']=('slink','toybox',0o777)
supervisor=P/'system/bin'/('a6l_framework_root_services' if opts.rooted else 'a6l_framework_services')
put('bin/framework-services',supervisor,0o755)
put('root/system/bin/framework-services',supervisor,0o755)
put('a6l_simplefb.ko',kernel_archive/'a6l_simplefb.ko' if opts.runtime_kernel else R/'firmware/extracted/android-display-v40-20260917-r1/a6l_simplefb.ko',0o400)
props={}
for line in (P/'system/build.prop').read_text().splitlines():
    if '=' in line and not line.startswith('#'):
        k,v=line.split('=',1)
        if k.startswith(('ro.build.','ro.product.','dalvik.vm.','ro.dalvik.')) and len(v)<92:props[k]=v
props.update({'dalvik.vm.boot-image':'/system/framework/boot.art','ro.zygote':'zygote64','ro.product.cpu.abilist':'arm64-v8a','ro.product.cpu.abilist64':'arm64-v8a','ro.product.cpu.abilist32':'','dalvik.vm.heapsize':'256m','dalvik.vm.heapgrowthlimit':'192m','dalvik.vm.usejit':'false'})
props['sys.use_memfd']='true'  # Supported libcutils switch; this mainline kernel has no ashmem.
props['ro.property_service.version']='2'  # The private server implements PROP_MSG_SETPROP2.
if opts.runtime_kernel:
    props['ro.dalvik.vm.enable_uffd_gc']='true'
    props['ro.hardware.virtual_device']='1'
if opts.bpf:
    # Reproduce init's default for this VM's newly built vendor fixture, with
    # no frozen board level. This does NOT describe A6L's stock vendor image.
    api=int(props.get('ro.product.first_api_level',props['ro.build.version.sdk']))
    assert 1 <= api < 10000
    vendor_source=(A/'system/core/libvendorsupport/version_props.cpp').read_text()
    assert '202404 + ((sdkApiLevel - __ANDROID_API_V__) * 100)' in vendor_source
    props['ro.vendor.api_level']=str(api if api<35 else 202404+(api-35)*100)
    if (opts.series or 0)>=61:
        # Normally set by the bootloader/device config to select the vendor audio APEX; the HAL finds its XML configuration through it.
        props['ro.boot.vendor.apex.com.android.hardware.audio']='com.android.hardware.audio'
if opts.apex_service:
    # tmpfs cannot pin file extents. Use apexd's supported loop-file path.
    props['apexd.config.use_fiemap']='false'
    props['ro.apex.updatable']='true'
textfile('root/system/etc/a6l-runtime.prop',''.join(f'{k}={v}\n' for k,v in sorted(props.items())))
javac=A/'prebuilts/jdk/jdk21/linux-x86/bin/javac'
classes=O/'classes';classes.mkdir();dex=O/'dex';dex.mkdir()
subprocess.run([str(javac),'--release','17','-d',str(classes),str(R/'device/hisense/a6l/diagnostic/FrameworkProbe.java')],check=True)
env=dict(os.environ,JAVA_HOME=str(javac.parent.parent));env['PATH']=str(javac.parent)+':'+env['PATH']
subprocess.run([str(A/'out/host/linux-x86/bin/d8'),'--min-api','36','--output',str(dex),str(classes/'org/a6l/probe/FrameworkProbe.class')],env=env,check=True)
put('root/system/framework/a6l-probe.dex',dex/'classes.dex')
textfile('root/system/bin/framework-probe.sh',r'''#!/system/bin/sh
export PATH=/system/bin:/apex/com.android.art/bin:/apex/com.android.sdkext/bin
export ANDROID_ROOT=/system ANDROID_DATA=/data ANDROID_STORAGE=/storage
export ANDROID_ART_ROOT=/apex/com.android.art ANDROID_I18N_ROOT=/apex/com.android.i18n ANDROID_TZDATA_ROOT=/apex/com.android.tzdata
mkdir -p /data/system/environ /data/dalvik-cache/arm64 /data/misc /data/resource-cache /data/local/tmp /data/system_ce/0 /data/system_de/0 /data/user/0 /data/user_de/0
mkdir -p /storage /mnt/user /mnt/runtime/default /mnt/runtime/read /mnt/runtime/write /mnt/runtime/full /mnt/pass_through /mnt/installer /mnt/androidwritable
chmod 0711 /data /data/dalvik-cache
mkdir -p /linkerconfig
echo A6L_LINKERCONFIG_BEGIN
/apex/com.android.runtime/bin/linkerconfig --target /linkerconfig || exit 9
echo A6L_LINKERCONFIG_DONE
cat /linkerconfig/ld.config.txt > /logs/linkerconfig.log
# Bootstrap library paths are only for tools before namespace generation.
unset LD_LIBRARY_PATH
echo A6L_CLASSPATH_BEGIN
/apex/com.android.sdkext/bin/derive_classpath /data/system/environ/classpath || exit 10
cat /data/system/environ/classpath
sed 's/^export \([^ ]*\) /export \1=/' /data/system/environ/classpath > /data/classpath.sh
. /data/classpath.sh
echo A6L_CLASSPATH_DONE
export CLASSPATH=/system/framework/a6l-probe.dex
echo A6L_ART_BEGIN
timeout 65 /apex/com.android.art/bin/dalvikvm64 -Ximage:/system/framework/boot.art -Xbootclasspath:$BOOTCLASSPATH -cp "$CLASSPATH" org.a6l.probe.FrameworkProbe art
art_result=$?
echo A6L_ART_EXIT=$art_result
echo A6L_APP_PROCESS_BEGIN
timeout 65 /system/bin/app_process64 /system/bin org.a6l.probe.FrameworkProbe
app_result=$?
echo A6L_APP_PROCESS_EXIT=$app_result
if [ "$art_result" -eq 0 ] && [ "$app_result" -eq 0 ]; then
  echo A6L_SYSTEMSERVER_BEGIN
  export CLASSPATH=$SYSTEMSERVERCLASSPATH
  mkdir -p /data/system /data/misc/profiles /data/misc/zygote /data/misc/apexdata /data/misc/apexdata/com.android.art /data/misc/apexdata/com.android.art/dalvik-cache
  chown -R 1000:1000 /data/system /data/misc
  mkdir -p /data/app /data/app-private /data/app-ephemeral /data/misc/installd /mnt/expand
  /system/bin/installd > /logs/installd.log 2>&1 &
  echo A6L_INSTALLD_STARTED pid=$!
  timeout 100 /system/bin/framework-services --zygote
  echo A6L_SYSTEMSERVER_EXIT=$?
  echo A6L_RUNTIME_FOUNDATION_PASS
  exit 0
fi
exit 11
''',0o755)
if opts.apex_service:
    probe=O/'generated/root/system/bin/framework-probe.sh'
    probe.write_text(probe.read_text().replace('echo A6L_CLASSPATH_BEGIN',
        '/system/bin/framework-apex-v51.sh || exit 12\n'
        '/apex/com.android.runtime/bin/linkerconfig --target /linkerconfig || exit 13\n'
        'echo A6L_CLASSPATH_BEGIN',1).replace('timeout ','timeout --foreground -k 3 '))
if opts.native_bootstrap:
    probe.write_text(probe.read_text().replace('echo A6L_CLASSPATH_BEGIN',
        '/system/bin/framework-native-v52.sh || exit 14\necho A6L_CLASSPATH_BEGIN',1).replace(
        'export ANDROID_ROOT=/system ANDROID_DATA=/data ANDROID_STORAGE=/storage',
        'export ANDROID_ROOT=/system ANDROID_DATA=/data ANDROID_STORAGE=/storage ASEC_MOUNTPOINT=/mnt/asec\nmkdir -p /mnt/asec'))
if opts.applications:
    probe.write_text(probe.read_text().replace('mkdir -p /data/system /data/misc/profiles',
        'mkdir -p /data/system /data/misc/user /data/misc/profiles',1).replace(
        'export ANDROID_ROOT=/system',
        'export ANDROID_BOOTLOGO=1 ANDROID_ASSETS=/system/app EXTERNAL_STORAGE=/sdcard\nexport ANDROID_ROOT=/system',1).replace(
        '  timeout --foreground -k 3 100 /system/bin/framework-services --zygote',
        '''  i=0
  while [ "$i" -lt 15 ]; do
    service check installd > /logs/installd-binder.txt
    if grep -q ': found' /logs/installd-binder.txt; then echo A6L_INSTALLD_SERVICE_PASS; break; fi
    sleep 1
    i=$((i+1))
  done
  timeout --foreground -k 3 180 /system/bin/framework-services --zygote''',1))
# Initramfs paths live directly below /tmp: no second copy consumes guest RAM.
if opts.health:
    probe.write_text(probe.read_text().replace('echo A6L_CLASSPATH_BEGIN',
        '/system/bin/framework-health-v54.sh || exit 15\necho A6L_CLASSPATH_BEGIN',1))
if opts.series and opts.series>=58:
    probe.write_text(probe.read_text().replace('echo A6L_CLASSPATH_BEGIN',
        '/system/bin/framework-storage-v58.sh || exit 17\necho A6L_CLASSPATH_BEGIN',1))
if opts.bpf:
    probe.write_text(probe.read_text().replace('/system/bin/framework-native-v52.sh || exit 14',
        '/system/bin/framework-bpf-v55.sh || exit 16\n/system/bin/framework-native-v52.sh || exit 14',1))
if opts.series and opts.series>=60:
    text=probe.read_text()
    assert text.count('/system/bin/framework-apex-v51.sh || exit 12')==1 and text.count('  echo A6L_SYSTEMSERVER_EXIT=$?')==1
    text=text.replace('/system/bin/framework-apex-v51.sh || exit 12',
        'timeout --foreground -k 3 3000 /system/bin/vold --blkid_context=u:r:blkid:s0 --blkid_untrusted_context=u:r:blkid_untrusted:s0 --fsck_context=u:r:fsck:s0 --fsck_untrusted_context=u:r:fsck_untrusted:s0 > /logs/vold.log 2>&1 &\necho $! > /logs/vold-early.pid\n/system/bin/framework-apex-v51.sh || exit 12',1)
    text=text.replace('echo A6L_CLASSPATH_BEGIN','/system/bin/framework-media-v60.sh || echo A6L_MEDIA_FAILED result=$?\necho A6L_CLASSPATH_BEGIN',1)
    if opts.series>=62:text=text.replace('echo A6L_CLASSPATH_BEGIN','/system/bin/framework-security-v62.sh || echo A6L_SECURITY_FAILED result=$?\necho A6L_CLASSPATH_BEGIN',1)
    if opts.series>=64:text=text.replace('echo A6L_CLASSPATH_BEGIN','/system/bin/framework-datadirs-v64.sh || echo A6L_DATADIRS_FAILED result=$?\necho A6L_CLASSPATH_BEGIN',1)
    # init.rc gives zygote 'rlimit nofile 32768'; the shell default (1024) made NetworkStats fail with EMFILE after boot (V65).
    if opts.series>=66:text=text.replace('  echo A6L_SYSTEMSERVER_BEGIN','  ulimit -n 32768\n  echo A6L_SYSTEMSERVER_BEGIN',1)
    # Reap daemons that detach from the supervisor's process group before binderfs/bpffs cleanup.
    text=text.replace('  echo A6L_SYSTEMSERVER_EXIT=$?','  echo A6L_SYSTEMSERVER_EXIT=$?\n  for svc in media.audio_flinger media.audio_policy android.system.keystore2.IKeystoreService/default android.service.gatekeeper.IGateKeeperService netd vold; do service check $svc | grep -q ": found" && echo A6L_POST_SERVICE_PASS name=$svc || echo A6L_POST_SERVICE_MISSING name=$svc; done\n  killall -9 android.hardware.security.keymint-service.nonsecure netd audioserver vold idmap2d keystore2 gatekeeperd android.hardware.audio.service-aidl.example android.hardware.audio.effect.service-aidl.example iptables-restore ip6tables-restore 2>/dev/null || true\n  sleep 2',1)
    assert text.count('-k 3 180 /system/bin/framework-services --zygote')==1
    text=text.replace('-k 3 180 /system/bin/framework-services --zygote','-k 3 900 /system/bin/framework-services --zygote',1)
    probe.write_text(text)
if opts.series and opts.series>=59:
    probe.write_text(probe.read_text().replace('echo A6L_CLASSPATH_BEGIN',
        '/system/bin/framework-netd-v59.sh || echo A6L_NETD_FAILED result=$?\necho A6L_CLASSPATH_BEGIN',1))
rc=O/'init.rc';rc.write_text((R/'device/hisense/a6l/diagnostic/android-init.rc').read_text()+'''
on early-init
    start frameworktest
service frameworktest /system/bin/sh /v48/qemu.sh
    user root
    group root
    disabled
    oneshot
    seclabel u:r:su:s0
''')
script=O/'qemu.sh';script.write_text('''#!/system/bin/sh
/system/bin/toybox mknod /dev/v48-console c 204 64
exec > /dev/v48-console 2>&1
echo A6L_QEMU_FRAMEWORK_START
/system/bin/toybox mkdir -p /tmp/a6l-v48
/system/bin/toybox mount --bind /v48/payload /tmp/a6l-v48
/system/bin/toybox mount --bind /tmp/a6l-v48/root /tmp/a6l-v48/root
/system/bin/toybox chmod -R a+rX /tmp/a6l-v48/root
/system/bin/toybox insmod /tmp/a6l-v48/a6l_simplefb.ko
(
 while [ ! -f /tmp/a6l-v48/root/logs/framework.log ]; do /system/bin/toybox sleep 0.1; done
 /system/bin/toybox tail -n +1 -f /tmp/a6l-v48/root/logs/framework.log
) &
tail_pid=$!
/tmp/a6l-v48/bin/framework-services
result=$?
kill "$tail_pid"
echo A6L_QEMU_FRAMEWORK_EXIT=$result
for f in /tmp/a6l-v48/root/logs/*; do
 echo LOGFILE:$f
 /system/bin/toybox cat "$f"
done
echo A6L_QEMU_FRAMEWORK_DONE
''')
if opts.rooted:
    rc.write_text('''on early-init
    mkdir /tmp 0770 root root
    mount tmpfs tmpfs /tmp nosuid nodev
    symlink /dev/pts/ptmx /dev/ptmx
    start frameworktest
service frameworktest /system/bin/sh /v48/qemu.sh
    user root
    group root
    disabled
    oneshot
    seclabel u:r:su:s0
''')
    script.write_text('''#!/system/bin/sh
/system/bin/toybox mknod /dev/v49-console c 204 64
exec > /dev/v49-console 2>&1
echo A6L_QEMU_FRAMEWORK_START
# This VM has no physical or virtual disks. All overlays are initramfs files.
/system/bin/toybox insmod /v48/payload/a6l_simplefb.ko
for part in apex vendor system_ext product system; do
 /system/bin/toybox mount --bind /v48/payload/root/$part /$part || exit 20
done
/system/bin/toybox mount --bind /system/etc /etc || exit 21
/system/bin/toybox mkdir -p /logs
(
 while [ ! -f /logs/framework.log ]; do /system/bin/toybox sleep 0.1; done
 /system/bin/toybox tail -n +1 -f /logs/framework.log
) &
tail_pid=$!
/v48/payload/bin/framework-services
result=$?
kill "$tail_pid"
echo A6L_QEMU_FRAMEWORK_EXIT=$result
for f in /logs/*; do
 echo LOGFILE:$f
 /system/bin/toybox cat "$f"
done
echo A6L_QEMU_FRAMEWORK_DONE
''')
lines=(R/'firmware/extracted/android-ram-v38-20260917-r2/ramdisk.list').read_text().splitlines()
if opts.apex_service:
    rc.write_text(rc.read_text().replace('on early-init\n',
        'on early-init\n    write /dev/kmsg A6L_V51_INIT_EARLY\n',1).replace(
        '    seclabel u:r:su:s0\n','    seclabel u:r:su:s0\n    stdio_to_kmsg\n',1))
    script.write_text(script.read_text().replace('#!/system/bin/sh\n',
        '#!/system/bin/sh\necho A6L_V51_SHELL_STARTED > /dev/kmsg\nset -x\n',1))
lines=[f'file /system/etc/init/hw/init.rc {rc} 0644 0 0' if l.startswith('file /system/etc/init/hw/init.rc ') else l for l in lines]
dirs={'/v48','/v48/payload'}
for name in entries:dirs.update('/v48/payload/'+str(p) for p in Path(name).parents if str(p)!='.')
lines += [f'dir {p} 0755 0 0' for p in sorted(dirs)]
if opts.erofs:
    # Phone-style delivery: the whole payload is ONE compressed read-only image (what `adb push` would place in
    # tmpfs); a tmpfs overlay provides the few writable paths. RAM cost ~ image size instead of the expanded tree.
    import tarfile
    stage=O/'stage';stage.mkdir()
    for name,(kind,src,mode) in entries.items():
        dest=stage/name;dest.parent.mkdir(parents=True,exist_ok=True)
        if kind=='slink':os.symlink(src,dest)
        else:shutil.copyfile(src,dest);os.chmod(dest,mode)
    image=O/'payload.erofs'
    subprocess.run(['/home/a6l/android/a6l-lineage24/out/host/linux-x86/bin/mkfs.erofs','-zlz4hc','--all-root','-T','1789344000',str(image),str(stage)],check=True,stdout=subprocess.DEVNULL)
    shutil.rmtree(stage)
    with tarfile.open(kernel_archive/'modules.tar.gz') as t:
        (O/'overlay.ko').write_bytes(t.extractfile(next(m for m in t.getmembers() if m.name.endswith('/overlay.ko'))).read())
    lines=[l for l in lines if not l.startswith('dir /v48/payload/')]
    lines+=[f'file /v48/payload.erofs {image} 0400 0 0',f'file /v48/overlay.ko {O/"overlay.ko"} 0400 0 0','dir /v48/lower 0755 0 0','dir /v48/rw 0755 0 0']
    prelude='''echo A6L_EROFS_DELIVERY_BEGIN
/system/bin/toybox mknod /dev/loop-control c 10 237 2>/dev/null
/system/bin/toybox mknod /dev/loop7 b 7 7 2>/dev/null
/system/bin/toybox losetup -r /dev/loop7 /v48/payload.erofs || exit 30
/system/bin/toybox mount -t erofs -o ro /dev/loop7 /v48/lower || exit 31
/system/bin/toybox insmod /v48/overlay.ko || exit 32
/system/bin/toybox mount -t tmpfs -o size=1024m tmpfs /v48/rw || exit 33
/system/bin/toybox mkdir -p /v48/rw/upper /v48/rw/work
/system/bin/toybox mount -t overlay -o lowerdir=/v48/lower,upperdir=/v48/rw/upper,workdir=/v48/rw/work overlay /v48/payload || exit 34
echo A6L_EROFS_DELIVERY_PASS
'''
    text=script.read_text();marker='# This VM has no physical or virtual disks. All overlays are initramfs files.\n'
    assert text.count(marker)==1;script.write_text(text.replace(marker,prelude,1))
    print(f'EROFS image_bytes={image.stat().st_size} sha256={sha(image)}',flush=True)
else:
    for name,(kind,src,mode) in entries.items():lines.append(f'{kind} /v48/payload/{name} {src} {mode:04o} 0 0')
lines += [f'file /v48/qemu.sh {script} 0755 0 0']
recipe=O/'ramdisk.list';recipe.write_text('\n'.join(lines)+'\n')
manifest={name:{'kind':kind,'source':src,'mode':mode,'sha256':sha(Path(src)) if kind=='file' else None} for name,(kind,src,mode) in entries.items()}
(archive/'manifest.json').write_text(json.dumps({'files':manifest,'apexes':apexes,'scope':'offline real apexd activation experiment' if opts.apex_service else 'offline expanded APEX runtime; does not validate apexd activation'},indent=2))
print(f'PACKAGING entries={len(entries)}',flush=True)
data=subprocess.check_output(['/home/a6l/kernel/out-a6l-android-init/usr/gen_init_cpio','-t','1789344000',str(recipe)])
with gzip.open(O/'ramdisk.gz','wb',compresslevel=1) as f:f.write(data)
del data
print(f'PACKAGED compressed_bytes={(O/"ramdisk.gz").stat().st_size}',flush=True)
vm_dtb=R/'firmware/extracted/android-display-v40-20260917-r1/positive/virt.dtb'
if opts.apex_service:
    # The V40 fixture advertises only 2 GiB, regardless of QEMU's -m value.
    vm_dtb=O/'virt.dtb';shutil.copyfile(R/'firmware/extracted/android-display-v40-20260917-r1/positive/virt.dtb',vm_dtb)
    subprocess.run(['fdtput','-t','x',str(vm_dtb),'/memory@40000000','reg','0','40000000',
                    '2' if opts.applications and not opts.erofs else '1','0' if opts.applications and not opts.erofs else '80000000'],check=True)  # --erofs: 6 GiB like the phone
    shutil.copyfile(vm_dtb,archive/'virt.dtb')
args=['qemu-system-aarch64','-machine','virt,gic-version=3','-cpu','cortex-a53','-smp','4','-m','6144' if opts.erofs else '8192' if opts.applications else ('6144' if opts.apex_service else '3072'),'-nodefaults','-nographic','-monitor','none','-serial','stdio','-nic','none','-no-reboot','-dtb',str(vm_dtb),'-kernel',str(kernel_archive/'Image'),'-initrd',str(O/'ramdisk.gz'),'-append','console=ttyAMA0,115200 earlycon loglevel=7 panic=0 androidboot.selinux=permissive androidboot.init_rc=/system/etc/init/hw/init.rc']
with (O/'console.log').open('wb') as f:
    p=subprocess.Popen(args,stdout=f,stderr=subprocess.STDOUT)
    try:
        deadline=time.monotonic()+(1700 if (opts.series or 0)>=60 else 420)
        while p.poll() is None and time.monotonic()<deadline:
            if b'A6L_QEMU_FRAMEWORK_DONE' in (O/'console.log').read_bytes():break
            time.sleep(2)
    finally:
        if p.poll() is None:p.terminate()
        p.wait(timeout=10)
log=(O/'console.log').read_text(errors='replace');(archive/'console.log').write_text(log)
shutil.copyfile(O/'console.log',archive/'console.raw.log')
checks={k:s in log for k,s in {'classpath':'A6L_CLASSPATH_DONE','art':'A6L_ART_CORE_PASS','framework_jni':'A6L_FRAMEWORK_JNI_PASS','shared_memory':'A6L_SHARED_MEMORY_PASS','binder':'A6L_FRAMEWORK_BINDER_PASS','systemserver_attempted':'A6L_SYSTEMSERVER_BEGIN','cleanup':'A6L_FRAMEWORK_SERVICES_PASS','done':'A6L_QEMU_FRAMEWORK_DONE'}.items()}
if opts.apex_service:
    checks.update(apex_mounts='A6L_APEX_MOUNTS_PASS' in log,apex_service='A6L_APEX_SERVICE_PASS' in log)
if opts.native_bootstrap:
    checks.update(aconfig_storage='A6L_ACONFIG_STORAGE_PASS' in log,suspend_service='A6L_SYSTEM_SUSPEND_SERVICE_PASS' in log)
if opts.applications:
    checks.update(installd_service='A6L_INSTALLD_SERVICE_PASS' in log)
if opts.health:
    checks.update(health_service='A6L_HEALTH_SERVICE_PASS' in log)
if opts.bpf:
    checks.update(bpf_loader='A6L_BPF_LOADER_PASS' in log)
if opts.series and opts.series>=58:
    checks.update(vold_service='A6L_VOLD_SERVICE_PASS' in log,idmap_service='A6L_IDMAP_SERVICE_PASS' in log)
if opts.series and opts.series>=59:
    checks.update(netd_service='A6L_NETD_SERVICE_PASS' in log)
if opts.series and opts.series>=60:
    checks.update(audioflinger_service='A6L_POST_SERVICE_PASS name=media.audio_flinger' in log)
if opts.series and opts.series>=61:
    checks.update(audio_hal_service='A6L_AUDIO_HAL_SERVICE_PASS' in log)
if opts.series and opts.series>=62:
    checks.update(keystore2_service='A6L_KEYSTORE2_SERVICE_PASS' in log or 'A6L_POST_SERVICE_PASS name=android.system.keystore2' in log,gatekeeperd_service='A6L_GATEKEEPERD_SERVICE_PASS' in log)
if opts.erofs:
    checks.update(erofs_delivery='A6L_EROFS_DELIVERY_PASS' in log,boot_completed='name=sys.boot_completed result=0' in log)
if opts.hint_compat:
    # RoleManager is reached only after the HintManager constructor returns.
    checks.update(hint_no_aidl_compat='SystemServerTiming StartRoleManagerService' in log)
report={'runtime_foundation_passed':all(checks.values()),'checks':checks,
        'kernel_sha256':sha(kernel_archive/'Image'),
        'read_barrier_mismatch':'read barrier state mismatch' in log,
        'kernel_profile':kernel_archive.name if opts.runtime_kernel else 'validated-v38',
        'systemserver_entered':'Entered the Android system server!' in log,
        'system_ui_passed':False,'console_sha256':sha(O/'console.log'),
        'raw_console_file':'console.raw.log','console_text_sha256':sha(archive/'console.log'),'command':args}
(archive/'report.json').write_text(json.dumps(report,indent=2)+'\n')
for name in ['framework_services.c','framework_properties.h','FrameworkProbe.java']:shutil.copyfile(R/'device/hisense/a6l/diagnostic'/name,archive/name)
if opts.rooted:
    for name in ['framework_root_services.c','framework_root_properties.h']:
        shutil.copyfile(R/'device/hisense/a6l/diagnostic'/name,archive/name)
if opts.apex_service:
    shutil.copyfile(R/'device/hisense/a6l/diagnostic/framework-apex-v51.sh',archive/'framework-apex-v51.sh')
if opts.native_bootstrap:
    shutil.copyfile(R/'device/hisense/a6l/diagnostic/framework-native-v52.sh',archive/'framework-native-v52.sh')
if opts.health:
    shutil.copyfile(R/'device/hisense/a6l/diagnostic/framework-health-v54.sh',archive/'framework-health-v54.sh')
if opts.bpf:
    shutil.copyfile(R/'device/hisense/a6l/diagnostic/framework-bpf-v55.sh',archive/'framework-bpf-v55.sh')
if opts.series and opts.series>=58:
    shutil.copyfile(R/'device/hisense/a6l/diagnostic/framework-storage-v58.sh',archive/'framework-storage-v58.sh')
if opts.series and opts.series>=59:
    shutil.copyfile(R/'device/hisense/a6l/diagnostic/framework-netd-v59.sh',archive/'framework-netd-v59.sh')
if opts.series and opts.series>=60:
    shutil.copyfile(R/'device/hisense/a6l/diagnostic/framework-media-v60.sh',archive/'framework-media-v60.sh')
if opts.series and opts.series>=62:
    shutil.copyfile(R/'device/hisense/a6l/diagnostic/framework-security-v62.sh',archive/'framework-security-v62.sh')
if opts.hint_compat:
    # copyfile only: copytree's copystat fails with EPERM on the Windows-backed /mnt/c archive
    (archive/'hint-compat-source').mkdir(exist_ok=True)
    for src in sorted((R/'research/framework-hint-v56/adaptation').iterdir()):
        if src.is_file():shutil.copyfile(src,archive/'hint-compat-source'/src.name)
shutil.copyfile(__file__,archive/'Test-FrameworkV48.py')
print(json.dumps(report,indent=2))
print('\n'.join(line for line in log.splitlines() if 'A6L_PRIVATE_PROPERTY' not in line and any(s in line for s in ['A6L_', 'FATAL EXCEPTION', 'FatalError', 'Entered the Android', 'SystemServerTiming ']) and len(line)<400)[-5000:])
