"""Generate RAM-only V47 runners from verified V45/V46 physical workflows."""
from pathlib import Path
import py_compile
ROOT=Path(__file__).resolve().parents[1];T=ROOT/'tools'
def put(name,s):
    p=T/name;assert not p.exists();p.write_text(s);py_compile.compile(str(p),doraise=True)
s=(T/'Run-AndroidSurfaceOnV45.py').read_text()
s=s.replace("capture-controls-user-v45/surface","capture-input-user-v47/input").replace("android-surface-v44","android-input-v47").replace('a6l-v44','a6l-v47')
s=s.replace("=='v45'","=='v46'")
s=s.replace("name=='a6l_simplefb.ko'", "name in ['a6l_simplefb.ko','edt-ft5x06.ko']")
s=s.replace("        p=shell('/tmp/a6l-v47/bin/a6l_graphics_services',timeout=115,required=False)", "        shell('/system/bin/toybox','insmod','/tmp/a6l-v47/edt-ft5x06.ko',timeout=15)\n        p=shell('/tmp/a6l-v47/bin/a6l_graphics_services',timeout=250,required=False)")
s=s.replace('A6L_SURFACE_SERVICES_PASS','A6L_INPUT_SERVICES_PASS')
s=s.replace("'A6L_SURFACE_PASS real_surfaceflinger=1 layers=4 transaction=checked' in client", "'A6L_INPUT_PASS ' in client")
s=s.replace("        report['presentation_passed']=", "        motions=re.findall(r'A6L_INPUT_MOTION action=(\\d+) pointers=(\\d+) x=([\\d.]+) y=([\\d.]+)',client)\n        report['motion_count']=len(motions)\n        report['max_pointers']=max((int(m[1]) for m in motions),default=0)\n        report['touch_events_received']=len(motions)>0\n        report['presentation_passed']=")
s=s.replace("report['passed']=report['client_composition']", "report['passed']=report['touch_events_received'] and report['client_composition']")
s=s.replace('real AIDL allocator, mapper5, DRM composer; no persistent mounts or firmware writes','real SurfaceFlinger and EventHub/InputReader/InputDispatcher; no persistent mounts or firmware writes')
put('Run-AndroidInputV47.py',s)
s=(T/'Run-LaptopControlsCapture-v46.py').read_text()
s=s.replace('capture-controls-user-v46','capture-input-user-v47').replace('A6L-v46','A6L-v47')
s=s.replace('V46 requires','V47 requires').replace('Test installed V46 combined controls','Test V47 RAM input on installed V46 recovery')
put('Run-LaptopInputCapture-v47.py',s)
s=(T/'Launch-ControlsV46.py').read_text().replace('capture-controls-user-v46','capture-input-user-v47').replace('controls-v46-launch.log','input-v47-launch.log').replace('A6L-v46-capture','A6L-v47-capture').replace('A6L combined controls test','A6L Android input test').replace('Run-LaptopControlsCapture-v46.py','Run-LaptopInputCapture-v47.py').replace('Verify-ControlsV46Stage.py','Verify-InputV47Stage.py').replace('verified V46 capture','verified V47 RAM capture')
put('Launch-InputV47.py',s)
print('V47 runners generated; no phone actions')
