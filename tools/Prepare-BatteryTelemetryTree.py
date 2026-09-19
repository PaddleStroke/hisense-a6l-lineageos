"""Compile a fuel-gauge-only DT review candidate; no image installation."""
import hashlib,json,subprocess
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
OUT=ROOT/'firmware/extracted/power-sensors-prep-20260917'
base=ROOT/'firmware/extracted/recovery-probe-android-ram-v38-20260917/merged-captured-abl.dtb'
overlay=OUT/'a6l-battery-telemetry.dtbo';candidate=OUT/'candidate-merged.dtb'
assert not candidate.exists()
subprocess.run(['dtc','-@','-I','dts','-O','dtb','-o',str(overlay),str(ROOT/'device/hisense/a6l/kernel/a6l-battery-telemetry.dtso')],check=True)
subprocess.run(['fdtoverlay','-i',str(base),'-o',str(candidate),str(overlay)],check=True)
def tree(p):
    def read(node):
        props=subprocess.check_output(['fdtget','-p',str(p),node],text=True).splitlines()
        result={node+'/'+prop:subprocess.check_output(['fdtget','-t','bx',str(p),node,prop]).strip().decode() for prop in props}
        for child in subprocess.check_output(['fdtget','-l',str(p),node],text=True).splitlines():result.update(read(node.rstrip('/')+'/'+child))
        return result
    return read('/')
before=tree(base);after=tree(candidate)
changes={k:{'before':before.get(k),'after':after.get(k)} for k in before.keys()|after.keys() if before.get(k)!=after.get(k)}
fg='/soc@0/spmi@800f000/pmic@0/battery@4000'
allowed=['/battery/',fg+'/','/__symbols__/a6l_battery']
assert all(any(k.startswith(a) for a in allowed) for k in changes),changes
def get(node,prop,kind='s'):return subprocess.check_output(['fdtget','-t',kind,str(candidate),node,prop],text=True).strip()
assert get(fg,'status')=='okay'
assert get('/soc@0/spmi@800f000/pmic@0/charger@1000','status')=='disabled'
assert get('/battery','charge-full-design-microamp-hours','u')=='3800000'
assert get('/battery','voltage-max-design-microvolt','u')=='4400000'
assert fg+'/power-supplies' not in after
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
report={'passed':True,'base_sha256':sha(base),'candidate_sha256':sha(candidate),'changes':changes,'charger_still_disabled':True,'physical_tested':False,'installed':False,'limitations':['fuel gauge probe performs PMIC control writes, not passive register reads','runtime telemetry/IRQ/temperature/current not validated','does not establish charging safety or complete battery management','final image must apply reviewed changes before bootloader adjustments']}
(OUT/'tree-report.json').write_text(json.dumps(report,indent=2)+'\n')
print('BATTERY_TELEMETRY_TREE_PASS',len(changes),'changed properties; charger remains disabled')
