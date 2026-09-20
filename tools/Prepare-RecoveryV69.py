"""Assemble the V69 diagnostic recovery CANDIDATE. Offline only; nothing is flashed.

V69 = installed V68 recovery + two DT overlays (G1 GPU, E1 e-ink PMIC/rear touch, S1 front ALS, M1 modem/Wi-Fi nodes; every new driver is a module loaded only by attended scripts). Kernel and ramdisk unchanged.
Legacy text from the V68 generator follows:
  1. kernel: phone-kernel-v67-candidate (V38 config + Android networking + RMTFS_MEM);
  2. DT: + a6l-eink-flash-read (SPI personality of BLSP2 QUP4 + chosen marker, no MTD driver)
         + research/claude-adsp candidate A (adsp_pil okay + firmware-name + chosen marker);
  3. ramdisk: sdhci-msm.ko replaced by the module built from the same V67 tree.
RAM init, secure ADB keys, command line, bootloader selection overlay and header layout are unchanged.
Every DT property difference is checked against an explicit allowlist.
"""
import gzip,hashlib,importlib.util,io,json,struct,subprocess,sys,tarfile,zlib
from pathlib import Path
from a6l_fdt import read_fdt
ROOT=Path(__file__).resolve().parents[1]
OLD=ROOT/'firmware/extracted/recovery-v68-candidate-20260920'
KERNEL=ROOT/'firmware/extracted/phone-kernel-v67-candidate-20260919'
OUT=ROOT/'firmware/extracted/recovery-v69-candidate-20260921'
PACK=Path('/home/a6l/android/a6l-lineage24/system/tools/mkbootimg')
V46='2448b101eb780b1630cc6fd7181315da50211de2504b08cd6dad9e34d44a7520'  # predecessor = installed V68
V67_IMAGE='0d7d2eb6692b0439a43306ce6f8e26b07334fe21897b8774f99317921fd5acf7'
sha=lambda b:hashlib.sha256(b).hexdigest()
def run(*args):return subprocess.check_output([str(a) for a in args],stderr=subprocess.STDOUT)
def load(name):
    spec=importlib.util.spec_from_file_location(name,ROOT/f'tools/{name}.py');m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m);return m
def unpack(image,out):
    raw=run(sys.executable,PACK/'unpack_bootimg.py','--boot_img',image,'--out',out,'--format','mkbootimg','-0').split(b'\0');assert raw.pop()==b'';return [x.decode() for x in raw]
def replace_in_newc(archive,name,data):
    """Rewrite one regular file inside an uncompressed newc cpio, keeping every other byte."""
    out=io.BytesIO();pos=0;hits=0
    while pos<len(archive):
        hdr=archive[pos:pos+110];assert hdr[:6]==b'070701',pos
        f=[int(hdr[6+8*i:14+8*i],16) for i in range(13)];namesize,filesize=f[11],f[6]
        nstart=pos+110;dstart=(nstart+namesize+3)&~3;end=(dstart+filesize+3)&~3
        entry=archive[nstart:nstart+namesize-1].decode()
        if entry==name:
            hits+=1;f[6]=len(data)
            out.write(b'070701'+b''.join(b'%08X'%x for x in f)+archive[nstart:dstart]+data+bytes((-len(data))%4))
        else:out.write(archive[pos:end])
        pos=end
        if entry=='TRAILER!!!':out.write(archive[pos:]);break
    assert hits==1,(name,hits);return out.getvalue()
