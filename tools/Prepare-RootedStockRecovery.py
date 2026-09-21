"""Build a recovery-slot image that boots STOCK Android 9 with Magisk root (for register-level tracing of the stock e-ink
path). Offline only; nothing is flashed. Stock boot/system/vendor/userdata are never modified: the image lives in the
recovery slot we already own, and V71 can be reinstalled afterwards.
  kernel  = stock boot kernel, Magisk legacy-SAR hexpatch (skip_initramfs -> want_initramfs)
  ramdisk = Magisk v30.7 ramdisk (magiskinit, RECOVERYMODE=true: no key held -> boots system as root)
  recovery_dtbo + header layout = stock recovery image (what this ABL already boots from that slot)
usage: Prepare-RootedStockRecovery.py   (expects ~/magisk/work/new-boot.img from boot_patch.sh)"""
import hashlib,json,subprocess,sys,importlib.util
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
PACK=Path('/home/a6l/android/a6l-lineage24/system/tools/mkbootimg')
STOCK=ROOT/'firmware/raw-backup-20260914/partitions'
PREV=ROOT/'firmware/extracted/recovery-v71-candidate-20260921'
OUT=ROOT/'firmware/extracted/recovery-rootedstock-20260921';OUT.mkdir(exist_ok=False)
sha=lambda b:hashlib.sha256(b).hexdigest()
def run(*a):return subprocess.check_output([str(x) for x in a],stderr=subprocess.STDOUT)
def unpack(img,out):
    raw=run(sys.executable,PACK/'unpack_bootimg.py','--boot_img',img,'--out',out,'--format','mkbootimg','-0').split(b'\0');assert raw.pop()==b'';return [x.decode() for x in raw]
def load(name):
    spec=importlib.util.spec_from_file_location(name,ROOT/f'tools/{name}.py');m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m);return m
patched=Path('/home/a6l/magisk/work/new-boot.img')
pargs=unpack(patched,OUT/'magisk-parts');rargs=unpack(STOCK/'recovery.bin',OUT/'stock-recovery-parts')
get=lambda args,k:args[args.index('--'+k)+1]
assert get(pargs,'cmdline')==get(rargs,'cmdline'),'boot and recovery cmdlines differ'
stock_kernel=(STOCK/'boot.bin').read_bytes()
args=list(rargs)
for k in ('kernel','ramdisk'):args[args.index('--'+k)+1]=get(pargs,k)
body=OUT/'body.img';run(sys.executable,PACK/'mkbootimg.py',*args,'--output',body)
b=bytearray(body.read_bytes());orig=(STOCK/'recovery.bin').read_bytes()
layout=load('Verify-BootRoundtrip').layout(bytes(b))
assert layout['body_end']==len(b)<=len(orig)==67108864
header=load('Test-CapturedAblHeader').check(bytes(b[:4096]))
assert header['status']=='0x0' and header['image_size']==layout['body_end'],header
cand=bytes(b)+bytes(len(orig)-len(b));(OUT/'recovery-diagnostic-unsigned.img').write_bytes(cand)
prev=json.loads((PREV/'report.json').read_text())['candidate_sha256']
k=Path(get(pargs,'kernel')).read_bytes()
report={'packaging_passed':True,'ready_to_flash':False,'reviewed_by_user':False,'candidate_sha256':sha(cand),'previous_sha256':prev,
 'kernel_sha256':sha(k),'ramdisk_sha256':sha(Path(get(pargs,'ramdisk')).read_bytes()),'command_line':get(rargs,'cmdline'),'layout':layout,
 'magisk':{'version':'v30.7','apk_sha256':'e0d32d2123532860f97123d927b1bb86c4e08e6fd8a48bfc6b5bee0afae9ebd5','config':'KEEPVERITY=true KEEPFORCEENCRYPT=true RECOVERYMODE=true LEGACYSAR=true'},
 'scope':'Stock Android 9 kernel + Magisk ramdisk in the RECOVERY slot, stock recovery_dtbo and header layout. Stock partitions untouched.'}
(OUT/'report.json').write_text(json.dumps(report,indent=2)+'\n')
(OUT/'captured-abl-validation.json').write_text(json.dumps({'passed':True,'header':header,'scope':'captured ABL header routine only; DT path is the stock one (stock kernel DT + stock recovery_dtbo), not re-emulated'},indent=2)+'\n')
print('ROOTEDSTOCK_PACKAGE_PASS',sha(cand),'body',len(b),flush=True)
