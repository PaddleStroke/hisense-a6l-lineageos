"""Assemble V46 DT-only recovery candidate from exactly validated V45 inputs.

No phone access. Keeps kernel, RAM init, storage permissions, secure ADB and
bootloader selection overlay unchanged. New modules are loaded by later tests.
"""
import gzip,hashlib,importlib.util,json,struct,subprocess,sys,zlib
from pathlib import Path
from a6l_fdt import read_fdt
ROOT=Path(__file__).resolve().parents[1]
OLD=ROOT/'firmware/extracted/recovery-controls-v45-20260917'
OUT=ROOT/'firmware/extracted/recovery-controls-v46-20260918'
PACK=Path('/home/a6l/android/a6l-lineage24/system/tools/mkbootimg')
sha=lambda b:hashlib.sha256(b).hexdigest()
def run(*args):return subprocess.check_output([str(a) for a in args],stderr=subprocess.STDOUT)
def load(name):
    spec=importlib.util.spec_from_file_location(name,ROOT/f'tools/{name}.py');m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m);return m
def unpack(image,out):
    raw=run(sys.executable,PACK/'unpack_bootimg.py','--boot_img',image,'--out',out,'--format','mkbootimg','-0').split(b'\0');assert raw.pop()==b'';return [x.decode() for x in raw]
