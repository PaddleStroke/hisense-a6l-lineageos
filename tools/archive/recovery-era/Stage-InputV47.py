"""Transfer checked V47 RAM payload only; never reboot or launch phone tests."""
import argparse,hashlib,json,subprocess,tarfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1];T=ROOT/'tools'
ap=argparse.ArgumentParser();ap.add_argument('--attempt',type=int,required=True);args=ap.parse_args()
pkg=ROOT/f'firmware/extracted/android-input-v47-20260918-r{args.attempt}'
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
assert json.loads((pkg/'qemu-report.json').read_text())['passed']
files={n:T/n for n in ['Run-AndroidInputV47.py','Run-LaptopInputCapture-v47.py','Launch-InputV47.py','Verify-InputV47Stage.py']}
for name in ['manifest.json','qemu-report.json']:files['android-input-v47/'+name]=pkg/name
manifest=json.loads((pkg/'manifest.json').read_text())['files']
for name,info in manifest.items():
    assert sha(pkg/'payload'/name)==info['sha256']
    files['android-input-v47/payload/'+name]=pkg/'payload'/name
pins={n:sha(p) for n,p in files.items()}
old=json.loads((T/'controls-v46-pins.json').read_text())
for name in ['Wait-AndroidRamReady.py','diagnostic-user-v38-tools.json']:pins[name]=old[name]
pinfile=T/'input-v47-pins.json';assert not pinfile.exists();pinfile.write_text(json.dumps(pins,indent=2)+'\n')
files[pinfile.name]=pinfile
out=ROOT/'research/android-input-v47-20260918';out.mkdir(exist_ok=True)
archive=out/'stage.tar';assert not archive.exists()
with tarfile.open(archive,'w') as tar:
    for name,p in files.items():tar.add(p,arcname=name,recursive=False)
remote='/home/pierrelouis/A6L-usb-20260915'
config=str(T/'a6l-laptop-ssh.conf')
ssh=['C:/Windows/System32/OpenSSH/ssh.exe','-F',config,'a6l-laptop']
scp=['C:/Windows/System32/OpenSSH/scp.exe','-F',config]
def run(argv):
    p=subprocess.run(argv,capture_output=True,text=True,timeout=180)
    assert p.returncode==0,(p.stdout,p.stderr);return p.stdout
unpacker=out/'Unpack-InputV47.py'
unpacker.write_text('''import hashlib,json,tarfile
from pathlib import Path
r=Path(__file__).resolve().parent
assert not (r/'capture-input-user-v47').exists()
p=r/'input-v47-stage.tar'
assert hashlib.sha256(p.read_bytes()).hexdigest()=='''+repr(sha(archive))+'''
expected='''+repr({n:sha(p) for n,p in files.items()})+'''
with tarfile.open(p) as tar:
    members=tar.getmembers();assert len(members)==len(expected) and {m.name for m in members}==set(expected)
    for m in members:
        assert m.isfile() and not Path(m.name).is_absolute() and '..' not in Path(m.name).parts
        data=tar.extractfile(m).read();assert hashlib.sha256(data).hexdigest()==expected[m.name]
        dest=r/m.name;assert not dest.is_symlink()
        assert not dest.exists() or hashlib.sha256(dest.read_bytes()).hexdigest()==expected[m.name]
    for m in members:
        dest=r/m.name;dest.parent.mkdir(parents=True,exist_ok=True)
        if not dest.exists():dest.write_bytes(tar.extractfile(m).read())
print('V47_FILES_STAGED')
''')
run(scp+[str(archive),'a6l-laptop:'+remote+'/input-v47-stage.tar'])
run(scp+[str(unpacker),'a6l-laptop:'+remote+'/Unpack-InputV47.py'])
print(run(ssh+['python3 '+remote+'/Unpack-InputV47.py']))
result=run(ssh+['python3 '+remote+'/Verify-InputV47Stage.py'])
(out/'laptop-verification.json').write_text(result);print(result)
