"""Archive exact stock battery limits, profile and sensor/thermal configuration."""
import hashlib,json,shutil,subprocess
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
OUT=ROOT/'firmware/extracted/power-sensors-prep-20260917'
DT=ROOT/'firmware/extracted/stock-dtbo-20260914/stock-00-merged.dtb'
def get(node,prop,kind):
    return subprocess.check_output(['fdtget','-t',kind,str(DT),node,prop],text=True).strip()
battery='/qcom,battery-data/qcom,4101451_hisense_hlte730t_3800mah_pm660_averaged_masterslave_aug7th2019'
charger='/soc/qcom,spmi@800f000/qcom,pm660@0/qcom,qpnp-smb2'
fg='/soc/qcom,spmi@800f000/qcom,pm660@0/qpnp,fg'
data={}
for name,node,props in [
 ('battery',battery,['qcom,nom-batt-capacity-mah','qcom,max-voltage-uv','qcom,fastchg-current-ma','qcom,batt-id-kohm','qcom,battery-beta']),
 ('charger',charger,['qcom,fcc-max-ua','qcom,fv-max-uv','qcom,usb-icl-ua','qcom,thermal-mitigation','qcom,warm-fcc-ua','qcom,cool-fcc-ua','qcom,warm-fv-comp-uv']),
 ('fuel_gauge',fg,['qcom,fg-jeita-thresholds','qcom,fg-cutoff-voltage','qcom,fg-empty-voltage','qcom,fg-chg-term-current'])]:
    data[name]={'stock_node':node,'properties':{p:[int(v) for v in get(node,p,'u').split()] for p in props}}
profile=bytes(int(v,16) for v in get(battery,'qcom,fg-profile-data','bx').split())
(OUT/'stock-fg-profile.bin').write_bytes(profile)
data['profile']={'bytes':len(profile),'sha256':hashlib.sha256(profile).hexdigest(),'uploaded':False}
data['stock_dtb_sha256']=hashlib.sha256(DT.read_bytes()).hexdigest()
files=[]
for rel in ['etc/thermal-engine.conf','etc/sensors/sensor_def_qcomdev.conf','etc/sensors/hals.conf']:
    src=ROOT/'firmware/extracted/vendor'/rel;dest=OUT/'stock-config'/rel
    dest.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(src,dest)
    files.append({'path':str(dest.relative_to(OUT)),'sha256':hashlib.sha256(dest.read_bytes()).hexdigest()})
data['stock_config_files']=files
data['note']='Stock configuration evidence, not new charging setpoints. Generic sensor registry defaults may differ from persisted calibration; live AK09918 identity takes precedence over AK09911 comments.'
(OUT/'stock-config-manifest.json').write_text(json.dumps(data,indent=2)+'\n')
print('STOCK_POWER_SENSOR_CONFIG_ARCHIVED',len(profile),'profile bytes')