def main():
    OUT.mkdir(exist_ok=False)
    original=(OLD/'recovery-diagnostic-unsigned.img').read_bytes()
    assert sha(original)=='aae86f0da6f15a0c1f6e425e88806edb165289c459462b232f0b8773f5e4ac14'
    assert json.loads((OLD/'captured-abl-validation.json').read_text())['passed']
    assert json.loads((ROOT/'firmware/extracted/controls-radio-prep-20260917/readiness.json').read_text())['complete_offline']
    base=OUT/'base.dtb';base.write_bytes((OLD/'base.dtb').read_bytes())
    source=ROOT/'device/hisense/a6l/kernel/a6l-eink-key-v46.dtso'
    overlay=OUT/'a6l-eink-key-v46.dtbo'
    run('dtc','-@','-I','dts','-O','dtb','-o',overlay,source)
    merged=OUT/'eink-key-merged.dtb'
    run('fdtoverlay','-i',base,'-o',merged,overlay)
    base.write_bytes(merged.read_bytes())
    run('fdtput','-t','s',base,'/chosen','hisense,a6l-controls','v46')
    before=read_fdt((OLD/'base.dtb').read_bytes());after=read_fdt(base.read_bytes())
    allowed=['/soc@0/spmi@800f000/pmic@0/gpio@c000/a6l-eink-key-state']
    changes={}
    for node in before.keys()|after.keys():
        for prop in before.get(node,{}).keys()|after.get(node,{}).keys():
            a=before.get(node,{}).get(prop);b=after.get(node,{}).get(prop)
            if a==b:continue
            assert any(node==n for n in allowed) or (node=='/a6l-buttons' and prop in ['pinctrl-names','pinctrl-0']) or (node=='/chosen' and prop=='hisense,a6l-controls') or (node=='/__symbols__' and prop=='a6l_eink_key'),(node,prop)
            changes[node+'/'+prop]={'before':None if a is None else a.hex(),'after':None if b is None else b.hex()}
    assert after['/soc@0/spmi@800f000/pmic@0/charger@1000']==before['/soc@0/spmi@800f000/pmic@0/charger@1000']
    assert after['/chosen'].get('bootargs')==before['/chosen'].get('bootargs')
    # Resolve every added phandle to the intended node, including cross-overlay references.
    handles={struct.unpack('>I',p['phandle'])[0]:n for n,p in after.items() if 'phandle' in p}
    ref=lambda n,p:handles[struct.unpack('>I',after[n][p])[0]]
    assert ref('/soc@0/i2c@c178000/touchscreen@38','iovcc-supply')=='/remoteproc/glink-edge/rpm-requests/regulators-1/l11'
    assert ref('/soc@0/spmi@800f000/pmic@0/battery@4000','monitored-battery')=='/battery'
    key='/soc@0/spmi@800f000/pmic@0/gpio@c000/a6l-eink-key-state'
    assert ref('/a6l-buttons','pinctrl-0')==key
    assert after[key]['pins']==b'gpio11\0' and after[key]['function']==b'normal\0'
    assert after[key]['input-enable']==b''
    for prop,value in [('power-source',0),('qcom,pull-up-strength',0),('qcom,drive-strength',3)]:
        assert struct.unpack('>I',after[key][prop])[0]==value
    assert after['/a6l-buttons/eink-key']==before['/a6l-buttons/eink-key']
    payload=(OLD/'Image.gz-dtb').read_bytes();dec=zlib.decompressobj(31);kernel=dec.decompress(payload)
    assert dec.eof and dec.unused_data==(OLD/'base.dtb').read_bytes()
    assert sha(kernel)==json.loads((OLD/'report.json').read_text())['kernel_sha256']
    payload=gzip.compress(kernel,mtime=0)+base.read_bytes();(OUT/'Image.gz-dtb').write_bytes(payload)
    for file in ['ramdisk.cpio.gz','overlay.dtbo','recovery-dtbo.img']:(OUT/file).write_bytes((OLD/file).read_bytes())
    args=unpack(OLD/'recovery-diagnostic-unsigned.img',OUT/'original-parts')
    cmdline=args[args.index('--cmdline')+1]
    for key,file in [('kernel','Image.gz-dtb'),('ramdisk','ramdisk.cpio.gz'),('recovery_dtbo','recovery-dtbo.img')]:args[args.index('--'+key)+1]=str(OUT/file)
    bodyfile=OUT/'recovery-diagnostic-body.img';run(sys.executable,PACK/'mkbootimg.py',*args,'--output',bodyfile)
    body=bytearray(bodyfile.read_bytes());body[28:32]=original[28:32];bodyfile.write_bytes(body)
    layout=load('Verify-BootRoundtrip').layout(body)
    assert layout['body_end']==len(body)<len(original)==67108864
    assert layout['sections']['kernel']['sha256']==sha(payload)
    assert layout['sections']['ramdisk']['sha256']==sha((OLD/'ramdisk.cpio.gz').read_bytes())
    h1,h2=bytearray(original[:4096]),bytearray(body[:4096])
    for a,b in [(8,12),(576,608),(1636,1644)]:h1[a:b]=h2[a:b]=bytes(b-a)
    assert h1==h2,'Unexpected boot header change'
    again=unpack(bodyfile,OUT/'roundtrip-parts');run(sys.executable,PACK/'mkbootimg.py',*again,'--output',OUT/'roundtrip.img')
    rebuilt=bytearray((OUT/'roundtrip.img').read_bytes());rebuilt[28:32]=body[28:32];assert rebuilt==body
    candidate=bytes(body)+bytes(len(original)-len(body));(OUT/'recovery-diagnostic-unsigned.img').write_bytes(candidate)
    report={'packaging_passed':True,'ready_to_flash':False,'candidate_sha256':sha(candidate),'kernel_sha256':sha(kernel),'ramdisk_sha256':sha((OUT/'ramdisk.cpio.gz').read_bytes()),'base_sha256':sha(base.read_bytes()),'command_line':cmdline,'layout':layout,'dt_changes':changes,'kernel_ramdisk_unchanged':True,'physical_tested':False,'remaining':['captured ABL checks','guarded installation tools','attended physical validation'],'scope':'V45 plus explicit stock PM660 GPIO11 pinctrl only. Corrected haptic module separately staged in RAM; unchanged pulse bounds.'}
    (OUT/'report.json').write_text(json.dumps(report,indent=2)+'\n')
    print('V46_PACKAGE_PASS',sha(candidate),'properties',len(changes),flush=True)
if __name__=='__main__':main()
