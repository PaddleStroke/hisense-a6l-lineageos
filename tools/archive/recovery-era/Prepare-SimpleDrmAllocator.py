"""Add simpleDRM to minigbm's existing dumb-buffer backend; preserve provenance."""
from pathlib import Path
import hashlib,json,subprocess
ROOT=Path(__file__).resolve().parents[1]
repo=Path('/home/a6l/android/a6l-lineage24/external/minigbm')
out=ROOT/'research/android-allocator-v41-20260917'
out.mkdir(exist_ok=False)
assert not subprocess.check_output(['git','-C',str(repo),'status','--porcelain'],text=True).strip()
head=subprocess.check_output(['git','-C',str(repo),'rev-parse','HEAD'],text=True).strip()
changes={'dumb_driver.c':[('INIT_DUMB_DRIVER(vkms)','INIT_DUMB_DRIVER(vkms)\nINIT_DUMB_DRIVER(simpledrm)')],
         'drv.c':[('extern const struct backend backend_vkms;','extern const struct backend backend_vkms;\nextern const struct backend backend_simpledrm;'),
                  ('&backend_vkms,\t    &backend_mock','&backend_vkms,\t    &backend_mock, &backend_simpledrm')]}
report={'head':head,'files':{}}
for name,replacements in changes.items():
    p=repo/name;before=p.read_bytes();s=before.decode()
    for old,new in replacements:assert s.count(old)==1,(name,old);s=s.replace(old,new)
    after=s.encode();(out/(name+'.before')).write_bytes(before);(out/name).write_bytes(after)
    p.write_bytes(after)
    report['files'][name]={'before_sha256':hashlib.sha256(before).hexdigest(),'after_sha256':hashlib.sha256(after).hexdigest()}
(out/'simpledrm.patch').write_bytes(subprocess.check_output(['git','-C',str(repo),'diff','--','dumb_driver.c','drv.c']))
(out/'report.json').write_text(json.dumps(report,indent=2)+'\n')
print(json.dumps(report,indent=2))
