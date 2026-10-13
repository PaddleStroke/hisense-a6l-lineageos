"""Adapt merged LineageOS change 434585 to the pinned framework; no HAL stub."""
from pathlib import Path
import hashlib, json, subprocess

R=Path(__file__).resolve().parents[1]
A=Path('/home/a6l/android/a6l-lineage24')
repo=A/'frameworks/base'
rel='services/core/java/com/android/server/power/hint/HintManagerService.java'
p=repo/rel
out=R/'research/framework-hint-v56/adaptation'
out.mkdir(exist_ok=False)
subprocess.run(['git','-C',str(repo),'diff','--exit-code','HEAD','--',rel],check=True)
old=p.read_text()
anchor='''        }
        if (mSupportInfo.headroom.isCpuSupported) {'''
assert old.count(anchor)==1
new=old.replace(anchor,'''        } else {
            mSupportInfo = getDummySupportInfo();
        }
        if (mSupportInfo.headroom.isCpuSupported) {''',1)
anchor='''        SupportInfo supportInfo = new SupportInfo();
        supportInfo.usesSessions = isHintSessionSupported();'''
assert new.count(anchor)==1
new=new.replace(anchor,'''        return getDummySupportInfo();
    }

    private SupportInfo getDummySupportInfo() {
        SupportInfo supportInfo = new SupportInfo();
        supportInfo.usesSessions = isHintSessionSupported();''',1)
(out/'HintManagerService.before.java').write_text(old)
(out/'HintManagerService.java').write_text(new)
p.write_text(new)
patch=subprocess.check_output(['git','-C',str(repo),'diff','--',rel])
(out/'hintmanager-no-aidl.patch').write_bytes(patch)
(out/'provenance.json').write_text(json.dumps({
    'framework_head':subprocess.check_output(['git','-C',str(repo),'rev-parse','HEAD'],text=True).strip(),
    'upstream_change':'https://review.lineageos.org/c/LineageOS/android_frameworks_base/+/434585',
    'upstream_commit':'5518dd64224c3dfe2674a496c771770904458a78',
    'before_sha256':hashlib.sha256(old.encode()).hexdigest(),
    'after_sha256':hashlib.sha256(new.encode()).hexdigest()},indent=2)+'\n')
print(patch.decode())
