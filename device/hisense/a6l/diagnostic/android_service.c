/* SPDX-License-Identifier: GPL-2.0-only */
/* RAM-only PID 1: collect diagnostics and export them through a USB ACM port.
 * No shell, persistent filesystem mounts, or flash commands. V37 adds a forked,
 * fixed-range read-only storage verifier after successful module startup.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/vfs.h>
#include <sys/sysmacros.h>
#include <sys/system_properties.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

int a6l_storage_verify(void (*emit)(const char *, ...));
int a6l_storage_hash_selftest(void);

#define JOURNAL_SIZE (2 * 1024 * 1024)
#define GADGET "/sys/kernel/config/usb_gadget/a6lprobe"
static char journal[JOURNAL_SIZE];
static size_t journal_used;
static int console_fd = -1;
static int status_fd = -1;

static void record(const char *data, size_t size)
{
    if (console_fd >= 0) {
        ssize_t ignored = write(console_fd, data, size);
        (void)ignored;
    }
    if (size <= JOURNAL_SIZE - journal_used) {
        memcpy(journal + journal_used, data, size);
        journal_used += size;
    }
}

static void message(const char *format, ...)
{
    char buffer[1024];
    va_list args;
    va_start(args, format);
    int length = vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    if (length > 0) {
        size_t size = (size_t)length < sizeof(buffer) ? (size_t)length : sizeof(buffer) - 1;
        /* Only fresh status messages go to kmsg. Replayed kernel records must
         * never be written back, or the collector would feed itself forever.
         */
        if (status_fd >= 0) {
            ssize_t ignored = write(status_fd, buffer, size);
            (void)ignored;
        }
        record(buffer, size);
    }
}

static void deferred_devices(void)
{
    char line[512];
    FILE *file = fopen("/sys/kernel/debug/devices_deferred", "re");
    message("A6L_DEFERRED_BEGIN\n");
    if (!file) {
        message("A6L_DEFERRED unavailable: %s\n", strerror(errno));
    } else {
        unsigned int lines = 0;
        while (lines < 64 && fgets(line, sizeof(line), file)) {
            message("A6L_DEFERRED %s", line);
            ++lines;
        }
        if (!feof(file))
            message("A6L_DEFERRED list limited to 64 lines\n");
        fclose(file);
    }
    message("A6L_DEFERRED_END\n");
}

static int directory(const char *path)
{
    if (mkdir(path, 0755) == 0 || errno == EEXIST)
        return 0;
    message("mkdir %s: %s\n", path, strerror(errno));
    return -1;
}

static int mount_virtual(const char *type, const char *path)
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
    message("mount %s: %s\n", path, strerror(errno));
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

static int attribute(const char *path, const char *value)
{
    int fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0) {
        message("open %s: %s\n", path, strerror(errno));
        return -1;
    }
    size_t length = strlen(value);
    ssize_t written;
    do {
        written = write(fd, value, length);
    } while (written < 0 && errno == EINTR);
    int saved_errno = errno;
    close(fd);
    if (written != (ssize_t)length) {
        message("write %s: %s\n", path, written < 0 ? strerror(saved_errno) : "short write");
        return -1;
    }
    return 0;
}

static void snapshot(const char *path)
{
    char buffer[4096];
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    message("\n--- %s ---\n", path);
    if (fd < 0) {
        message("unavailable: %s\n", strerror(errno));
        return;
    }
    ssize_t count;
    size_t total = 0;
    while (total < 65536 && (count = read(fd, buffer, sizeof(buffer))) > 0) {
        record(buffer, (size_t)count);
        total += (size_t)count;
    }
    close(fd);
}

static int find_udc(char *name, size_t capacity)
{
    DIR *dir = opendir("/sys/class/udc");
    if (!dir)
        return -1;
    struct dirent *entry;
    int found = -1;
    while ((entry = readdir(dir))) {
        if (entry->d_name[0] == '.')
            continue;
        if (strlen(entry->d_name) + 1 <= capacity) {
            strcpy(name, entry->d_name);
            found = 0;
        }
        break;
    }
    closedir(dir);
    return found;
}

static int configure_usb(void)
{
    if (directory(GADGET) ||
        attribute(GADGET "/idVendor", "0x1d6b") ||
        attribute(GADGET "/idProduct", "0x0104") ||
        attribute(GADGET "/bcdDevice", "0x0001") ||
        attribute(GADGET "/bcdUSB", "0x0200") ||
        directory(GADGET "/strings/0x409") ||
        attribute(GADGET "/strings/0x409/serialnumber", "HLTE730T-PROBE") ||
        attribute(GADGET "/strings/0x409/manufacturer", "A6L development") ||
        attribute(GADGET "/strings/0x409/product", "A6L RAM diagnostics") ||
        directory(GADGET "/configs/c.1") ||
        attribute(GADGET "/configs/c.1/MaxPower", "100") ||
        directory(GADGET "/functions/acm.usb0"))
        return -1;
    if (symlink(GADGET "/functions/acm.usb0", GADGET "/configs/c.1/acm.usb0") != 0) {
        message("link ACM function: %s\n", strerror(errno));
        return -1;
    }
    return 0;
}