def main():
    OUT.mkdir(exist_ok=False)
    original=(OLD/'recovery-diagnostic-unsigned.img').read_bytes();assert sha(original)==V46
    assert json.loads((OLD/'captured-abl-validation.json').read_text())['passed']
    kernel=(KERNEL/'Image').read_bytes();assert sha(kernel)==V67_IMAGE
    assert 'A6L_PHONE_KERNEL_V67_CANDIDATE_BUILD_PASS' in (KERNEL/'build.log').read_text(errors='replace')
    # --- device tree -------------------------------------------------------------------
    base=OUT/'base.dtb';base.write_bytes((OLD/'base.dtb').read_bytes())
    for label,source in [('gpu',ROOT/'device/hisense/a6l/kernel/a6l-gpu.dtso'),('eink-side',ROOT/'device/hisense/a6l/kernel/a6l-eink-pmic-reartouch.dtso'),('front-als',ROOT/'device/hisense/a6l/kernel/a6l-front-als.dtso'),('modem-wifi',ROOT/'device/hisense/a6l/kernel/a6l-modem-wifi.dtso')]:
        overlay=OUT/f'a6l-{label}.dtbo';run('dtc','-@','-I','dts','-O','dtb','-o',overlay,source)
        merged=OUT/f'{label}-merged.dtb';run('fdtoverlay','-i',base,'-o',merged,overlay);base.write_bytes(merged.read_bytes())
    run('fdtput','-t','s',base,'/chosen','hisense,a6l-controls','v69')
    before=read_fdt((OLD/'base.dtb').read_bytes());after=read_fdt(base.read_bytes())
    find=lambda suffix:next(n for n in after if n.endswith(suffix))
    gpu=find('gpu@5000000');zap=gpu+'/zap-shader';i2c2=find('i2c@c176000');i2c7=find('i2c@c1b7000');i2c6=find('i2c@c1b6000');mss=find('remoteproc@4080000');wifi=find('wifi@18800000')
    new_nodes={n for n in after if n not in before}
    expected_new={find('/a6l-tps65185-default-state'),find('/a6l-rear-touch-default-state'),i2c2+'/pmic@68',i2c2+'/pmic@68/regulators',
                  i2c2+'/pmic@68/regulators/vcom',i2c2+'/pmic@68/regulators/vposneg',i2c2+'/pmic@68/regulators/v3p3',
                  i2c7+'/touchscreen@38','/regulator-epd-vin',find('/regulators-0/l3'),find('/a6l-front-als-default-state'),i2c6+'/light-sensor@47',
                  find('/regulators-1/l13'),find('/regulators-1/l5'),find('/regulators-1/l6'),find('/regulators-1/l9'),find('/regulators-1/l19')}
    assert new_nodes==expected_new,(new_nodes^expected_new)
    allowed_props={('/chosen','hisense,a6l-controls'),('/chosen','hisense,a6l-gpu'),('/chosen','hisense,a6l-eink-side'),
                   (gpu,'status'),(zap,'firmware-name'),(i2c2,'status'),(i2c2,'clock-frequency'),(i2c7,'status'),(i2c7,'clock-frequency'),(i2c6,'status'),(i2c6,'clock-frequency'),
                   ('/chosen','hisense,a6l-front-als'),('/chosen','hisense,a6l-modem'),(mss,'status'),(mss,'firmware-name'),(wifi,'status'),
                   (wifi,'vdd-0.8-cx-mx-supply'),(wifi,'vdd-1.8-xo-supply'),(wifi,'vdd-1.3-rfa-supply'),(wifi,'vdd-3.3-ch0-supply')}
    changes={}
    for node in before.keys()|after.keys():
        for prop in before.get(node,{}).keys()|after.get(node,{}).keys():
            a=before.get(node,{}).get(prop);b=after.get(node,{}).get(prop)
            if a==b:continue
            assert node in new_nodes or (node,prop) in allowed_props or (node=='/__symbols__' and a is None) or (prop=='phandle' and a is None),(node,prop)
            changes[node+'/'+prop]={'before':None if a is None else a.hex(),'after':None if b is None else b.hex()}
    assert after[gpu]['status']==b'okay\0' and after[gpu]['compatible'].startswith(b'qcom,adreno-512')
    assert after[zap]['firmware-name']==b'qcom/hisense/a6l/a512_zap.mdt\0'
    mdss=find('display-subsystem@c900000') if any(n.endswith('display-subsystem@c900000') for n in after) else None
    assert mdss is None or after[mdss].get('status',b'disabled\0')==b'disabled\0','MDSS must stay disabled: display remains simpledrm'
    handles={struct.unpack('>I',p['phandle'])[0]:n for n,p in after.items() if 'phandle' in p}
    ref=lambda n,p,i=0:handles[struct.unpack_from('>I',after[n][p],4*i)[0]]
    pm=i2c2+'/pmic@68';tlmm=find('pinctrl@3100000')
    for prop,line in [('enable-gpios',35),('vcom-ctrl-gpios',3),('wakeup-gpios',80),('pwr-good-gpios',0)]:
        assert ref(pm,prop)==tlmm and struct.unpack('>III',after[pm][prop])[1:]==(line,0),prop
    assert ref(pm,'vin-supply')=='/regulator-epd-vin' and struct.unpack('>III',after['/regulator-epd-vin']['gpio'])[1]==2
    als=i2c6+'/light-sensor@47'
    assert struct.unpack('>I',after[als]['reg'])[0]==0x47 and struct.unpack('>III',after[als]['interrupts-extended'])[1:]==(71,2)
    for prop,name,lo,hi in [('vdd-0.8-cx-mx-supply','l5',525000,950000),('vdd-1.8-xo-supply','l9',1750000,1900000),('vdd-1.3-rfa-supply','l6',1200000,1370000),('vdd-3.3-ch0-supply','l19',3200000,3400000)]:
        node=ref(wifi,prop);assert node.endswith('/regulators-1/'+name),(prop,node)
        assert struct.unpack('>I',after[node]['regulator-min-microvolt'])[0]==lo and struct.unpack('>I',after[node]['regulator-max-microvolt'])[0]==hi
    assert after[mss]['firmware-name']==b'qcom/hisense/a6l/mba.mbn\0qcom/hisense/a6l/modem.mdt\0'
    ts=i2c7+'/touchscreen@38'
    assert struct.unpack('>III',after[ts]['interrupts-extended'])[1:]==(73,2) and struct.unpack('>III',after[ts]['reset-gpios'])[1:]==(65,1)
    assert ref(ts,'vcc-supply').endswith('/regulators-0/l3') and ref(ts,'iovcc-supply').endswith('/regulators-1/l11')
    assert after['/soc@0/spmi@800f000/pmic@0/charger@1000']==before['/soc@0/spmi@800f000/pmic@0/charger@1000']
    assert after['/chosen'].get('bootargs')==before['/chosen'].get('bootargs')
    # --- ramdisk: only sdhci-msm.ko changes ----------------------------------------------
    (OUT/'ramdisk.cpio.gz').write_bytes((OLD/'ramdisk.cpio.gz').read_bytes())
    # --- boot image, identical layout rules to V46 ---------------------------------------
    payload=gzip.compress(kernel,mtime=0)+base.read_bytes();(OUT/'Image.gz-dtb').write_bytes(payload)
    for file in ['overlay.dtbo','recovery-dtbo.img']:(OUT/file).write_bytes((OLD/file).read_bytes())
    args=unpack(OLD/'recovery-diagnostic-unsigned.img',OUT/'original-parts');cmdline=args[args.index('--cmdline')+1]
    for key,file in [('kernel','Image.gz-dtb'),('ramdisk','ramdisk.cpio.gz'),('recovery_dtbo','recovery-dtbo.img')]:args[args.index('--'+key)+1]=str(OUT/file)
    bodyfile=OUT/'recovery-diagnostic-body.img';run(sys.executable,PACK/'mkbootimg.py',*args,'--output',bodyfile)
    body=bytearray(bodyfile.read_bytes());body[28:32]=original[28:32];bodyfile.write_bytes(body)
    layout=load('Verify-BootRoundtrip').layout(body)
    assert layout['body_end']==len(body)<len(original)==67108864
    assert layout['sections']['kernel']['sha256']==sha(payload)
    h1,h2=bytearray(original[:4096]),bytearray(body[:4096])
    for a,b in [(8,12),(576,608),(1636,1644)]:h1[a:b]=h2[a:b]=bytes(b-a)   # kernel size, ramdisk size, id, dtbo offset
    assert h1==h2,'Unexpected boot header change'
    again=unpack(bodyfile,OUT/'roundtrip-parts');run(sys.executable,PACK/'mkbootimg.py',*again,'--output',OUT/'roundtrip.img')
    rebuilt=bytearray((OUT/'roundtrip.img').read_bytes());rebuilt[28:32]=body[28:32];assert rebuilt==body
    candidate=bytes(body)+bytes(len(original)-len(body));(OUT/'recovery-diagnostic-unsigned.img').write_bytes(candidate)
    report={'packaging_passed':True,'ready_to_flash':False,'reviewed_by_user':False,'candidate_sha256':sha(candidate),'previous_sha256':V46,
            'kernel_sha256':sha(kernel),'ramdisk_sha256':sha((OUT/'ramdisk.cpio.gz').read_bytes()),
            'base_sha256':sha(base.read_bytes()),'command_line':cmdline,'layout':layout,'dt_changes':changes,
            'scope':'V68 + Adreno 512 GPU node (G1) + TPS65185 e-ink PMIC and rear touch (E1); same kernel and ramdisk. Not booted anywhere. ABL DT validation and QEMU checks are separate steps.'}
    (OUT/'report.json').write_text(json.dumps(report,indent=2)+'\n')
    print('V69_PACKAGE_PASS',sha(candidate),'properties',len(changes),flush=True)
if __name__=='__main__':main()
