"""Build configured modules in an isolated copy; never change validated V38 outputs."""
import hashlib,json,os,shutil,subprocess,time
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
K=Path('/home/a6l/kernel/a6l-baseline-7.2')
BASE=Path('/home/a6l/kernel/out-a6l-android-init')
OUT=Path('/home/a6l/kernel/out-a6l-peripheral-prep-20260917')
ARCH=ROOT/'firmware/extracted/peripheral-prep-20260917'
ARCH.mkdir(exist_ok=True)
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
protected=[BASE/'.config',BASE/'arch/arm64/boot/Image',BASE/'Module.symvers']
before={str(p):sha(p) for p in protected}
assert before[str(BASE/'arch/arm64/boot/Image')]=='0dc31cdc9c3a6ecec53ae919737480783d104ff8451018d1448573efd140d2d7'
assert not OUT.exists()
subprocess.run(['cp','-a','--reflink=auto',str(BASE),str(OUT)],check=True)
env=dict(os.environ)
env['PATH']='/home/a6l/android/a6l-lineage24/prebuilts/clang/host/linux-x86/clang-r584948/bin:'+env['PATH']
env.update(KBUILD_BUILD_USER='a6l',KBUILD_BUILD_HOST='a6l-build',KBUILD_BUILD_TIMESTAMP='2026-09-14 00:00:00 UTC')
start=time.monotonic()
with (ARCH/'build.log').open('w') as log:
    result=subprocess.run(['make',f'O={OUT}','ARCH=arm64','LLVM=1','-j16','modules'],cwd=K,env=env,stdout=log,stderr=subprocess.STDOUT)
after={str(p):sha(p) for p in protected}
report={'build_exit':result.returncode,'elapsed_seconds':time.monotonic()-start,'validated_output_unchanged':before==after,'protected_hashes':after,'config_unchanged':sha(OUT/'.config')==before[str(BASE/'.config')],'phone_tested':False,'phone_writes':False}
(ARCH/'build-report.json').write_text(json.dumps(report,indent=2)+'\n')
print(json.dumps(report,indent=2),flush=True)
assert report['validated_output_unchanged'] and report['config_unchanged'] and result.returncode==0