static time_t monotonic_seconds(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        message("A6L_CLOCK_FAILED; stopping staged diagnostics\n");
        for (;;)
            sleep(60);
    }
    return now.tv_sec;
}

/* Small read-only sysfs attributes, also visible on the LCD. */
static void usb_status(const char *udc)
{
    const char *attributes[] = { "state", "current_speed", "device/power/runtime_status" };
    for (unsigned int i = 0; i < sizeof(attributes) / sizeof(attributes[0]); ++i) {
        char path[512], value[128];
        snprintf(path, sizeof(path), "/sys/class/udc/%s/%s", udc, attributes[i]);
        int fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            message("A6L_USB_STATUS %s unavailable errno=%d\n", attributes[i], errno);
            continue;
        }
        ssize_t count = read(fd, value, sizeof(value) - 1);
        int saved_errno = errno;
        close(fd);
        if (count >= 0) {
            value[count] = 0;
            value[strcspn(value, "\r\n")] = 0;
            message("A6L_USB_STATUS %s=%s\n", attributes[i], value);
        } else {
            message("A6L_USB_STATUS %s read errno=%d\n", attributes[i], saved_errno);
        }
    }
}

int main(void)
{
    if (getpid() == 1 || getppid() != 1) {
        fputs("Run this helper only as an Android init service.\n", stderr);
        return 2;
    }
    /* kmsg reaches the working early LCD console. The normal /dev/console
     * may instead select an unavailable UART. Do not depend on opening it.
     */
    status_fd = open("/dev/kmsg", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if (status_fd < 0)
        console_fd = open("/dev/console", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    message("A6L_RAM_PROBE_START\n");
    char init_path[128] = {0};
    ssize_t init_len = readlink("/proc/1/exe", init_path, sizeof(init_path)-1);
    message("A6L_ANDROID_SERVICE_START pid=%ld parent=%ld init=%s\n",
            (long)getpid(), (long)getppid(), init_len > 0 ? init_path : "unavailable");
    snapshot("/sys/fs/selinux/enforce");
    char prop[PROP_VALUE_MAX];
    __system_property_get("ro.a6l.ramdiag", prop);
    message("A6L_ANDROID_PROPERTY ramdiag=%s\n", prop);
    __system_property_get("ro.boottime.init.selinux", prop);
    message("A6L_ANDROID_PROPERTY selinux_time=%s\n", prop);
    if (mount_virtual("proc", "/proc") || mount_virtual("sysfs", "/sys")) {
        message("Required virtual filesystem unavailable; stopping diagnostics.\n");
        for (;;)
            sleep(60);
    }
    /* This RAM-only logging setting keeps a bounded dependency snapshot from
     * being discarded by the per-writer /dev/kmsg burst limit.
     */
    attribute("/proc/sys/kernel/printk_devkmsg", "on\n");
    struct utsname version;
    if (uname(&version) == 0)
        message("kernel=%s machine=%s\n", version.release, version.machine);
    snapshot("/proc/cmdline");
    snapshot("/proc/meminfo");
    snapshot("/proc/partitions");
    snapshot("/proc/mounts");
    message("A6L_RAM_PROBE_READY: virtual filesystems mounted; no persistent mounts.\n");
    int debugfs_ok = mount("debugfs", "/sys/kernel/debug", "debugfs",
                           MS_RDONLY | MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL) == 0;
    message("A6L_DEBUGFS read-only mount: %s\n", debugfs_ok ? "success" : strerror(errno));
    if (debugfs_ok)
        deferred_devices();
    int configfs_ok = mount_virtual("configfs", "/sys/kernel/config") == 0;
    int kmsg = open("/dev/kmsg", O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    int serial = -1;
    size_t sent = 0;
    int usb_attempted = 0;
    int usb_configured = 0, bind_attempted = 0, usb_ready = 0;
    int serial_attempted = 0, first_write = 1;
    int adb_linked = 0;
    int config_notice = 0, bind_notice = 0, connect_notice = 0, serial_notice = 0;
    int connect_attempted = 0, connect_ok = 0;
    int deferred20 = 0, deferred60 = 0;
    int storage_attempted = 0, storage_snapshot = 0, storage_armed = 0;
    size_t storage_notice_end = 0;
    pid_t storage_child = -1;
    pid_t read_child = -1;
    int storage_loaded = 0, read_attempted = 0;
    int storage_crypto_ok = a6l_storage_hash_selftest() == 0;
    message("A6L_STORAGE_HASH_SELFTEST success=%d\n", storage_crypto_ok);
    char udc[256] = "";
    time_t started = monotonic_seconds(), next_heartbeat = 0;
    message("A6L_STAGED_STORAGE_V20 config_after=0 bind_after=4 connect_after=8 serial_after=12 module_after=20 heartbeat=2\n");
    for (;;) {
        time_t elapsed = monotonic_seconds() - started;
        char buffer[8192];
        ssize_t count;
        if (kmsg >= 0) {
            for (int i = 0; i < 64; ++i) {
                count = read(kmsg, buffer, sizeof(buffer));
                if (count > 0)
                    record(buffer, (size_t)count);
                else if (count < 0 && errno == EPIPE)
                    continue;
                else
                    break;
            }
        }
        if (elapsed >= next_heartbeat) {
            __system_property_get("init.svc.adbd", prop);
            message("A6L_ANDROID_ADBD state=%s\n", prop);
            message("A6L_RAM_PROBE_ALIVE seconds=%lld usb_attempted=%d serial_attempted=%d sent=%zu\n",
                    (long long)elapsed, usb_attempted, serial_attempted, sent);
            next_heartbeat = elapsed + 2;
            if (udc[0])
                usb_status(udc);
        }
        if (elapsed >= 0 && configfs_ok && !config_notice) {
            config_notice = 1;
            message("A6L_NEXT: USB CONFIGURATION at seconds=0; UDC stays unbound\n");
        }
        if (elapsed >= 0 && configfs_ok && !usb_attempted) {
            usb_attempted = 1;
            message("A6L_USB_CONFIG_BEGIN seconds=%lld\n", (long long)elapsed);
            usb_configured = configure_usb() == 0;
            message("A6L_USB_CONFIG_END success=%d; UDC still unbound\n", usb_configured);
        }
        if (elapsed >= 2 && usb_configured && !bind_notice) {
            bind_notice = 1;
            message("A6L_NEXT: USB BINDING at seconds=4; connection held by kernel\n");
        }
        if (elapsed >= 4 && usb_configured && !bind_attempted) {
            __system_property_get("sys.usb.ffs.ready", prop);
            if (!strcmp(prop, "1")) {
                adb_linked = directory(GADGET "/functions/ffs.adb") == 0 &&
                    symlink(GADGET "/functions/ffs.adb", GADGET "/configs/c.1/ffs.adb") == 0;
                message("A6L_ANDROID_ADB_FUNCTION linked=%d\n", adb_linked);
            } else if (elapsed < 8) {
                usleep(100000);
                continue;
            } else {
                message("A6L_ANDROID_ADB_FUNCTION unavailable; keeping ACM fallback\n");
            }
            bind_attempted = 1;
            message("A6L_USB_FIND_UDC_BEGIN\n");
            if (find_udc(udc, sizeof(udc)) == 0) {
                message("A6L_USB_BIND_BEGIN udc=%s\n", udc);
                usb_ready = attribute(GADGET "/UDC", udc) == 0;
                message("A6L_USB_BIND_END success=%d\n", usb_ready);
            } else {
                message("A6L_USB_FIND_UDC_END unavailable; no binding attempted\n");
            }
        }
        if (elapsed >= 6 && usb_ready && !connect_notice) {
            connect_notice = 1;
            message("A6L_NEXT: USB CONNECT at seconds=8; serial stays closed\n");
        }
        if (elapsed >= 8 && usb_ready && !connect_attempted) {
            char path[512];
            connect_attempted = 1;
            snprintf(path, sizeof(path), "/sys/class/udc/%s/soft_connect", udc);
            message("A6L_USB_CONNECT_BEGIN seconds=%lld\n", (long long)elapsed);
            connect_ok = attribute(path, "connect\n") == 0;
            message("A6L_USB_CONNECT_END success=%d\n", connect_ok);
        }
        if (elapsed >= 10 && connect_ok && !serial_notice) {
            serial_notice = 1;
            message("A6L_NEXT: SERIAL OPEN at seconds=12\n");
        }
        if (elapsed >= 12 && connect_ok && !serial_attempted) {
            serial_attempted = 1;
            message("A6L_SERIAL_OPEN_BEGIN seconds=%lld\n", (long long)elapsed);
            message("A6L_ANDROID_SERIAL_NODE result=%d\n", diagnostic_node("ttyGS0", 0));
            serial = open("/dev/ttyGS0", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
            int saved_errno = serial < 0 ? errno : 0;
            message("A6L_SERIAL_OPEN_END fd=%d errno=%d\n", serial, saved_errno);
            sent = 0;
        }
        if (serial >= 0 && sent < journal_used) {
            size_t remaining = journal_used - sent;
            if (remaining > 16384)
                remaining = 16384;
            if (first_write)
                message("A6L_SERIAL_WRITE_BEGIN bytes=%zu\n", remaining);
            count = write(serial, journal + sent, remaining);
            int saved_errno = count < 0 ? errno : 0;
            if (first_write) {
                message("A6L_SERIAL_WRITE_END count=%zd errno=%d\n", count, saved_errno);
                first_write = 0;
            }
            if (count > 0)
                sent += (size_t)count;
            else if (count < 0 && saved_errno != EAGAIN && saved_errno != EINTR) {
                message("A6L_SERIAL_CLOSE_BEGIN errno=%d\n", saved_errno);
                close(serial);
                message("A6L_SERIAL_CLOSE_END; no reopen in this trial\n");
                serial = -1;
            }
        }
        /* Load only the bundled eMMC driver, after USB has carried logs. Keep
         * the parent draining kmsg while module initialization runs in a child.
         * No retries, shell, module unloading or persistent device opens.
         */
        if (elapsed >= 18 && serial >= 0 && sent > 0 && !storage_armed) {
            storage_armed = 1;
            message("A6L_STORAGE_MODULE_FORK_ARMED earliest=20 child_pause_ms=1000\n");
            storage_notice_end = journal_used;
        }
        if (elapsed >= 20 && serial >= 0 && storage_armed &&
            sent >= storage_notice_end && !storage_attempted) {
            storage_attempted = 1;
            message("A6L_STORAGE_MODULE_FORK_BEGIN seconds=%lld\n", (long long)elapsed);
            storage_child = fork();
            if (storage_child == 0) {
                close(serial);
                if (kmsg >= 0)
                    close(kmsg);
                int fd = open("/sdhci-msm.ko", O_RDONLY | O_CLOEXEC);
                if (fd < 0) {
                    message("A6L_STORAGE_MODULE_OPEN_FAILED errno=%d\n", errno);
                    _exit(1);
                }
                message("A6L_STORAGE_MODULE_LOAD_BEGIN pause_ms=1000\n");
                usleep(1000000);
                long result = syscall(__NR_finit_module, fd, "", 0);
                int saved_errno = result < 0 ? errno : 0;
                close(fd);
                message("A6L_STORAGE_MODULE_LOAD_END result=%ld errno=%d\n", result, saved_errno);
                _exit(result == 0 ? 0 : 1);
            }
            if (storage_child < 0)
                message("A6L_STORAGE_MODULE_FORK_FAILED errno=%d\n", errno);
            else
                message("A6L_STORAGE_MODULE_CHILD pid=%ld\n", (long)storage_child);
        }
        if (storage_child > 0) {
            int status;
            pid_t result = waitpid(storage_child, &status, WNOHANG);
            if (result == storage_child) {
                message("A6L_STORAGE_MODULE_CHILD_EXIT status=%d\n", status);
                storage_loaded = WIFEXITED(status) && WEXITSTATUS(status) == 0;
                if (storage_loaded) {
                    int node_ok = diagnostic_node("mmcblk1", 1);
                    message("A6L_ANDROID_STORAGE_NODE result=%d\n", node_ok);
                    storage_loaded = node_ok == 0;
                }
                storage_child = -1;
                snapshot("/proc/modules");
                snapshot("/proc/partitions");
            }
        }
        if (storage_attempted && elapsed >= 30 && !storage_snapshot) {
            storage_snapshot = 1;
            snapshot("/proc/partitions");
            message("A6L_STORAGE_SNAPSHOT_DONE\n");
        }
        if (storage_crypto_ok && storage_loaded && elapsed >= 32 && serial >= 0 && !read_attempted) {
            read_attempted = 1;
            message("A6L_STORAGE_READ_FORK_BEGIN seconds=%lld\n", (long long)elapsed);
            read_child = fork();
            if (read_child == 0) {
                close(serial);
                if (kmsg >= 0)
                    close(kmsg);
                _exit(a6l_storage_verify(message));
            }
            if (read_child < 0)
                message("A6L_STORAGE_READ_FORK_FAILED errno=%d\n", errno);
        }
        if (read_child > 0) {
            int status;
            pid_t result = waitpid(read_child, &status, WNOHANG);
            if (result == read_child) {
                message("A6L_STORAGE_READ_CHILD_EXIT status=%d\n", status);
                read_child = -1;
            }
        }
        if (debugfs_ok && ((!deferred20 && elapsed >= 20) || (!deferred60 && elapsed >= 60))) {
            if (elapsed >= 20) deferred20 = 1;
            if (elapsed >= 60) deferred60 = 1;
            deferred_devices();
        }
        usleep(100000);
    }
}
