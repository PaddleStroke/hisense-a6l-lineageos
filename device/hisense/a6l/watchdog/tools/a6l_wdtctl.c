// SPDX-License-Identifier: GPL-2.0-only
// a6l_wdtctl (cpufreq-watchdog agent, 29 Sep 2026): attended/QEMU test tool for the APSS watchdog (qcom-wdt) and the
// Android watchdogd contract. Static aarch64 (NDK) so it runs from adb shell, recovery or a bare QEMU initramfs.
//   a6l_wdtctl info  [dev]                    open, print support/timeout/pretimeout/bootstatus, magic-close (stops it)
//   a6l_wdtctl pet   <timeout> <interval> <n> [dev]  like watchdogd: SETTIMEOUT, ping n times every interval s, magic-close
//   a6l_wdtctl bite  <timeout> [dev]          SETTIMEOUT then stop pinging WITHOUT closing: the board must reset
// Opening a watchdog starts it; info/pet always end with the magic 'V' close (no NOWAYOUT in the A6L kernels), so they
// leave it stopped. /dev/watchdog busy = watchdogd already owns it (that is fine, report and exit 3).
#include <errno.h>
#include <fcntl.h>
#include <linux/watchdog.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

static void kmsg(const char *s) {
	int k = open("/dev/kmsg", O_WRONLY | O_CLOEXEC);
	if (k >= 0) { dprintf(k, "A6L_WDT %s\n", s); close(k); }
	printf("A6L_WDT %s\n", s); fflush(stdout);
}

static int opendev(const char *dev) {
	int fd = open(dev, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		printf("A6L_WDT open %s failed: %s\n", dev, strerror(errno));
		exit(errno == EBUSY ? 3 : 2);
	}
	return fd;
}

static void show(int fd) {
	struct watchdog_info wi; int v;
	if (!ioctl(fd, WDIOC_GETSUPPORT, &wi))
		printf("A6L_WDT identity=\"%s\" options=0x%x fw=%u%s%s%s%s\n", wi.identity, wi.options, wi.firmware_version,
		       wi.options & WDIOF_SETTIMEOUT ? " settimeout" : "", wi.options & WDIOF_MAGICCLOSE ? " magicclose" : "",
		       wi.options & WDIOF_PRETIMEOUT ? " pretimeout" : "", wi.options & WDIOF_CARDRESET ? " cardreset" : "");
	if (!ioctl(fd, WDIOC_GETTIMEOUT, &v)) printf("A6L_WDT timeout=%d\n", v);
	if (!ioctl(fd, WDIOC_GETPRETIMEOUT, &v)) printf("A6L_WDT pretimeout=%d\n", v);
	if (!ioctl(fd, WDIOC_GETBOOTSTATUS, &v)) printf("A6L_WDT bootstatus=0x%x%s\n", v, v & WDIOF_CARDRESET ? " (last reset by the watchdog)" : "");
	if (!ioctl(fd, WDIOC_GETTIMELEFT, &v)) printf("A6L_WDT timeleft=%d\n", v);
	fflush(stdout);
}

static int settimeout(int fd, int t) {
	int v = t;
	if (ioctl(fd, WDIOC_SETTIMEOUT, &v)) { printf("A6L_WDT SETTIMEOUT %d failed: %s\n", t, strerror(errno)); return -1; }
	printf("A6L_WDT SETTIMEOUT %d -> %d\n", t, v); fflush(stdout);
	return v;
}

static void magic_close(int fd) {
	if (write(fd, "V", 1) != 1) printf("A6L_WDT magic write failed: %s\n", strerror(errno));
	close(fd); printf("A6L_WDT closed with magic V (stopped)\n"); fflush(stdout);
}

int main(int argc, char **argv) {
	if (argc < 2) { fprintf(stderr, "usage: %s info|pet|bite ...\n", argv[0]); return 1; }
	const char *cmd = argv[1];
	if (!strcmp(cmd, "info")) {
		int fd = opendev(argc > 2 ? argv[2] : "/dev/watchdog");
		show(fd); magic_close(fd); kmsg("INFO_DONE"); return 0;
	}
	if (!strcmp(cmd, "pet") && argc >= 5) {
		int t = atoi(argv[2]), iv = atoi(argv[3]), n = atoi(argv[4]);
		if (t <= 0 || iv <= 0 || n <= 0 || iv >= t) { fprintf(stderr, "need 0 < interval < timeout, n > 0\n"); return 1; }
		int fd = opendev(argc > 5 ? argv[5] : "/dev/watchdog");
		int got = settimeout(fd, t);
		if (got < 0) { magic_close(fd); return 4; }
		if (got <= iv) { printf("A6L_WDT driver timeout %d <= interval %d: would bite\n", got, iv); magic_close(fd); return 4; }
		show(fd);
		for (int i = 1; i <= n; i++) {
			if (write(fd, "", 1) != 1) { printf("A6L_WDT ping %d failed: %s\n", i, strerror(errno)); magic_close(fd); return 5; }
			printf("A6L_WDT ping %d/%d\n", i, n); fflush(stdout);
			sleep(iv);
		}
		magic_close(fd); kmsg("PET_DONE"); return 0;
	}
	if (!strcmp(cmd, "bite") && argc >= 3) {
		int t = atoi(argv[2]); char m[128];
		if (t <= 0) return 1;
		int fd = opendev(argc > 3 ? argv[3] : "/dev/watchdog");
		int got = settimeout(fd, t);
		if (got < 0) { magic_close(fd); return 4; }
		snprintf(m, sizeof(m), "BITE_ARMED timeout=%d: no more pings, the board must reset in ~%d s", got, got);
		kmsg(m); sync();
		for (int s = 0;; s++) {   /* never close: an unexpected close keeps the hardware running anyway */
			sleep(1);
			snprintf(m, sizeof(m), "still alive %d s after arming", s + 1); kmsg(m);
		}
	}
	fprintf(stderr, "bad arguments\n"); return 1;
}
