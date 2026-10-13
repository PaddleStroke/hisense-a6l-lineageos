"""Compile isolated controls candidates and reject unrelated DT changes."""
import hashlib,json,subprocess
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
OUT=ROOT/'firmware/extracted/controls-radio-prep-20260917'
BASE=ROOT/'firmware/extracted/recovery-probe-android-ram-v38-20260917/merged-captured-abl.dtb'
def tree(p):
    def read(node):
        props=subprocess.check_output(['fdtget','-p',str(p),node],text=True).splitlines()
        result={node+'/'+prop:subprocess.check_output(['fdtget','-t','bx',str(p),node,prop]).strip().decode() for prop in props}
        for child in subprocess.check_output(['fdtget','-l',str(p),node],text=True).splitlines():result.update(read(node.rstrip('/')+'/'+child))
        return result
    return read('/')
before=tree(BASE)
pm='/soc@0/spmi@800f000/'
profiles={
 'buttons':['/a6l-buttons/',pm+'pmic@0/pon@800/pwrkey/status',pm+'pmic@0/pon@800/resin/status',pm+'pmic@0/pon@800/resin/linux,code'],
 'haptics':[pm+'pmic@1/vibrator@c000/'],
 'backlight':[pm+'pmic@3/leds@d800/'],
}
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
reports={}
for name,allowed in profiles.items():
    overlay=OUT/f'a6l-{name}.dtbo';candidate=OUT/f'{name}-candidate.dtb'
    assert not candidate.exists()
    source=ROOT/f'device/hisense/a6l/kernel/a6l-{name}.dtso'
    subprocess.run(['dtc','-@','-I','dts','-O','dtb','-o',str(overlay),str(source)],check=True)
    subprocess.run(['fdtoverlay','-i',str(BASE),'-o',str(candidate),str(overlay)],check=True)
    after=tree(candidate)
    changes={k:{'before':before.get(k),'after':after.get(k)} for k in before.keys()|after.keys() if before.get(k)!=after.get(k)}
    assert changes and all(any(k.startswith(a) for a in allowed) for k in changes),changes
    assert after[pm+'pmic@0/charger@1000/status']==before[pm+'pmic@0/charger@1000/status']
    reports[name]={'passed':True,'source_sha256':sha(source),'candidate_sha256':sha(candidate),'changes':changes}
    print('CONTROL_TREE_PASS',name,len(changes),flush=True)
(OUT/'control-tree-report.json').write_text(json.dumps({'base_sha256':sha(BASE),'candidates':reports,'phone_tested':False,'flashable':False,'scope':'Isolated DT compilation/application/scope checks; not electrical or physical validation'},indent=2)+'\n')
