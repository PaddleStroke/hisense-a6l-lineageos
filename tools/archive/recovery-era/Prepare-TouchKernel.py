"""Build stock upstream FT8719 support against the unchanged, validated V38 kernel."""
import hashlib,json,os,subprocess
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
K=Path('/home/a6l/kernel/a6l-baseline-7.2');O=Path('/home/a6l/kernel/out-a6l-android-init')
OUT=ROOT/'firmware/extracted/touchscreen-prep-20260917';OUT.mkdir(exist_ok=False)
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
protected=[O/'.config',O/'arch/arm64/boot/Image',O/'Module.symvers']
before={str(p):sha(p) for p in protected}
assert before[str(O/'arch/arm64/boot/Image')]=='0dc31cdc9c3a6ecec53ae919737480783d104ff8451018d1448573efd140d2d7'
env=dict(os.environ);env['PATH']='/home/a6l/android/a6l-lineage24/prebuilts/clang/host/linux-x86/clang-r584948/bin:'+env['PATH']
env.update(KBUILD_BUILD_USER='a6l',KBUILD_BUILD_HOST='a6l-build',KBUILD_BUILD_TIMESTAMP='2026-09-14 00:00:00 UTC')
with (OUT/'build.log').open('w') as log:
    subprocess.run(['make',f'O={O}','ARCH=arm64','LLVM=1','-j8','drivers/input/touchscreen/edt-ft5x06.ko'],cwd=K,env=env,stdout=log,stderr=subprocess.STDOUT,check=True)
after={str(p):sha(p) for p in protected};assert before==after
module=OUT/'edt-ft5x06.ko';module.write_bytes((O/'drivers/input/touchscreen/edt-ft5x06.ko').read_bytes())
info=subprocess.check_output(['modinfo',str(module)],text=True);(OUT/'module-info.txt').write_text(info)
assert '7.2.3-a6l-probe+' in info and 'focaltech,ft8719' in info
for name,src in [('edt-ft5x06.c',K/'drivers/input/touchscreen/edt-ft5x06.c'),('edt-ft5x06.yaml',K/'Documentation/devicetree/bindings/input/touchscreen/edt-ft5x06.yaml'),('kernel.config',O/'.config')]:
    (OUT/name).write_bytes(src.read_bytes())
base=ROOT/'firmware/extracted/recovery-probe-android-ram-v38-20260917/base.dtb'
subprocess.run(['dtc','-I','dtb','-O','dts','-o',str(OUT/'v38-base.dts'),str(base)],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,check=True)
(OUT/'report.json').write_text(json.dumps({'passed':True,'kernel_unchanged':before==after,'protected_hashes':after,'module_sha256':sha(module),'stock_driver_unmodified':True,'phone_tested':False,'phone_writes':False},indent=2)+'\n')
print(info);print('A6L_TOUCH_MODULE_BUILD_PASS')
