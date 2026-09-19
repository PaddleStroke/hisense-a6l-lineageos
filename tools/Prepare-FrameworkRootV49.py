"""Prepare a QEMU-only normal-root supervisor from the validated V48 runtime harness."""
from pathlib import Path
R=Path(__file__).resolve().parents[1];D=R/'device/hisense/a6l/diagnostic'
if (D/'framework_root_services.c').exists() or (D/'framework_root_properties.h').exists():
    raise SystemExit('Historical one-time V49 generator: current supervisor sources already exist; refusing to overwrite later validated fixes.')
s=(D/'framework_services.c').read_text().replace('#define ROOT "/tmp/a6l-v48/root"','#define ROOT "/"')
s=s.replace('framework_properties.h','framework_root_properties.h')
s=s.replace('static void launch_zygote(void) {',r'''
/* Real cgroup filesystems in the disposable VM; no synthetic scheduling files. */
static void setup_vm_cgroups(void) {
    const char *controllers[]={"cpu","blkio"};
    const char *paths[]={"/dev/cpuctl","/dev/blkio"};
    const char *groups[]={"", "foreground", "foreground_window", "background",
                         "top-app", "rt", "system", "system-background", "dex2oat"};
    for(int i=0;i<2;i++){
        dir(paths[i]);need(!mount("cgroup",paths[i],"cgroup",MS_NOSUID|MS_NODEV|MS_NOEXEC,controllers[i]),"VM cgroup v1");
        for(unsigned j=0;j<sizeof(groups)/sizeof(groups[0]);j++){
            char path[256],file[320];snprintf(path,sizeof(path),"%s/%s",paths[i],groups[j]);dir(path);
            need(!chown(path,1000,1000)&&!chmod(path,0775),"cgroup ownership");
            const char *files[]={"tasks","cgroup.procs"};
            for(int k=0;k<2;k++){
                snprintf(file,sizeof(file),"%s/%s",path,files[k]);
                need(!chown(file,1000,1000)&&!chmod(file,0664),"cgroup task permissions");
            }
        }
    }
    need(!mount("cgroup2","/sys/fs/cgroup","cgroup2",MS_NOSUID|MS_NODEV|MS_NOEXEC,NULL),"VM cgroup v2");
    const char *v2[]={"/sys/fs/cgroup","/sys/fs/cgroup/system","/sys/fs/cgroup/apps"};
    for(int i=0;i<3;i++){
        dir(v2[i]);need(!chown(v2[i],1000,1000)&&!chmod(v2[i],0775),"cgroup v2 ownership");
        char f[256];snprintf(f,sizeof(f),"%s/cgroup.subtree_control",v2[i]);
        int fd=open(f,O_WRONLY|O_CLOEXEC);need(fd>=0,"memory controller file");
        need(write(fd,"+memory",7)==7,"memory controller activation");close(fd);
    }
    puts("A6L_VM_CGROUPS_READY cpu_v1=1 blkio_v1=1 memory_v2=1 cpuset_v1=0");
}
static void launch_zygote(void) {''')
s=s.replace('    const char *names[]={"zygote","usap_pool_primary"};',
'''    /* Match init's descriptor hygiene; never leak supervisor/shell log FDs. */
    int nullfd=open("/dev/null",O_RDWR);need(nullfd>=0,"zygote null stdio");
    need(dup2(nullfd,0)>=0&&dup2(nullfd,1)>=0&&dup2(nullfd,2)>=0,"zygote stdio");
    if(nullfd>2)close(nullfd);
    need(!syscall(SYS_close_range,3u,~0u,0),"zygote inherited fd cleanup");
    const char *names[]={"zygote","usap_pool_primary"};''')
# Preserve init's property schema before the private /dev mount covers it.
s=s.replace('    bind_ro("/sys",ROOT "/sys");',
'''    dir("/v49-oldselinux");
    need(!mount("/sys/fs/selinux","/v49-oldselinux",NULL,MS_BIND,NULL),"preserve SELinux mount");
    bind_ro("/sys",ROOT "/sys");''')
s=s.replace('mount("/sys/fs/selinux",ROOT "/sys/fs/selinux"','mount("/v49-oldselinux",ROOT "/sys/fs/selinux"')
s=s.replace('    dir(ROOT "/dev");', '    bind_ro("/dev","/v49-olddev");\n    dir(ROOT "/dev");')
s=s.replace('    start_logger();','    setup_vm_cgroups();\n    start_logger();')
s=s.replace('    dir(ROOT "/logs");', '''    need(!chmod("/dev/binderfs/a6l-v48",0666)&&!chmod("/dev/binderfs/a6l-v48-hw",0666),"standard Binder device permissions");
    dir(ROOT "/logs");''')
s=s.replace('    need(!umount(ROOT "/dev/binderfs"),', '''    need(!umount("/dev/cpuctl")&&!umount("/dev/blkio")&&!umount("/sys/fs/cgroup"),"cgroups cleanup");
    need(!umount(ROOT "/dev/binderfs"),''')
s=s.replace('    need(!umount(ROOT "/sys"),', '    need(!umount("/v49-oldselinux"),"old selinux cleanup");\n    need(!umount(ROOT "/sys"),')
s=s.replace('if(chroot(ROOT)||chdir("/"))_exit(126);','if(chdir("/"))_exit(126);')
s=s.replace('    puts("A6L_FRAMEWORK_SERVICES_PASS', '    need(!umount("/v49-olddev"),"old devices cleanup");\n    puts("A6L_FRAMEWORK_SERVICES_PASS')
(D/'framework_root_services.c').write_text(s)
p=(D/'framework_properties.h').read_text().replace('open("/dev/__properties__/property_info"','open("/v49-olddev/__properties__/property_info"')
p=p.replace('need(!chroot(ROOT)&&!chdir("/"),"property chroot");','need(!chdir("/"),"property root");')
(D/'framework_root_properties.h').write_text(p)
bp=R/'device/hisense/a6l/Android.bp';old=bp.read_text()
if 'name: "a6l_framework_root_services"' not in old:
    bp.write_text(old+'''\ncc_binary {
    name: "a6l_framework_root_services",
    srcs: ["diagnostic/framework_root_services.c"],
    static_executable: true,
    compile_multilib: "64",
    system_shared_libs: [],
    static_libs: ["libc"],
    stl: "none",
    cflags: ["-Wall", "-Wextra", "-Werror"],
}
''')
print('QEMU-only normal-root supervisor prepared; hardware model guard retained')
