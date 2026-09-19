"""Select only the modules and tools needed for the attended combined tests."""
import hashlib,json,shutil
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1];OUT=ROOT/'firmware/extracted/controls-v45-prep-20260917';OUT.mkdir(exist_ok=False)
PAY=OUT/'payload';PAY.mkdir()
sources={
 'edt-ft5x06.ko':'firmware/extracted/touchscreen-prep-20260917/edt-ft5x06.ko',
 'qcom-spmi-haptics.ko':'firmware/extracted/controls-radio-prep-20260917/modules/qcom-spmi-haptics.ko',
 'pmi8998_fg.ko':'firmware/extracted/power-sensors-prep-20260917/modules/pmi8998_fg.ko',
 'a6l_haptic_probe':'firmware/extracted/controls-radio-prep-20260917/a6l_haptic_probe',
 'backlight_probe.sh':'device/hisense/a6l/diagnostic/backlight_probe.sh'}
files={}
for name,source in sources.items():
    path=ROOT/source;shutil.copyfile(path,PAY/name)
    files[name]={'sha256':hashlib.sha256(path.read_bytes()).hexdigest(),'bytes':path.stat().st_size,'source':source}
(OUT/'manifest.json').write_text(json.dumps({'files':files,'boot_marker':'/proc/device-tree/chosen/hisense,a6l-controls=v45','stage_only':True},indent=2)+'\n')
modules=[]
for name,module in [('edt-ft5x06.ko','edt_ft5x06'),('qcom-spmi-haptics.ko','qcom_spmi_haptics'),('pmi8998_fg.ko','pmi8998_fg')]:
    (OUT/'modules').mkdir(exist_ok=True);shutil.copyfile(PAY/name,OUT/'modules'/name)
    modules.append({'file':name,'name':module,'sha256':files[name]['sha256'],'depends':[]})
(OUT/'module-manifest.json').write_text(json.dumps({'load_order':modules,'phone_tested':False},indent=2)+'\n')
print('V45_PAYLOAD_PREPARED',len(files),'files; no charger, modem, Wi-Fi or Bluetooth activation modules included')
