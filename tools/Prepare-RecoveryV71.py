"""Assemble the V71 diagnostic recovery CANDIDATE. Offline only; nothing is flashed.

V71 = installed V70 with the DT REBUILT from the V68 base and all eight overlays, picking up the 21 Sep hardware fixes:
  - e-ink PMIC: gpio42/gpio56 fixed regulators (always-on), pm660l L3 pinned to 3.0 V (front ALS NAK);
  - sound: dai-link order reversed in the source so the merged DTB probes q6asm-dai before q6routing.
Kernel and ramdisk unchanged. Differences against the V70 DT must be in the allowlisted nodes or be a pure phandle shift.
Legacy text follows:

V70 = installed V68 recovery + two DT overlays (G1 GPU, E1 e-ink PMIC/rear touch, S1 front ALS, M1 modem/Wi-Fi nodes; every new driver is a module loaded only by attended scripts). Kernel and ramdisk unchanged.
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
OLD=ROOT/'firmware/extracted/recovery-v70-candidate-20260921'
V68DIR=ROOT/'firmware/extracted/recovery-v68-candidate-20260920'
KERNEL=ROOT/'firmware/extracted/phone-kernel-v67-candidate-20260919'
OUT=ROOT/'firmware/extracted/recovery-v71-candidate-20260921'
PACK=Path('/home/a6l/android/a6l-lineage24/system/tools/mkbootimg')
V46=json.loads((OLD/'report.json').read_text())['candidate_sha256']  # predecessor = installed V70
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
    base=OUT/'base.dtb';base.write_bytes((V68DIR/'base.dtb').read_bytes())
    K=ROOT/'device/hisense/a6l/kernel'
    for label in ['gpu','eink-pmic-reartouch','front-als','modem-wifi','display-native','eink-dsi','audio-internal','bluetooth']:
        overlay=OUT/f'a6l-{label}.dtbo';run('dtc','-@','-I','dts','-O','dtb','-o',overlay,K/f'a6l-{label}.dtso')
        merged=OUT/f'{label}-merged.dtb';run('fdtoverlay','-i',base,'-o',merged,overlay);base.write_bytes(merged.read_bytes())
    run('fdtput','-t','s',base,'/chosen','hisense,a6l-controls','v71')
    before=read_fdt((OLD/'base.dtb').read_bytes());after=read_fdt(base.read_bytes())
    find=lambda suffix:next(n for n in after if n.endswith(suffix))
    new_nodes={n for n in after if n not in before};assert sorted(new_nodes)==['/regulator-epd-i2c-en','/regulator-epd-pwr-on'],new_nodes
    assert not [n for n in before if n not in after]
    touched=['/regulator-epd-vin',find('/regulators-0/l3')]
    def phandle_shift(a,b):
        if a is None or b is None or len(a)!=len(b) or len(a)%4:return False
        deltas={struct.unpack('>I',b[i:i+4])[0]-struct.unpack('>I',a[i:i+4])[0] for i in range(0,len(a),4)}-{0}
        return deltas<={2}
    changes={};shifts=0
    for node in before.keys()|after.keys():
        for prop in before.get(node,{}).keys()|after.get(node,{}).keys():
            a=before.get(node,{}).get(prop);b=after.get(node,{}).get(prop)
            if a==b:continue
            if node not in new_nodes and node not in touched and phandle_shift(a,b):shifts+=1;changes[node+'/'+prop]={'before':a.hex(),'after':b.hex(),'kind':'phandle-shift'};continue
            ok=node in new_nodes or node in touched or (node=='/chosen' and prop=='hisense,a6l-controls') or (node=='/__symbols__' and a is None)
            assert ok,(node,prop)
            changes[node+'/'+prop]={'before':None if a is None else a.hex(),'after':None if b is None else b.hex()}
    l3=after[find('/regulators-0/l3')];assert struct.unpack('>I',l3['regulator-min-microvolt'])[0]==struct.unpack('>I',l3['regulator-max-microvolt'])[0]==3000000
    for n,g in [('/regulator-epd-pwr-on',42),('/regulator-epd-i2c-en',56)]:
        assert struct.unpack('>III',after[n]['gpio'])[1:]==(g,0) and 'regulator-always-on' in after[n] and 'enable-active-high' in after[n]
    text=run('dtc','-I','dtb','-O','dts',base).decode(errors='replace');snd=text[text.index('\tsound {'):]
    pos=[snd.index(x+' {') for x in ['mm1-dai-link','mm2-dai-link','int0-mi2s-dai-link','int3-mi2s-dai-link']];assert pos==sorted(pos),'dai-link order'
    print('phandle-shift properties',shifts,flush=True)
    lcd=find('dsi@c994000')+'/panel@0';assert after[lcd]['compatible']==b'mdss,ft8719-tianma-1080x2340\0' and struct.unpack('>III',after[lcd]['reset-gpios'])[1:]==(53,1)
    br=find('dsi@c996000')+'/bridge@0';assert after[br]['compatible']==b'toshiba,tc358762\0' and struct.unpack('>III',after[br]['reset-gpios'])[1:]==(12,1)
    t=after['/a6l-epd-panel/panel-timing'];u=lambda k:struct.unpack('>I',t[k])[0]
    assert (u('hactive'),u('vactive'),u('hfront-porch'),u('hback-porch'),u('hsync-len'),u('vfront-porch'),u('vback-porch'),u('vsync-len'))==(384,725,126,125,6,4,4,2)
    assert abs(u('clock-frequency')-(384+126+125+6)*(725+4+4+2)*85)<1000
    assert after[find('/sound')]['compatible']==b'qcom,sdm660-sndcard\0'
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
            'scope':'V70 rebuilt with 21 Sep fixes (gpio42/56 regulators, L3B 3.0 V, sound link order). Legacy: V69 + native display D1 (MDSS/DSI0/FT8719) + e-ink DSI1/TC358762/DPI panel E2 + internal audio A1; same kernel and ramdisk. Not booted anywhere. ABL DT validation and QEMU checks are separate steps.'}
    (OUT/'report.json').write_text(json.dumps(report,indent=2)+'\n')
    print('V71_PACKAGE_PASS',sha(candidate),'properties',len(changes),flush=True)
if __name__=='__main__':main()
