"""Prepare fresh V44 phone runners, without staging or executing them."""
from pathlib import Path
r=Path(__file__).resolve().parents[1]
replacements={
'Run-AndroidPresentV43.py':'Run-AndroidSurfaceV44.py',
'Run-LaptopPresentCapture-v43.py':'Run-LaptopSurfaceCapture-v44.py',
'Verify-PresentV43Stage.py':'Verify-SurfaceV44Stage.py',
'Launch-PresentV43.py':'Launch-SurfaceV44.py'}
for old,new in replacements.items():
    p=r/'tools'/new;assert not p.exists()
    s=(r/'tools'/old).read_text().replace('PresentV43','SurfaceV44').replace('PresentCapture','SurfaceCapture')
    s=s.replace('present-v43','surface-v44').replace('present-user-v43','surface-user-v44')
    s=s.replace('v43','v44').replace('V43','V44')
    if new.startswith('Run-Android'):
        s=s.replace("timeout=75,required=False", "timeout=115,required=False")
        s=s.replace("'A6L_GRAPHICS_SERVICES_PASS client=1 services_alive=3 namespace_cleanup=1'", "'A6L_SURFACE_SERVICES_PASS client=1 services_alive=5 private_properties=1 namespace_cleanup=1'")
        s=s.replace("'A6L_GRAPHICS_CLIENT_PASS allocator_aidl=1 mapper5=1 composer5=1 native_display=1'", "'A6L_SURFACE_PASS real_surfaceflinger=1 layers=4 transaction=checked'")
        s=s.replace("'A6L_PRESENT_PASS frames=4 buffers=2 commands=checked fences=checked'", "'A6L_SURFACE_PASS real_surfaceflinger=1 layers=4 transaction=checked'")
    if new.startswith('Run-Laptop'):
        s=s.replace('Collect-ProbeSerial-v38.py','Wait-AndroidRamReady.py')
        s=s.replace("str(CAPTURE / 'serial'), '--seconds', '1800'", "str(CAPTURE / 'readiness'), '--menu-seconds', '1800'")
    p.write_text(s)
