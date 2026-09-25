"""Assemble the V75-usb diagnostic recovery CANDIDATE (agent usbfix, 24/25 Sep 2026). Offline only; nothing is flashed.

V75-usb = V74 with ONE change: the RAM ramdisk gets a USB re-attach watchdog service.
  - new  /system/bin/a6l_usb_watchdog.sh (device/hisense/a6l/diagnostic/a6l_usb_watchdog.sh), mode 0755;
  - /system/etc/init/hw/init.rc = device/hisense/a6l/diagnostic/android-init-v75usb.rc
    (= android-init.rc + `start a6lusbwd` + the a6lusbwd service block).
Kernel Image, DT (base.dtb, byte-identical to V74), command line, header layout, recovery DTBO and ABL overlay are
V74's. Every other ramdisk entry is byte-identical to V74's (checked). The DT markers stay v74/v71 on purpose (every
bundle run.sh keeps working); the ramdisk identifies itself on kmsg with "A6L_USBWD START v75-usb".
"""
import gzip,hashlib,importlib.util,json,subprocess,sys
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
OLD=ROOT/'firmware/extracted/recovery-v74-candidate-20260923'
OUT=ROOT/'firmware/extracted/recovery-v75usb-candidate-20260925'
PACK=Path('/home/a6l/android/a6l-lineage24/system/tools/mkbootimg')
DIAG=ROOT/'device/hisense/a6l/diagnostic'
V74='24ede49b0f34b10f216617cb007c757e495fa2df1b6cd1ddd0afd63f9819da62'
sha=lambda b:hashlib.sha256(b).hexdigest()
def run(*args):return subprocess.check_output([str(a) for a in args],stderr=subprocess.STDOUT)
def load(name):
    spec=importlib.util.spec_from_file_location(name,ROOT/f'tools/{name}.py');m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m);return m
def unpack(image,out):
    raw=run(sys.executable,PACK/'unpack_bootimg.py','--boot_img',image,'--out',out,'--format','mkbootimg','-0').split(b'\0');assert raw.pop()==b'';return [x.decode() for x in raw]
def parse_newc(archive):
    """-> list of (name, fields[13], data, raw_entry_bytes); stops after TRAILER!!!"""
    pos=0;entries=[]
    while pos<len(archive):
        hdr=archive[pos:pos+110];assert hdr[:6]==b'070701',pos
        f=[int(hdr[6+8*i:14+8*i],16) for i in range(13)];namesize,filesize=f[11],f[6]
        nstart=pos+110;dstart=(nstart+namesize+3)&~3;end=(dstart+filesize+3)&~3
        name=archive[nstart:nstart+namesize-1].decode()
        entries.append((name,f,archive[dstart:dstart+filesize],archive[pos:end]))
        pos=end
        if name=='TRAILER!!!':break
    return entries,archive[pos:]
def newc_entry(name,f,data):
    nb=name.encode()+b'\0';f=list(f);f[6]=len(data);f[11]=len(nb)
    head=b'070701'+b''.join(b'%08X'%x for x in f)+nb
    return head+bytes((-len(head))%4)+data+bytes((-len(data))%4)
def text(path):  # repo files may be CRLF on Windows: the ramdisk gets LF
    return path.read_bytes().replace(b'\r\n',b'\n')
