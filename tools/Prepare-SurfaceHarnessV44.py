"""Generate the V44 diskless SurfaceFlinger test from V43, once."""
from pathlib import Path
r=Path(__file__).resolve().parents[1]
p=r/'tools/Test-AndroidSurfaceV44.py';assert not p.exists()
s=(r/'tools/Test-AndroidPresentV43.py').read_text()
s=s.replace('present-v43','surface-v44').replace('v43','v44')
s=s.replace('a6l_present_services','a6l_surface_services')
s=s.replace("('vendor','bin/a6l_graphics_client')", "('system','bin/a6l_surface_client'),('system','bin/surfaceflinger'),('vendor','lib64/hw/vulkan.pastel.so')")
s=s.replace("P/group/('bin/a6l_present_client' if path=='bin/a6l_graphics_client' else path)","P/group/path")
s=s.replace("['present_services.c','present_client.cpp']","['surface_services.c','surface_client.cpp','private_properties.h']")
s=s.replace('A6L_PRESENT_VISIBLE','A6L_SURFACE_VISIBLE')
s=s.replace('deadline=time.monotonic()+95','deadline=time.monotonic()+150')
s=s.replace(" 'services':b'A6L_GRAPHICS_SERVICES_PASS client=1 services_alive=3 namespace_cleanup=1' in data,\n 'allocator':b'A6L_GRAPHICS_ALLOCATOR_PASS binder=1 mapper5=1 metadata=1 pixels=2527200' in data,\n 'composer':b'A6L_GRAPHICS_CLIENT_PASS allocator_aidl=1 mapper5=1 composer5=1 native_display=1' in data,",
" 'services':b'A6L_SURFACE_SERVICES_PASS client=1 services_alive=4 private_properties=1 namespace_cleanup=1' in data,")
s=s.replace("b'A6L_PRESENT_PASS frames=4 buffers=2 commands=checked fences=checked'", "b'A6L_SURFACE_PASS real_surfaceflinger=1 layers=4 transaction=checked'")
a=s.index('    colors=');b=s.index("    checks['full_scanout_pixels']",a)
s=s[:a]+'''    top=struct.pack('<I',0xffff0000)*540+struct.pack('<I',0xff00ff00)*540
    bottom=struct.pack('<I',0xff0000ff)*540+struct.pack('<I',0xffffffff)*540
    expected=top*1170+bottom*1170
'''+s[b:]
p.write_text(s)
