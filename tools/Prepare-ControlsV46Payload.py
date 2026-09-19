"""Package only the checked corrected module and unchanged bounded pulse helper."""
import hashlib,json,shutil
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
FIX=ROOT/'firmware/extracted/haptics-brake-prep-20260918-r4'
OLD=ROOT/'firmware/extracted/controls-v45-prep-20260917'
OUT=ROOT/'firmware/extracted/controls-v46-prep-20260918'
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
qemu=json.loads((FIX/'qemu-module-report.json').read_text())
assert qemu['passed'] and all(qemu['checks'].values())
module=json.loads((FIX/'module-manifest.json').read_text())['load_order'][0]
assert module['file']=='qcom-spmi-haptics.ko'
assert sha(FIX/'modules'/module['file'])==module['sha256']
old=json.loads((OLD/'manifest.json').read_text())['files']
helper=OLD/'payload/a6l_haptic_probe'
assert sha(helper)==old['a6l_haptic_probe']['sha256']=='75087e18947f719a62d101c0cd7d65e4a4f99cba692ecc579ec129571a65d53b'
payload=OUT/'payload';payload.mkdir(parents=True,exist_ok=False)
files={}
for name,src in [(module['file'],FIX/'modules'/module['file']),('a6l_haptic_probe',helper)]:
    shutil.copyfile(src,payload/name)
    files[name]={'sha256':sha(payload/name),'bytes':(payload/name).stat().st_size,'source':str(src.relative_to(ROOT))}
(OUT/'manifest.json').write_text(json.dumps({'files':files,'boot_marker':'/proc/device-tree/chosen/hisense,a6l-controls=v46','stage_only':True},indent=2)+'\n')
shutil.copyfile(FIX/'qemu-module-report.json',OUT/'qemu-module-report.json')
print(json.dumps({'prepared':True,'files':files},indent=2))
