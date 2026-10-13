"""Check final offline reports and hashes; record a precise resume state."""
import hashlib,json
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
OUT=ROOT/'firmware/extracted/controls-radio-prep-20260917'
read=lambda n:json.loads((OUT/n).read_text())
modules=read('module-manifest.json')
for m in modules['load_order']:
    assert hashlib.sha256((OUT/'modules'/m['file']).read_bytes()).hexdigest()==m['sha256']
for item in read('stock-assets-manifest.json')['assets']:
    assert hashlib.sha256((OUT/item['file']).read_bytes()).hexdigest()==item['sha256']
for item in read('bluetooth-firmware-report.json')['files']:
    assert hashlib.sha256((OUT/item['upstream_file']).read_bytes()).hexdigest()==item['sha256']
module_test=read('qemu-module-report.json');probe_test=read('qemu-controls-report.json');trees=read('control-tree-report.json')
assert module_test['passed'] and probe_test['passed']
for name,result in trees['candidates'].items():
    assert result['passed']
    assert hashlib.sha256((OUT/f'{name}-candidate.dtb').read_bytes()).hexdigest()==result['candidate_sha256']
    assert hashlib.sha256((ROOT/f'device/hisense/a6l/kernel/a6l-{name}.dtso').read_bytes()).hexdigest()==result['source_sha256']
kernel_checks={
 'arch/arm64/boot/Image':'0dc31cdc9c3a6ecec53ae919737480783d104ff8451018d1448573efd140d2d7',
 '.config':'a025c51c4dd610236fdc6e4fa09e85c30552dcd87dd36523d649b640fcda21e1',
 'Module.symvers':'04ed58631faa4d828c242186c1546e22ff8c3c706a8e0e6513e303d69b81a1b6'}
for file,sha in kernel_checks.items():
    assert hashlib.sha256((Path('/home/a6l/kernel/out-a6l-android-init')/file).read_bytes()).hexdigest()==sha
summary={'complete_offline':True,'phone_tested':False,'phone_flashed':False,'kernel_unchanged':kernel_checks,
 'module_count':len(modules['load_order']),'qemu_module_checks':len(module_test['checks']),'qemu_probe_checks':len(probe_test['checks']),
 'dt_candidates':list(trees['candidates']),'bluetooth_firmware_files':6,
 'not_ready_for_activation':['Bluetooth until VDD_IO supply mapped','GNSS until modem/LOC service','audio until ADSP/codec/DAI integration'],
 'next':'User returns: V44 display test, then package reviewed control/touch DT candidates for short attended tests. No controls payload staged to laptop/phone.'}
(OUT/'readiness.json').write_text(json.dumps(summary,indent=2)+'\n')
resume=ROOT/'docs/resume-next-session.md'
line='CONTROLS/RADIO PREP COMPLETE 2026-09-17 user away. No phone mutations/reboots/activations. 11haptic+BT modules QEMU25/25PASS; separate buttons/haptics/backlight DT candidates scope15/2/9PASS (notflashable). Static AArch64 haptic helper built4m33; 100ms8192FF -> 1276mV vsstock3200, arbitrarystrength disallowed; upstream scaling hazard noted. Brightness4s64/256restore script; QEMUr2 bounds/refusal9/9PASS (nohardware). OwnBTpartition10files extractedverified;6CR TLV/NVMvalid copiedlowercaseqca, ROMvariantunselected. UARTttyHS0=c1af000 confirmed, GPIO16-19; BT VDD_IO unresolved (stock BOBpin1 chip-pwd3.6V cannotmap1.8V), noactivationDTguessed. Audio8XMLvariantsexpanded routes;activeXMLunproven, ADSPsharedprereq; GNSS3ELFdeps+5configs archived,QMI_LOCneedsmodem. Artifacts controls-radio-prep-20260917/readiness.json, docs/controls-radio-preparation-20260917.md, research/controls-radio-20260917. ExactV38Image/config/symversunchanged. No payloadstaged; V44pendingphysical. Allsessionsfinished,noagents.\n\n'
assert not resume.read_text().startswith('CONTROLS/RADIO PREP COMPLETE')
resume.write_text(line+resume.read_text())
print('CONTROLS_PREPARATION_VERIFIED',summary)
