"""Turn a PASSING --erofs VM run into the RAM bundle for the attended phone run. Offline; run inside WSL.
usage: Prepare-FrameworkPhoneBundleV71.py <series> <attempt>
The bundle is exactly what the VM booted (same payload.erofs bytes, same overlay.ko) plus the phone launcher.
"""
import hashlib,json,shutil,sys,glob
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
series,attempt=sys.argv[1],sys.argv[2]
work=Path(f'/home/a6l/kernel/framework-v{series}-r{attempt}')
arch=Path(sorted(glob.glob(str(ROOT/f'firmware/extracted/android-framework-v{series}-*-r{attempt}')))[-1])
report=json.loads((arch/'report.json').read_text())
failed=[k for k,v in report['checks'].items() if not v and k!='cleanup']
assert not failed and report['checks'].get('erofs_delivery') and report['checks'].get('boot_completed'),failed
assert report['kernel_profile'].startswith('phone-kernel-v67-candidate'),report['kernel_profile']
OUT=ROOT/f'firmware/extracted/framework-phone-bundle-v71-from-v{series}r{attempt}';OUT.mkdir(exist_ok=False)
def sha(p):
    h=hashlib.sha256()
    with open(p,'rb') as f:
        for b in iter(lambda:f.read(1<<22),b''):h.update(b)
    return h.hexdigest()
for name in ['payload.erofs','overlay.ko']:shutil.copyfile(work/name,OUT/name)
shutil.copyfile(ROOT/'device/hisense/a6l/diagnostic/framework-phone-v71.sh',OUT/'framework-phone-v71.sh')
names=['payload.erofs','overlay.ko','framework-phone-v71.sh']
(OUT/'SHA256SUMS').write_text(''.join(f'{sha(OUT/n)}  {n}\n' for n in names))
(OUT/'manifest.json').write_text(json.dumps({'from_vm_run':arch.name,'vm_checks':report['checks'],'kernel_sha256':report['kernel_sha256'],
    'requires_recovery':'recovery-v68..v71 candidate','files':{n:{'sha256':sha(OUT/n),'bytes':(OUT/n).stat().st_size} for n in names},
    'phone_access':False,'note':'payload contains VM-tuned properties (ro.hardware.virtual_device=1, software rendering); first phone run is an observation run.'},indent=2)+'\n')
print('V71_PHONE_BUNDLE_PASS',OUT.name,sum((OUT/n).stat().st_size for n in names))