def main():
    OUT.mkdir(exist_ok=False)
    original=(OLD/'recovery-diagnostic-unsigned.img').read_bytes();assert sha(original)==V74
    assert json.loads((OLD/'captured-abl-validation.json').read_text())['passed']
    old=json.loads((OLD/'report.json').read_text())
    # --- ramdisk ---------------------------------------------------------------------------
    rd_old=gzip.decompress((OLD/'ramdisk.cpio.gz').read_bytes())
    entries,tail=parse_newc(rd_old)
    names=[e[0] for e in entries]
    rc_old=text(DIAG/'android-init.rc');rc_new=text(DIAG/'android-init-v75usb.rc');wd=text(DIAG/'a6l_usb_watchdog.sh')
    rc_entry=entries[names.index('system/etc/init/hw/init.rc')]
    assert rc_entry[2]==rc_old,'V74 init.rc differs from the repo copy'
    assert 'system/bin/a6l_usb_watchdog.sh' not in names
    # the new rc must be the old one plus only added lines
    added=[l for l in rc_new.decode().split('\n') if l not in rc_old.decode().split('\n')]
    ok_added={'    start a6lusbwd','service a6lusbwd /system/bin/sh /system/bin/a6l_usb_watchdog.sh','# V75-usb',
              '    # V75-usb: re-attach watchdog (soft_connect toggle -> UDC rebind -> dwc3 rebind -> glue rebind)'}
    assert set(added)<=ok_added,added   # service body lines (user/group/disabled/oneshot/seclabel) already exist in the old rc
    assert [l for l in rc_old.decode().split('\n') if l not in rc_new.decode().split('\n')]==[],'rc lines removed'
    assert b'start a6lusbwd' in rc_new and b'service a6lusbwd /system/bin/sh /system/bin/a6l_usb_watchdog.sh' in rc_new
    assert wd.startswith(b'#!/system/bin/sh\n') and b'log "START v75-usb' in wd
    sh_entry=entries[names.index('system/bin/sh')]
    maxino=max(e[1][0] for e in entries)
    out=b''
    for name,f,data,raw in entries:
        if name=='system/etc/init/hw/init.rc':out+=newc_entry(name,f,rc_new)
        elif name=='TRAILER!!!':
            nf=list(sh_entry[1]);nf[0]=maxino+1;nf[1]=0o100755;nf[2]=nf[3]=0;nf[4]=1
            out+=newc_entry('system/bin/a6l_usb_watchdog.sh',nf,wd)+raw
        else:out+=raw
    out+=tail
    # self-check: re-parse, every other entry byte-identical
    e2,_=parse_newc(out);m2={e[0]:e for e in e2};m1={e[0]:e for e in entries}
    assert set(m2)==set(m1)|{'system/bin/a6l_usb_watchdog.sh'}
    for n in m1:
        if n!='system/etc/init/hw/init.rc':assert m1[n][3]==m2[n][3],n
    assert m2['system/bin/a6l_usb_watchdog.sh'][2]==wd and m2['system/bin/a6l_usb_watchdog.sh'][1][1]==0o100755
    assert m2['system/etc/init/hw/init.rc'][2]==rc_new
    (OUT/'ramdisk.cpio').write_bytes(out)
    rdgz=gzip.compress(out,compresslevel=9,mtime=0);(OUT/'ramdisk.cpio.gz').write_bytes(rdgz)
    assert gzip.decompress(rdgz)==out
    # --- kernel+DT and the rest: byte-identical to V74 --------------------------------------
    for file in ['Image.gz-dtb','base.dtb','overlay.dtbo','recovery-dtbo.img']:(OUT/file).write_bytes((OLD/file).read_bytes())
    args=unpack(OLD/'recovery-diagnostic-unsigned.img',OUT/'original-parts');cmdline=args[args.index('--cmdline')+1]
    assert (OUT/'original-parts/kernel').read_bytes()==(OLD/'Image.gz-dtb').read_bytes()
    assert (OUT/'original-parts/ramdisk').read_bytes()==(OLD/'ramdisk.cpio.gz').read_bytes()
    for key,file in [('kernel','Image.gz-dtb'),('ramdisk','ramdisk.cpio.gz'),('recovery_dtbo','recovery-dtbo.img')]:args[args.index('--'+key)+1]=str(OUT/file)
    bodyfile=OUT/'recovery-diagnostic-body.img';run(sys.executable,PACK/'mkbootimg.py',*args,'--output',bodyfile)
    body=bytearray(bodyfile.read_bytes());body[28:32]=original[28:32];bodyfile.write_bytes(body)
    layout=load('Verify-BootRoundtrip').layout(body)
    assert layout['body_end']==len(body)<len(original)==67108864
    assert layout['sections']['kernel']['sha256']==old['layout']['sections']['kernel']['sha256']
    assert layout['sections']['ramdisk']['sha256']==sha(rdgz)
    h1,h2=bytearray(original[:4096]),bytearray(body[:4096])
    for a,b in [(8,12),(16,20),(576,608),(1636,1644)]:h1[a:b]=h2[a:b]=bytes(b-a)   # kernel size, ramdisk size, id, dtbo offset
    assert h1==h2,'Unexpected boot header change'
    again=unpack(bodyfile,OUT/'roundtrip-parts');run(sys.executable,PACK/'mkbootimg.py',*again,'--output',OUT/'roundtrip.img')
    rebuilt=bytearray((OUT/'roundtrip.img').read_bytes());rebuilt[28:32]=body[28:32];assert rebuilt==body
    candidate=bytes(body)+bytes(len(original)-len(body));(OUT/'recovery-diagnostic-unsigned.img').write_bytes(candidate)
    report={'packaging_passed':True,'ready_to_flash':False,'reviewed_by_user':False,'candidate_sha256':sha(candidate),'previous_sha256':V74,
            'kernel_sha256':old['kernel_sha256'],'ramdisk_sha256':sha(rdgz),'ramdisk_cpio_sha256':sha(out),
            'watchdog_sha256':sha(wd),'init_rc_sha256':sha(rc_new),
            'base_sha256':old['base_sha256'],'command_line':cmdline,'layout':layout,'dt_changes':old['dt_changes'],
            'scope':'V74 + ramdisk USB re-attach watchdog (a6lusbwd service). Kernel, DT, cmdline, DTBO identical to V74. Not booted anywhere.'}
    assert report['base_sha256']==sha((OUT/'base.dtb').read_bytes())
    (OUT/'report.json').write_text(json.dumps(report,indent=2)+'\n')
    print('V75USB_PACKAGE_PASS',sha(candidate),'ramdisk',sha(rdgz),flush=True)
if __name__=='__main__':main()
