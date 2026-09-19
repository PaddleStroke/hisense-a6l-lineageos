"""Create a bounded controls follow-up runner; never execute it here."""
from pathlib import Path
import py_compile
T=Path(__file__).resolve().parent
def put(name,s):
    p=T/name;assert not p.exists(),p;p.write_text(s);py_compile.compile(str(p),doraise=True)
s=(T/'Create-V45InputNodes.py').read_text().replace('V45','V46').replace('v45','v46')
put('Create-V46InputNodes.py',s)
s=(T/'Run-ControlsV45.py').read_text().replace('V45','V46').replace('v45','v46')
s=s.replace("choices=['baseline','surface','touch-load','touch-events','keys','brightness','vibration','battery']", "choices=['baseline','keys','vibration']")
s=s.replace("args.stage in ['brightness','vibration']", "args.stage == 'vibration'")
start=s.index("        elif args.stage=='surface':")
end=s.index("        elif args.stage=='keys':",start)
s=s[:start]+s[end:]
start=s.index("        elif args.stage=='brightness':")
end=s.index("        elif args.stage=='vibration':",start)
s=s[:start]+s[end:]
start=s.index("        elif args.stage=='battery':")
end=s.index("        report['passed']=True",start)
s=s[:start]+s[end:]
# Input nodes are created from known registered devices after identity checks.
anchor="        snapshot('dmesg-before',['/system/bin/toybox','dmesg'])"
s=s.replace(anchor,"        run(['/usr/bin/python3',str(ROOT/'Create-V46InputNodes.py')])\n"+anchor)
s=s.replace("            load('qcom-spmi-haptics.ko','qcom_spmi_haptics')", "            assert not re.search(r'^qcom_spmi_haptics ',shell('/system/bin/toybox','cat','/proc/modules')['stdout'],re.M),'Unexpected preloaded haptic module'\n            load('qcom-spmi-haptics.ko','qcom_spmi_haptics')\n            run(['/usr/bin/python3',str(ROOT/'Create-V46InputNodes.py')])")
start=s.index("            assert all(s in caps")
end=s.index("        elif args.stage=='vibration':",start)
s=s[:start]+'''            blocks=re.split(r'(?=add device \\d+:)',caps)
            nodes=[]
            for block in blocks:
                match=re.search(r'add device \\d+: (/dev/input/event\\d+)',block)
                if match and '"A6L side keys"' in block:nodes.append(match[1])
            assert len(nodes)==1,nodes
            snapshot('interrupts-before',['/system/bin/toybox','cat','/proc/interrupts'])
            print('KEY_CAPTURE_ARMED: briefly press only the e-ink side key repeatedly for 20 seconds.',flush=True)
            r=run(['adb','-s',SERIAL,'shell','-tt','/system/bin/toybox','timeout','20','/system/bin/toolbox','getevent','-t',nodes[0]],timeout=30,required=False)
            assert r['exit'] in [0,124,143]
            (output/'events.txt').write_text(r['stdout'])
            downs=len(re.findall(r'\\b0001\\s+0268\\s+00000001\\b',r['stdout']))
            ups=len(re.findall(r'\\b0001\\s+0268\\s+00000000\\b',r['stdout']))
            report.update(key_down_count=downs,key_up_count=ups,physical_key_passed=downs>0 and ups>0)
            snapshot('interrupts-after',['/system/bin/toybox','cat','/proc/interrupts'])
            # An observed zero is a completed diagnostic, not a collector failure.
            # Keep the hardware result separate from successful capture/ADB.
'''+s[end:]
s=s.replace('One explicitly selected, bounded V46 test stage','One explicitly selected, bounded V46 key/haptic stage')
put('Run-ControlsV46.py',s)
print('V46 runner/input-node helper generated')
