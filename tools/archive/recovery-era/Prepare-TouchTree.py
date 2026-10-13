"""Compile and inspect an offline touchscreen candidate; never installs it."""
import hashlib,json,subprocess
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
OUT=ROOT/'firmware/extracted/touchscreen-prep-20260917'
base=ROOT/'firmware/extracted/recovery-probe-android-ram-v38-20260917/merged-captured-abl.dtb'
overlay=OUT/'a6l-front-touch.dtbo';candidate=OUT/'candidate-merged.dtb'
assert not candidate.exists()
subprocess.run(['dtc','-@','-I','dts','-O','dtb','-o',str(overlay),str(ROOT/'device/hisense/a6l/kernel/a6l-front-touch.dtso')],check=True)
subprocess.run(['fdtoverlay','-i',str(base),'-o',str(candidate),str(overlay)],check=True)
bus='/soc@0/i2c@c178000';touch=bus+'/touchscreen@38'
# The first bring-up uses short PIO transfers; do not introduce BAM DMA yet.
for prop in ['dmas','dma-names']:
    subprocess.run(['fdtput','-d',str(candidate),bus,prop],check=True)
def tree(p):
    def read(node):
        props=subprocess.check_output(['fdtget','-p',str(p),node],text=True).splitlines()
        result={node+'/'+prop:subprocess.check_output(['fdtget','-t','bx',str(p),node,prop]).strip().decode() for prop in props}
        for child in subprocess.check_output(['fdtget','-l',str(p),node],text=True).splitlines():
            result.update(read(node.rstrip('/')+'/'+child))
        return result
    return read('/')
before=tree(base);after=tree(candidate)
changes={k:{'before':before.get(k),'after':after.get(k)} for k in before.keys()|after.keys() if before.get(k)!=after.get(k)}
allowed=[bus+'/', '/soc@0/pinctrl@3100000/a6l-front-touch-irq-state/', '/remoteproc/glink-edge/rpm-requests/regulators-1/l11/', '/__symbols__/a6l_touch_']
assert all(any(k.startswith(a) for a in allowed) for k in changes),changes
def get(node,prop,kind='s'):return subprocess.check_output(['fdtget','-t',kind,str(candidate),node,prop],text=True).strip()
assert get(bus,'status')=='okay'
assert get(touch,'compatible')=='focaltech,ft8719'
assert get(touch,'interrupts','u')=='67 2'
assert get(touch,'touchscreen-size-x','u')=='1080'
assert get(touch,'touchscreen-size-y','u')=='2340'
assert touch+'/reset-gpios' not in after
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
report={'passed':True,'base_sha256':sha(base),'candidate_sha256':sha(candidate),'changes':changes,'physical_tested':False,'installed':False,'limitations':['boot-initialized LCD/touch power required','reset and suspend sequencing unvalidated','final packaging must apply same changes to base/board overlay before ABL adjustments']}
(OUT/'tree-report.json').write_text(json.dumps(report,indent=2)+'\n')
subprocess.run(['dtc','-I','dtb','-O','dts','-o',str(OUT/'candidate-merged.dts'),str(candidate)],check=True,stderr=subprocess.DEVNULL)
print(json.dumps({k:v for k,v in report.items() if k!='changes'},indent=2));print('changed properties:',len(changes))
