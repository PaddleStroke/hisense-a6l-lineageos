"""Build an isolated exact-length haptic brake-pattern parser candidate."""
import difflib,gzip,hashlib,json,os,shutil,subprocess
from pathlib import Path
from a6l_fdt import read_fdt,cells

ROOT=Path(__file__).resolve().parents[1]
K=Path('/home/a6l/kernel/a6l-baseline-7.2');BASE=Path('/home/a6l/kernel/out-a6l-android-init')
BUILD=Path('/home/a6l/kernel/out-a6l-peripheral-prep-20260917-r2');BUILD.mkdir(exist_ok=True)
M=Path('/home/a6l/kernel/a6l-haptics-brake-20260918-r2');M.mkdir(exist_ok=True)
OUT=ROOT/'firmware/extracted/haptics-brake-prep-20260918-r2';OUT.mkdir(exist_ok=True)
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
source=K/'drivers/input/misc/qcom-spmi-haptics.c'
protected=[source,BASE/'.config',BASE/'arch/arm64/boot/Image',BASE/'Module.symvers']
before={str(p):sha(p) for p in protected}
s=source.read_text();old='''\thaptics->brake_pat[3] = 0x1;\n\n\tret = of_property_read_u8_array(node, "qcom,brake-pattern", haptics->brake_pat, 4);\n\tif (ret < 0 && ret != -EINVAL) {\n\t\tdev_err(&pdev->dev, "qcom,brake-pattern is invalid, ret = %d\\n", ret);\n\t\tgoto register_fail;\n\t}'''
assert s.count(old)==1
new='''\thaptics->brake_pat[3] = 0x1;\n\n\t/* Accept exactly four u32 cells or the four-byte legacy form. */\n\t{\n\t\tconst void *brake_prop;\n\t\tint brake_len;\n\n\t\tbrake_prop = of_get_property(node, "qcom,brake-pattern", &brake_len);\n\t\tif (!brake_prop) {\n\t\t\t/* Preserve the original absent-property defaults. */\n\t\t\tret = -EINVAL;\n\t\t} else if (brake_len == 16) {\n\t\t\tu32 brake[4];\n\n\t\t\tret = of_property_read_u32_array(node, "qcom,brake-pattern", brake, 4);\n\t\t\tif (!ret) {\n\t\t\t\tfor (i = 0; i < 4; i++) {\n\t\t\t\t\tif (brake[i] > 3) {\n\t\t\t\t\t\tret = -EINVAL;\n\t\t\t\t\t\tbreak;\n\t\t\t\t\t}\n\t\t\t\t\thaptics->brake_pat[i] = brake[i];\n\t\t\t\t}\n\t\t\t}\n\t\t} else if (brake_len == 4) {\n\t\t\tret = of_property_read_u8_array(node, "qcom,brake-pattern",\n\t\t\t\t\thaptics->brake_pat, 4);\n\t\t\tif (!ret) {\n\t\t\t\tfor (i = 0; i < 4; i++) {\n\t\t\t\t\tif (haptics->brake_pat[i] > 3) {\n\t\t\t\t\t\tret = -EINVAL;\n\t\t\t\t\t\tbreak;\n\t\t\t\t\t}\n\t\t\t\t}\n\t\t\t}\n\t\t} else {\n\t\t\t/* A present property with any other byte length is malformed. */\n\t\t\tret = -EOVERFLOW;\n\t\t}\n\t}\n\tif (ret < 0 && ret != -EINVAL) {\n\t\tdev_err(&pdev->dev, "qcom,brake-pattern is invalid, ret = %d\\n", ret);\n\t\tgoto register_fail;\n\t}'''
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
(OUT/'modules').mkdir(exist_ok=True);shutil.copyfile(M/'qcom-spmi-haptics.ko',OUT/'modules/qcom-spmi-haptics.ko')
module={'file':'qcom-spmi-haptics.ko','name':'qcom_spmi_haptics','sha256':sha(OUT/'modules/qcom-spmi-haptics.ko'),'depends':[]}
(OUT/'module-manifest.json').write_text(json.dumps({'load_order':[module],'phone_tested':False},indent=2)+'\n')
after={str(p):sha(p) for p in protected};assert before==after
parser_checks={
    'absent_defaults_preserved': 'if (!brake_prop)' in modified and 'ret = -EINVAL;' in modified,
    'exact_u32_length': 'brake_len == 16' in modified,
    'exact_legacy_length': 'brake_len == 4' in modified,
    'malformed_lengths_rejected': 'ret = -EOVERFLOW;' in modified,
    'u32_values_checked': 'brake[i] > 3' in modified,
    'byte_values_checked': 'haptics->brake_pat[i] > 3' in modified,
}
assert all(parser_checks.values())
(OUT/'parser-regression-report.json').write_text(json.dumps({'checks':parser_checks,'method':'compiled candidate module plus source-level checks of the extracted C parser branches'},indent=2)+'\n')
report={'built':True,'stock_intended_brake':[3,3,0,0],'v45_old_driver_reads':list(raw[:4]),'candidate_reads':cells(raw),'protected_unchanged':before==after,'protected':after,'module':module,'physical_failure_causation_proven':False,'phone_tested':False,'amplitude_and_duration_unchanged':True,'parser_regression_checks':parser_checks}
(OUT/'report.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report,indent=2))

# R2 copy of the existing diskless module ABI/load-unload harness invocation.
QOUT=Path('/home/a6l/kernel/haptics-brake-module-qemu-20260918-r2');QOUT.mkdir(exist_ok=True)
kernel=ROOT/'firmware/extracted/android-init-kernel-20260917/Image';assert sha(kernel)=='0dc31cdc9c3a6ecec53ae919737480783d104ff8451018d1448573efd140d2d7'
commands=['#!/system/bin/sh','/system/bin/toybox mknod /dev/peripheral-console c 204 64','exec > /dev/peripheral-console 2>&1','echo A6L_PERIPHERAL_ABI_START','/system/bin/toybox insmod /peripheral/qcom-spmi-haptics.ko','echo A6L_LOAD_qcom_spmi_haptics=$?','/system/bin/toybox cat /proc/modules','/system/bin/toybox rmmod qcom_spmi_haptics','echo A6L_UNLOAD_qcom_spmi_haptics=$?','echo A6L_MODULES_AFTER','/system/bin/toybox cat /proc/modules','echo A6L_PERIPHERAL_ABI_DONE']
script=QOUT/'test.sh';script.write_text('\n'.join(commands)+'\n')
rc=QOUT/'init.rc';rc.write_text((ROOT/'device/hisense/a6l/diagnostic/android-init.rc').read_text()+'\n'.join(['on early-init','    start peripheralabitest','service peripheralabitest /system/bin/sh /peripheral/test.sh','    user root','    group root','    disabled','    oneshot','    seclabel u:r:su:s0',''])+'\n')
lines=(ROOT/'firmware/extracted/android-ram-v38-20260917-r2/ramdisk.list').read_text().splitlines();lines=[f'file /system/etc/init/hw/init.rc {rc} 0644 0 0' if x.startswith('file /system/etc/init/hw/init.rc ') else x for x in lines];lines += ['dir /peripheral 0755 0 0',f'file /peripheral/test.sh {script} 0755 0 0',f'file /peripheral/qcom-spmi-haptics.ko {OUT}/modules/qcom-spmi-haptics.ko 0400 0 0']
recipe=QOUT/'ramdisk.list';recipe.write_text('\n'.join(lines)+'\n');(QOUT/'ramdisk.gz').write_bytes(gzip.compress(subprocess.check_output([str(BUILD/'usr/gen_init_cpio'),'-t','1789344000',str(recipe)]),mtime=0))
cmd=['qemu-system-aarch64','-machine','virt,gic-version=3','-cpu','cortex-a53','-smp','2','-m','1024','-nodefaults','-nographic','-monitor','none','-serial','stdio','-nic','none','-no-reboot','-kernel',str(kernel),'-initrd',str(QOUT/'ramdisk.gz'),'-append','console=ttyAMA0,115200 loglevel=1 panic=0 androidboot.selinux=permissive androidboot.init_rc=/system/etc/init.rc']
with (QOUT/'console.log').open('wb') as f:
    p=subprocess.Popen(cmd,stdout=f,stderr=subprocess.STDOUT)
    try:
        import time;deadline=time.monotonic()+60
        while time.monotonic()<deadline:
            data=(QOUT/'console.log').read_bytes()
            if b'A6L_PERIPHERAL_ABI_DONE' in data or b'Kernel panic' in data or p.poll() is not None: break
            time.sleep(.1)
    finally:
        if p.poll() is None: p.kill();p.wait(timeout=5)
data=(QOUT/'console.log').read_text(errors='replace');(OUT/'qemu-module.log').write_text(data);tail=data.split('A6L_MODULES_AFTER')[-1].split('A6L_PERIPHERAL_ABI_DONE')[0]
qchecks={'LOAD_qcom_spmi_haptics':'A6L_LOAD_qcom_spmi_haptics=0' in data.splitlines(),'UNLOAD_qcom_spmi_haptics':'A6L_UNLOAD_qcom_spmi_haptics=0' in data.splitlines(),'no_panic':'Kernel panic' not in data,'finished':'A6L_PERIPHERAL_ABI_DONE' in data,'modules_removed':not any(line.startswith('qcom_spmi_haptics ') for line in tail.splitlines())}
qreport={'passed':all(qchecks.values()),'checks':qchecks,'scope':'Real module ABI/dependency load and unload; no physical hardware emulated','command':cmd};(OUT/'qemu-module-report.json').write_text(json.dumps(qreport,indent=2)+'\n');assert qreport['passed'],qchecks;print('qemu',json.dumps(qreport,indent=2))
