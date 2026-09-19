"""Derive an Android-init service from the preserved V37 RAM logger."""
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]
p = ROOT / 'device/hisense/a6l/diagnostic/android_service.c'
assert not p.exists()
s = (ROOT / 'device/hisense/a6l/diagnostic/init.c').read_text()
def replace(old, new):
    global s
    assert s.count(old) == 1, old
    s = s.replace(old, new)
replace('#include <sys/mount.h>', '#include <sys/mount.h>\n#include <sys/vfs.h>\n#include <sys/sysmacros.h>\n#include <sys/system_properties.h>\n#include <stdlib.h>')
start=s.index('static int mount_virtual(')
end=s.index('\nstatic int attribute',start)
s=s[:start]+'''static int mount_virtual(const char *type, const char *path)
{
    struct statfs fs;
    uint64_t expected = !strcmp(type, "proc") ? 0x9fa0 :
                    !strcmp(type, "sysfs") ? 0x62656572 : 0x62656570;
    if (statfs(path, &fs) == 0 && fs.f_type == expected)
        return 0;
    if (directory(path) != 0)
        return -1;
    if (mount(type, path, type, MS_NOSUID | MS_NODEV, NULL) == 0)
        return 0;
    message("mount %s: %s\\n", path, strerror(errno));
    return -1;
}

/* Android owns /dev; create only the two nodes this diagnostic needs. */
static int diagnostic_node(const char *name, int block)
{
    char sys[128], dev[64];
    unsigned int major_num, minor_num;
    if (strcmp(name, "mmcblk1") && strcmp(name, "ttyGS0")) return -1;
    snprintf(sys, sizeof(sys), "/sys/class/%s/%s/dev", block ? "block" : "tty", name);
    FILE *f = fopen(sys, "re");
    if (!f) return -1;
    int ok = fscanf(f, "%u:%u", &major_num, &minor_num) == 2;
    fclose(f);
    if (!ok || (block && (major_num != 179 || minor_num != 0))) return -1;
    snprintf(dev, sizeof(dev), "/dev/%s", name);
    mode_t mode = (block ? S_IFBLK : S_IFCHR) | 0600;
    if (mknod(dev, mode, makedev(major_num, minor_num)) && errno != EEXIST) return -1;
    struct stat st;
    return lstat(dev, &st) == 0 && (st.st_mode & S_IFMT) == (mode & S_IFMT) &&
           st.st_rdev == makedev(major_num, minor_num) ? 0 : -1;
}
''' + s[end:]
start=s.index('    if (getpid() != 1) {')
end=s.index('    /* kmsg reaches',start)
s=s[:start]+'''    if (getpid() == 1 || getppid() != 1) {
        fputs("Run this helper only as an Android init service.\\n", stderr);
        return 2;
    }
''' + s[end:]
replace('message("A6L_RAM_PROBE_START\\n");', '''message("A6L_RAM_PROBE_START\\n");
    char init_path[128] = {0};
    ssize_t init_len = readlink("/proc/1/exe", init_path, sizeof(init_path)-1);
    message("A6L_ANDROID_SERVICE_START pid=%ld parent=%ld init=%s\\n",
            (long)getpid(), (long)getppid(), init_len > 0 ? init_path : "unavailable");
    snapshot("/sys/fs/selinux/enforce");
    char prop[PROP_VALUE_MAX];
    __system_property_get("ro.a6l.ramdiag", prop);
    message("A6L_ANDROID_PROPERTY ramdiag=%s\\n", prop);
    __system_property_get("ro.boottime.init.selinux", prop);
    message("A6L_ANDROID_PROPERTY selinux_time=%s\\n", prop);''')
replace('int config_notice = 0, bind_notice', 'int adb_linked = 0;\n    int config_notice = 0, bind_notice')
replace('if (elapsed >= 4 && usb_configured && !bind_attempted)', 'if (elapsed >= 4 && usb_configured && !bind_attempted)')
replace('''            bind_attempted = 1;
            message("A6L_USB_FIND_UDC_BEGIN\\n");''', '''            __system_property_get("sys.usb.ffs.ready", prop);
            if (!strcmp(prop, "1")) {
                adb_linked = directory(GADGET "/functions/ffs.adb") == 0 &&
                    symlink(GADGET "/functions/ffs.adb", GADGET "/configs/c.1/ffs.adb") == 0;
                message("A6L_ANDROID_ADB_FUNCTION linked=%d\\n", adb_linked);
            } else if (elapsed < 8) {
                usleep(100000);
                continue;
            } else {
                message("A6L_ANDROID_ADB_FUNCTION unavailable; keeping ACM fallback\\n");
            }
            bind_attempted = 1;
            message("A6L_USB_FIND_UDC_BEGIN\\n");''')
replace('''            serial = open("/dev/ttyGS0",''', '''            message("A6L_ANDROID_SERIAL_NODE result=%d\\n", diagnostic_node("ttyGS0", 0));
            serial = open("/dev/ttyGS0",''')
replace('''                storage_loaded = WIFEXITED(status) && WEXITSTATUS(status) == 0;''', '''                storage_loaded = WIFEXITED(status) && WEXITSTATUS(status) == 0;
                if (storage_loaded) {
                    int node_ok = diagnostic_node("mmcblk1", 1);
                    message("A6L_ANDROID_STORAGE_NODE result=%d\\n", node_ok);
                    storage_loaded = node_ok == 0;
                }''')
replace('''            message("A6L_RAM_PROBE_ALIVE seconds=%lld usb_attempted=%d serial_attempted=%d sent=%zu\\n",''', '''            __system_property_get("init.svc.adbd", prop);
            message("A6L_ANDROID_ADBD state=%s\\n", prop);
            message("A6L_RAM_PROBE_ALIVE seconds=%lld usb_attempted=%d serial_attempted=%d sent=%zu\\n",''')
p.write_text(s)
print(p)
