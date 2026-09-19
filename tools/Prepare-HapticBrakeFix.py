"""Build an isolated module fixing cell/byte brake-pattern parsing. No phone access."""
import difflib,hashlib,json,os,shutil,subprocess
from pathlib import Path
from a6l_fdt import read_fdt,cells
ROOT=Path(__file__).resolve().parents[1]
K=Path('/home/a6l/kernel/a6l-baseline-7.2');BASE=Path('/home/a6l/kernel/out-a6l-android-init')
BUILD=Path('/home/a6l/kernel/out-a6l-peripheral-prep-20260917')
M=Path('/home/a6l/kernel/a6l-haptics-brake-20260918');M.mkdir(exist_ok=False)
OUT=ROOT/'firmware/extracted/haptics-brake-prep-20260918';OUT.mkdir(exist_ok=False)
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
source=K/'drivers/input/misc/qcom-spmi-haptics.c'
protected=[source,BASE/'.config',BASE/'arch/arm64/boot/Image',BASE/'Module.symvers']
before={str(p):sha(p) for p in protected}
s=source.read_text();old='\tret = of_property_read_u8_array(node, "qcom,brake-pattern", haptics->brake_pat, 4);'
assert s.count(old)==1
new='''	/* The binding uses four u32 cells; old trees may use four bytes. */
	if (of_property_count_u32_elems(node, "qcom,brake-pattern") == 4) {
		u32 brake[4];

		ret = of_property_read_u32_array(node, "qcom,brake-pattern", brake, 4);
		if (!ret) {
			for (i = 0; i < 4; i++) {
				if (brake[i] > 3) {
					ret = -EINVAL;
					goto register_fail;
				}
				haptics->brake_pat[i] = brake[i];
			}
		}
	} else {
		ret = of_property_read_u8_array(node, "qcom,brake-pattern", haptics->brake_pat, 4);
	}'''
modified=s.replace(old,new);(M/source.name).write_text(modified)
(M/'Makefile').write_text('obj-m += qcom-spmi-haptics.o\n')
(OUT/'original.c').write_text(s);(OUT/'candidate.c').write_text(modified)
(OUT/'brake-pattern.patch').write_text(''.join(difflib.unified_diff(s.splitlines(True),modified.splitlines(True),fromfile='a/drivers/input/misc/qcom-spmi-haptics.c',tofile='b/drivers/input/misc/qcom-spmi-haptics.c')))
dt=read_fdt((ROOT/'firmware/extracted/recovery-controls-v45-20260917/base.dtb').read_bytes())
raw=dt['/soc@0/spmi@800f000/pmic@1/vibrator@c000']['qcom,brake-pattern']
assert len(raw)==16 and cells(raw)==[3,3,0,0] and list(raw[:4])==[0,0,0,3]
env=dict(os.environ);env['PATH']='/home/a6l/android/a6l-lineage24/prebuilts/clang/host/linux-x86/clang-r584948/bin:'+env['PATH']
with (OUT/'build.log').open('w') as log:
    result=subprocess.run(['make','-C',str(K),f'O={BUILD}',f'M={M}','ARCH=arm64','LLVM=1','-j8','modules'],env=env,stdout=log,stderr=subprocess.STDOUT)
assert result.returncode==0,'see build.log'
(OUT/'modules').mkdir();shutil.copyfile(M/'qcom-spmi-haptics.ko',OUT/'modules/qcom-spmi-haptics.ko')
module={'file':'qcom-spmi-haptics.ko','name':'qcom_spmi_haptics','sha256':sha(OUT/'modules/qcom-spmi-haptics.ko'),'depends':[]}
(OUT/'module-manifest.json').write_text(json.dumps({'load_order':[module],'phone_tested':False},indent=2)+'\n')
after={str(p):sha(p) for p in protected};assert before==after
report={'built':True,'stock_intended_brake':[3,3,0,0],'v45_old_driver_reads':list(raw[:4]),'candidate_reads':cells(raw),'protected_unchanged':before==after,'protected':after,'module':module,'physical_failure_causation_proven':False,'phone_tested':False,'amplitude_and_duration_unchanged':True}
(OUT/'report.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report,indent=2))
