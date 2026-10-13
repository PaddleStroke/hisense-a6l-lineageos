"""Create new, isolated V46 offline packaging/check tools; no phone access."""
from pathlib import Path
import py_compile
ROOT=Path(__file__).resolve().parents[1];T=ROOT/'tools'
def put(name,s):
    p=T/name
    assert not p.exists(),p
    p.write_text(s)
    py_compile.compile(str(p),doraise=True)
s=(T/'Prepare-CombinedControlsV45.py').read_text()
s=s.replace('V45','V46').replace('v45','v46')
s=s.replace("OLD=ROOT/'firmware/extracted/recovery-probe-android-ram-v38-20260917'", "OLD=ROOT/'firmware/extracted/recovery-controls-v45-20260917'")
s=s.replace('recovery-controls-v46-20260917','recovery-controls-v46-20260918')
s=s.replace('54b0d7b2502b71d8493e4669f2081e46d9ec4ba7a23c275373d735134977cbbb','aae86f0da6f15a0c1f6e425e88806edb165289c459462b232f0b8773f5e4ac14')
start=s.index('    overlays=');end=s.index('    run(\'fdtput\',\'-t\',\'s\',base,\'/chosen\'')
s=s[:start]+'''    source=ROOT/'device/hisense/a6l/kernel/a6l-eink-key-v46.dtso'
    overlay=OUT/'a6l-eink-key-v46.dtbo'
    run('dtc','-@','-I','dts','-O','dtb','-o',overlay,source)
    merged=OUT/'eink-key-merged.dtb'
    run('fdtoverlay','-i',base,'-o',merged,overlay)
    base.write_bytes(merged.read_bytes())
'''+s[end:]
start=s.index('    allowed=');end=s.index('    changes={}',start)
s=s[:start]+"    allowed=['/soc@0/spmi@800f000/pmic@0/gpio@c000/a6l-eink-key-state']\n"+s[end:]
start=s.index('            assert any(');end=s.index('            changes[',start)
s=s[:start]+'''            assert any(node==n for n in allowed) or (node=='/a6l-buttons' and prop in ['pinctrl-names','pinctrl-0']) or (node=='/chosen' and prop=='hisense,a6l-controls') or (node=='/__symbols__' and prop=='a6l_eink_key'),(node,prop)
'''+s[end:]
start=s.index('    for key,node in');end=s.index('    payload=',start)
s=s[:start]+'''    key='/soc@0/spmi@800f000/pmic@0/gpio@c000/a6l-eink-key-state'
    assert ref('/a6l-buttons','pinctrl-0')==key
    assert after[key]['pins']==b'gpio11\\0' and after[key]['function']==b'normal\\0'
    assert after[key]['input-enable']==b''
    for prop,value in [('power-source',0),('qcom,pull-up-strength',0),('qcom,drive-strength',3)]:
        assert struct.unpack('>I',after[key][prop])[0]==value
    assert after['/a6l-buttons/eink-key']==before['/a6l-buttons/eink-key']
'''+s[end:]
s=s.replace('DT-only controls/touch/fuel-gauge additions. Buttons/WLED built-in probe at boot; touch/haptics/FG loaded only by attended test runner. Charger/radios/ADSP unchanged.','V45 plus explicit stock PM660 GPIO11 pinctrl only. Corrected haptic module separately staged in RAM; unchanged pulse bounds.')
s=s.replace('from exactly validated V38 inputs','from exactly validated V45 inputs')
put('Prepare-CombinedControlsV46.py',s)
s=(T/'Test-RecoveryControlsV45.py').read_text().replace('V45','V46').replace('v45','v46').replace('recovery-controls-v46-20260917','recovery-controls-v46-20260918')
s=s.replace('recovery-probe-android-ram-v38-20260917/base.dtb','recovery-controls-v45-20260917/base.dtb')
put('Test-RecoveryControlsV46.py',s)
print('V46 packaging/check scripts generated')
