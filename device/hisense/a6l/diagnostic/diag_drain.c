// SPDX-License-Identifier: Apache-2.0
/* A6L modem bring-up: open the modem's DIAG rpmsg channels and drain them, so the modem's diag task is not starved
 * (21 Sep 2026: mpss booted, all QMI services up, then "dog_hb.c: Task starvation: diag" 16 s later, in a loop).
 * This is the minimal stand-in for linux-msm/diag "diag-router" (which needs libudev). Nothing is ever written to the modem.
 * usage: a6l_diag_drain [seconds]   (run BEFORE starting the modem; it waits for the rpmsg control device to appear) */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/rpmsg.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <time.h>
#include <unistd.h>
static const char *chans[] = {"DIAG_CNTL", "DIAG", "DIAG_CMD", "DIAG_DCI_CNTL", "DIAG_DCI", NULL};
static int devnode(const char *cls, const char *name, char *out, size_t n) {   /* recovery has no udev: mknod from sysfs */
    char p[256], b[32]; snprintf(p, sizeof p, "/sys/class/%s/%s/dev", cls, name); FILE *f = fopen(p, "r"); if (!f) return -1;
    unsigned ma, mi; if (!fgets(b, sizeof b, f) || sscanf(b, "%u:%u", &ma, &mi) != 2) { fclose(f); return -1; } fclose(f);
    snprintf(out, n, "/dev/%s", name); mknod(out, S_IFCHR | 0600, makedev(ma, mi)); return 0;
}
static int is_modem_ctrl(const char *name) {
    char p[256], l[512]; snprintf(p, sizeof p, "/sys/class/rpmsg/%s", name); ssize_t k = readlink(p, l, sizeof l - 1); if (k < 0) return 0; l[k] = 0;
    return strstr(l, "4080000") || strstr(l, "modem") || strstr(l, "mpss");
}
int main(int argc, char **argv) {
    int secs = argc > 1 ? atoi(argv[1]) : 600; time_t end = time(0) + secs; struct pollfd fds[8]; int nf = 0; unsigned long total = 0;
    setvbuf(stdout, 0, _IONBF, 0);
    while (time(0) < end) {
        if (!nf) {   /* (re)attach: endpoints vanish when the modem restarts */
            DIR *d = opendir("/sys/class/rpmsg"); struct dirent *e; char ctrl[64] = "";
            while (d && (e = readdir(d))) if (!strncmp(e->d_name, "rpmsg_ctrl", 10) && is_modem_ctrl(e->d_name)) devnode("rpmsg", e->d_name, ctrl, sizeof ctrl);
            if (d) closedir(d);
            if (ctrl[0]) { int c = open(ctrl, O_RDWR); if (c >= 0) {
                for (int i = 0; chans[i]; i++) { struct rpmsg_endpoint_info ei; memset(&ei, 0, sizeof ei); strncpy(ei.name, chans[i], sizeof ei.name - 1); ei.src = 0xffffffff; ei.dst = 0xffffffff;
                    if (ioctl(c, RPMSG_CREATE_EPT_IOCTL, &ei)) printf("A6L_DIAG ept %s: %s\n", chans[i], strerror(errno)); }
                close(c); usleep(200000);
                d = opendir("/sys/class/rpmsg");
                while (d && (e = readdir(d)) && nf < 8) { if (strncmp(e->d_name, "rpmsg", 5) || !strncmp(e->d_name, "rpmsg_ctrl", 10)) continue;
                    char p[256], nm[64] = "", dev[64]; snprintf(p, sizeof p, "/sys/class/rpmsg/%s/name", e->d_name); FILE *f = fopen(p, "r"); if (f) { if (!fgets(nm, sizeof nm, f)) nm[0] = 0; fclose(f); }
                    if (strncmp(nm, "DIAG", 4) || devnode("rpmsg", e->d_name, dev, sizeof dev)) continue;
                    int fd = open(dev, O_RDONLY | O_NONBLOCK); if (fd < 0) { printf("A6L_DIAG open %s: %s\n", dev, strerror(errno)); continue; }
                    nm[strcspn(nm, "\n")] = 0; printf("A6L_DIAG draining %s (%s)\n", dev, nm); fds[nf].fd = fd; fds[nf].events = POLLIN; nf++; }
                if (d) closedir(d); } }
            if (!nf) { sleep(1); continue; }
        }
        int r = poll(fds, nf, 1000); if (r <= 0) continue;
        for (int i = 0; i < nf; i++) { if (fds[i].revents & (POLLERR | POLLHUP | POLLNVAL)) { printf("A6L_DIAG channel closed, re-attaching (drained %lu bytes)\n", total); for (int j = 0; j < nf; j++) close(fds[j].fd); nf = 0; sleep(2); break; }
            if (fds[i].revents & POLLIN) { char buf[16384]; ssize_t k = read(fds[i].fd, buf, sizeof buf); if (k > 0) total += k; } }
    }
    printf("A6L_DIAG_DONE drained=%lu bytes\n", total); return 0;
}
