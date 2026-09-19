"""Generate the V47 package/emulator harness from validated V44 graphics checks."""
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1];T=ROOT/'tools'
s=(T/'Test-AndroidSurfaceV44.py').read_text().replace('v44','v47').replace('V44','V47')
s=s.replace('android-surface-v47-20260917','android-input-v47-20260918').replace('surface-v47-r','input-v47-r')
s=s.replace("P/'system/bin/a6l_surface_services'","P/'system/bin/a6l_input_services'")
s=s.replace('a6l_surface_client','a6l_input_client')
s=s.replace("['surface_services.c','surface_client.cpp','private_properties.h']","['input_services.c','input_client.cpp','input_policy.h','input_fixture.c','private_properties.h']")
# The emulator fixture is deliberately outside the transferable phone payload.
anchor="script=OUT/'qemu.sh';"
s=s.replace(anchor,"fixture=P/'system/bin/a6l_input_fixture'\nassert fixture.is_file()\n"+anchor)
s=s.replace('/tmp/a6l-v47/bin/a6l_graphics_services\nprobe_exit=$?', '/tmp/a6l-v47/bin/a6l_graphics_services\nprobe_exit=$?')
s=s.replace('ready_before=$(/system/bin/getprop servicemanager.ready)', '''/v47/input-fixture > /tmp/fixture.log 2>&1 &
fixture_pid=$!
while ! /system/bin/toybox grep -q A6L_INPUT_FIXTURE_CREATED /tmp/fixture.log; do /system/bin/toybox sleep 0.1; done
ready_before=$(/system/bin/getprop servicemanager.ready)''')
s=s.replace('echo A6L_QEMU_GRAPHICS_DONE','wait "$fixture_pid"\necho A6L_QEMU_INPUT_FIXTURE_EXIT=$?\n/system/bin/toybox cat /tmp/fixture.log\necho A6L_QEMU_GRAPHICS_DONE')
s=s.replace("lines += [f'file /v47/qemu.sh {script} 0755 0 0']", "lines += [f'file /v47/qemu.sh {script} 0755 0 0',f'file /v47/input-fixture {fixture} 0755 0 0']")
# Explicit IDC for both exact hardware names; kernel direct-property remains required.
anchor='for name,info in files.items():\n    p=ARCH'
assert anchor in s
s=s.replace(anchor,"for name in ['generic_ft5x06__8d_', 'A6L_Input_Fixture']:\n    content('root/system/usr/idc/'+name+'.idc','device.internal = 1\\ntouch.deviceType = touchScreen\\ntouch.orientationAware = 1\\n')\n"+anchor)
s=s.replace("b'A6L_SURFACE_VISIBLE'", "b'A6L_INPUT_FRAME_VISIBLE'")
s=s.replace('A6L_SURFACE_SERVICES_PASS','A6L_INPUT_SERVICES_PASS')
s=s.replace("checks['present']=b'A6L_SURFACE_PASS real_surfaceflinger=1 layers=4 transaction=checked' in data", "checks['present']=b'A6L_INPUT_PASS ' in data and b'two_finger=1' in data\nchecks['fixture']=b'A6L_QEMU_INPUT_FIXTURE_EXIT=0' in data and b'A6L_INPUT_FIXTURE_PASS' in data\nmotions=[(int(a),int(n),float(x),float(y)) for a,n,x,y in re.findall(rb'A6L_INPUT_MOTION action=(\\d+) pointers=(\\d+) x=([\\d.]+) y=([\\d.]+)',data)]\nchecks['four_targets']=all(any(a==0 and n==1 and abs(x-tx)<2 and abs(y-ty)<2 for a,n,x,y in motions) for tx,ty in [(180,300),(900,300),(180,2040),(900,2040)])\nchecks['two_finger_delivery']=any(n==2 for a,n,x,y in motions)")
start=s.index("    top=struct.pack(");end=s.index("report={'passed'",start)
s=s[:start]+'''    def pixel(x,y):return struct.unpack_from('<I',frame,(y*1080+x)*4)[0]
    # First pointer ends at (380,900). The visible yellow marker must follow it.
    checks['touch_driven_scanout']=pixel(380,900)==0xffffff00
    checks['outside_frame_untouched']=not any(frame[1080*2340*4:])
'''+s[end:]
s=s.replace('deadline=time.monotonic()+150','deadline=time.monotonic()+220')
s=s.replace('print(data[-18000:].decode', 'print(data[-8000:].decode')
p=T/'Test-AndroidInputV47.py';assert not p.exists();p.write_text(s)
import py_compile
py_compile.compile(str(p),doraise=True)
print('V47 package/QEMU harness generated')
