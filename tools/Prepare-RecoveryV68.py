"""Assemble the V68 diagnostic recovery CANDIDATE. Offline only; nothing is flashed.

V68 = verified V46 recovery with exactly three changes:
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
OLD=ROOT/'firmware/extracted/recovery-controls-v46-20260918'
KERNEL=ROOT/'firmware/extracted/phone-kernel-v67-candidate-20260919'
OUT=ROOT/'firmware/extracted/recovery-v68-candidate-20260920'
PACK=Path('/home/a6l/android/a6l-lineage24/system/tools/mkbootimg')
V46='ce3727dda592065becb883ef3fe663cb3579290edb841854e01f4fc49d02a3c6'
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
    for label,source in [('eink-flash',ROOT/'device/hisense/a6l/kernel/a6l-eink-flash-read.dtso'),('adsp-diag',ROOT/'research/claude-adsp/patches/a6l-adsp-diag.dtso')]:
        overlay=OUT/f'a6l-{label}.dtbo';run('dtc','-@','-I','dts','-O','dtb','-o',overlay,source)
        merged=OUT/f'{label}-merged.dtb';run('fdtoverlay','-i',base,'-o',merged,overlay);base.write_bytes(merged.read_bytes())
    run('fdtput','-t','s',base,'/chosen','hisense,a6l-controls','v68')
    before=read_fdt((OLD/'base.dtb').read_bytes());after=read_fdt(base.read_bytes())
    spi='/soc@0/spi@c1b8000';pins='/soc@0/pinctrl@3100000/a6l-spi8-default-state'
    pinnode=next(n for n in after if n.endswith('/a6l-spi8-default-state'));pins=pinnode
    adsp=next(n for n in after if n.endswith('remoteproc@15700000'))
    allowed_nodes={spi,spi+'/epd-flash@0',pins}
    allowed_props={('/chosen','hisense,a6l-controls'),('/chosen','hisense,a6l-eink-flash'),('/chosen','hisense,a6l-adsp'),
                   (adsp,'status'),(adsp,'firmware-name'),('/__symbols__','a6l_blsp_spi8'),('/__symbols__','a6l_spi8_default')}
    changes={}
    for node in before.keys()|after.keys():
        for prop in before.get(node,{}).keys()|after.get(node,{}).keys():
            a=before.get(node,{}).get(prop);b=after.get(node,{}).get(prop)
            if a==b:continue
            assert node in allowed_nodes or (node,prop) in allowed_props,(node,prop)
            changes[node+'/'+prop]={'before':None if a is None else a.hex(),'after':None if b is None else b.hex()}
    i2c8=next(n for n in after if n.endswith('i2c@c1b8000'))
    assert after[i2c8].get('status',b'disabled\0')==b'disabled\0','i2c8 must stay disabled: it shares the QUP with spi8'
    assert after[spi]['compatible']==b'qcom,spi-qup-v2.2.1\0' and 'dmas' not in after[spi]
    assert struct.unpack('>III',after[spi]['interrupts'])==(0,104,4)
    assert struct.unpack('>I',after[spi+'/epd-flash@0']['spi-max-frequency'])[0]==4000000
    assert after[spi+'/epd-flash@0']['compatible']==b'eink,ed052tc2\0'
    assert after[pins]['pins']==b'gpio28\0gpio29\0gpio30\0gpio31\0' and after[pins]['function']==b'blsp_spi8_a\0'
    handles={struct.unpack('>I',p['phandle'])[0]:n for n,p in after.items() if 'phandle' in p}
    assert handles[struct.unpack('>I',after[spi]['pinctrl-0'])[0]]==pins
    gcc=handles[struct.unpack('>II',after[spi]['clocks'][:8])[0]];assert gcc.endswith('clock-controller@100000'),gcc
    assert struct.unpack('>IIII',after[spi]['clocks'])[1::2]==(44,36)
    assert after[adsp]['status']==b'okay\0' and after[adsp]['firmware-name']==b'qcom/hisense/a6l/adsp.mdt\0'
    assert after['/soc@0/spmi@800f000/pmic@0/charger@1000']==before['/soc@0/spmi@800f000/pmic@0/charger@1000']
    assert after['/chosen'].get('bootargs')==before['/chosen'].get('bootargs')
    # --- ramdisk: only sdhci-msm.ko changes ----------------------------------------------
    with tarfile.open(KERNEL/'modules.tar.gz') as t:
        member=next(m for m in t.getmembers() if m.name.endswith('/sdhci-msm.ko'));module=t.extractfile(member).read()
    sums=(KERNEL/'modules-SHA256SUMS').read_text();assert sha(module) in sums
    old_rd=gzip.decompress((OLD/'ramdisk.cpio.gz').read_bytes());new_rd=replace_in_newc(old_rd,'sdhci-msm.ko',module)
    (OUT/'ramdisk.cpio.gz').write_bytes(gzip.compress(new_rd,mtime=0));(OUT/'sdhci-msm.ko').write_bytes(module)
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
    for a,b in [(8,12),(16,20),(576,608),(1636,1644)]:h1[a:b]=h2[a:b]=bytes(b-a)   # kernel size, ramdisk size, id, dtbo offset
    assert h1==h2,'Unexpected boot header change'
    again=unpack(bodyfile,OUT/'roundtrip-parts');run(sys.executable,PACK/'mkbootimg.py',*again,'--output',OUT/'roundtrip.img')
    rebuilt=bytearray((OUT/'roundtrip.img').read_bytes());rebuilt[28:32]=body[28:32];assert rebuilt==body
    candidate=bytes(body)+bytes(len(original)-len(body));(OUT/'recovery-diagnostic-unsigned.img').write_bytes(candidate)
    report={'packaging_passed':True,'ready_to_flash':False,'reviewed_by_user':False,'candidate_sha256':sha(candidate),'previous_sha256':V46,
            'kernel_sha256':sha(kernel),'ramdisk_sha256':sha((OUT/'ramdisk.cpio.gz').read_bytes()),'sdhci_msm_sha256':sha(module),
            'base_sha256':sha(base.read_bytes()),'command_line':cmdline,'layout':layout,'dt_changes':changes,
            'scope':'V46 + V67 kernel + e-ink SPI read path + ADSP candidate A. Not booted anywhere. ABL DT validation and QEMU checks are separate steps.'}
    (OUT/'report.json').write_text(json.dumps(report,indent=2)+'\n')
    print('V68_PACKAGE_PASS',sha(candidate),'properties',len(changes),flush=True)
if __name__=='__main__':main()
