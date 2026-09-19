"""Select actual transitive modinfo dependencies, preserving load order and hashes."""
import argparse,hashlib,json,subprocess,shutil
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
BUILD=Path('/home/a6l/kernel/out-a6l-peripheral-prep-20260917')
ap=argparse.ArgumentParser();ap.add_argument('--profile',choices=['peripheral','power-sensors','controls-radio'],default='peripheral');args=ap.parse_args()
OUT=ROOT/f'firmware/extracted/{args.profile}-prep-20260917';OUT.mkdir(exist_ok=True)
DEST=OUT/'modules';DEST.mkdir(exist_ok=True)
normalize=lambda s:s.replace('-','_')
files={normalize(p.stem):p for p in BUILD.rglob('*.ko')}
groups={'wifi':['ath10k_snoc','qrtr_smd'], 'remoteproc':['qcom_q6v5_mss','qcom_q6v5_pas'], 'audio':['apr','q6core','q6afe_dai','q6afe_clocks','q6asm_dai','q6routing','snd_soc_sm8250','snd_soc_msm8916_analog','snd_soc_msm8916_digital']}
if args.profile=='power-sensors':
    groups={'power':['pmi8998_fg','qcom_smbx','qcom_spmi_rradc','qcom_spmi_adc5'], 'sensor_hub':['qrtr_smd','qcom_smgr_accel','qcom_smgr_gyro','qcom_smgr_mag','qcom_smgr_prox']}
elif args.profile=='controls-radio':
    groups={'vibration':['qcom_spmi_haptics'], 'bluetooth':['hci_uart','btqca']}
ordered=[];visiting=set();done={}
def visit(name):
    name=normalize(name)
    if name in done:return
    assert name not in visiting,name
    visiting.add(name)
    path=files[name]
    info=subprocess.check_output(['modinfo',str(path)],text=True)
    fields={line.split(':',1)[0]:line.split(':',1)[1].strip() for line in info.splitlines() if ':' in line}
    assert fields['vermagic'].startswith('7.2.3-a6l-probe+ '),name
    deps=[normalize(n) for n in fields.get('depends','').split(',') if n]
    for dep in deps:visit(dep)
    dest=DEST/path.name;shutil.copyfile(path,dest)
    item={'name':name,'file':path.name,'build_path':str(path.relative_to(BUILD)),'sha256':hashlib.sha256(dest.read_bytes()).hexdigest(),'depends':deps,'vermagic':fields['vermagic']}
    done[name]=item;ordered.append(item);visiting.remove(name)
for names in groups.values():
    for name in names:visit(name)
(OUT/'module-manifest.json').write_text(json.dumps({'groups':groups,'load_order':ordered,'phone_tested':False,'kernel':'7.2.3-a6l-probe+'},indent=2)+'\n')
(OUT/'kernel.config').write_bytes((BUILD/'.config').read_bytes())
print('PERIPHERAL_MODULES_PACKAGED',len(ordered))
print(' '.join(x['name'] for x in ordered))
