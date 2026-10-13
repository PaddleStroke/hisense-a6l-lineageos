"""Create V44 harness sources from the verified V43 scaffold, once."""
from pathlib import Path
root=Path(__file__).resolve().parents[1]
diag=root/'device/hisense/a6l/diagnostic'
p=diag/'surface_services.c'
assert not p.exists()
s=(diag/'present_services.c').read_text().replace('a6l-v43','a6l-v44')
s=s.replace('children[5]','children[7]').replace('i=4;i>=0','i=6;i>=0')
s=s.replace('static char old_ready[PROP_VALUE_MAX];\nstatic int restore_ready,restore_failed;','')
s=s.replace('    if(restore_ready){restore_failed=__system_property_set("servicemanager.ready",old_ready)!=0;if(!restore_failed)restore_ready=0;}\n','')
a=s.index('static void bind_property_socket(');b=s.index('static pid_t start(',a)
s=s[:a]+'#include "private_properties.h"\n'+s[b:]
s=s.replace('alarm(65)','alarm(100)')
s=s.replace('    bind_ro("/dev/__properties__",ROOT "/dev/__properties__");\n','')
s=s.replace('    __system_property_get("servicemanager.ready",old_ready);\n    bind_property_socket("property_service");bind_property_socket("property_service_for_system");restore_ready=1;',
            '    children[5]=start_private_properties();')
s=s.replace('    pid_t c=start(3,"/vendor/bin/a6l_graphics_client",1,"client");',
            '    start(6,"/system/bin/surfaceflinger",0,"surfaceflinger");\n    pid_t c=start(3,"/system/bin/a6l_surface_client",0,"client");')
s=s.replace('    need(WIFEXITED(status)', '    {int s=0;need(waitpid(children[6],&s,WNOHANG)==0,"SurfaceFlinger remained alive");}\n    need(WIFEXITED(status)')
a=s.index('    need(!restore_failed');b=s.index('    need(!umount(ROOT "/dev/binderfs")',a)
s=s[:a]+s[b:]
s=s.replace('A6L_GRAPHICS_SERVICES_PASS client=1 services_alive=3 namespace_cleanup=1',
            'A6L_SURFACE_SERVICES_PASS client=1 services_alive=4 private_properties=1 namespace_cleanup=1')
p.write_text(s)
bp=root/'device/hisense/a6l/Android.bp'
bp.write_text(bp.read_text()+'''
cc_binary {
    name: "a6l_surface_services",
    srcs: ["diagnostic/surface_services.c"],
    static_executable: true,
    compile_multilib: "64",
    system_shared_libs: [],
    static_libs: ["libc"],
    stl: "none",
    cflags: ["-Wall", "-Wextra", "-Werror"],
}
cc_binary {
    name: "a6l_surface_client",
    srcs: ["diagnostic/surface_client.cpp"],
    compile_multilib: "64",
    shared_libs: ["libgui", "libbinder", "libui", "libutils", "libcutils", "liblog"],
    cflags: ["-Wall", "-Wextra", "-Werror"],
}
''')
s=(root/'tools/build-android-present-v43.sh').read_text().replace('present-v43','surface-v44')
s=s.replace('diagnostic/present_services.c diagnostic/present_client.cpp',
            'diagnostic/surface_services.c diagnostic/surface_client.cpp diagnostic/private_properties.h')
s=s.replace('m -j8 a6l_present_services a6l_present_client','m -j8 a6l_surface_services a6l_surface_client surfaceflinger vulkan.pastel')
(root/'tools/build-android-surface-v44.sh').write_text(s)
