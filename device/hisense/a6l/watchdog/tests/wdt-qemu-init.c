// SPDX-License-Identifier: GPL-2.0-only
// wdt-qemu-init (cpufreq-watchdog agent, 29 Sep 2026): /init of the QEMU watchdog test initramfs (test-watchdog-qemu.sh).
// Loads qcom-wdt.ko (must load on this kernel: vermagic + modversions CRCs) and softdog.ko (stand-in device: QEMU virt has
// no APSS watchdog), then drives a6l_wdtctl through: info, the watchdogd contract (SETTIMEOUT 30 = 10 + 20), a short pet
// run with magic close, and finally a bite that must reset the VM (-no-reboot makes QEMU exit).
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

static int load(const char *path, const char *params) {
	int fd = open(path, O_RDONLY | O_CLOEXEC), r;
	if (fd < 0) { printf("A6L_QEMU_WDT open %s failed\n", path); return -1; }
	r = syscall(SYS_finit_module, fd, params, 0); close(fd);
	printf("A6L_QEMU_WDT insmod %s %s\n", path, r ? "FAILED" : "ok"); fflush(stdout);
	return r;
}
static int run(char *const argv[]) {
	pid_t p = fork(); int st = 0;
	if (!p) { execv(argv[0], argv); _exit(127); }
	waitpid(p, &st, 0);
	printf("A6L_QEMU_WDT %s %s rc=%d\n", argv[1], argv[2] ? argv[2] : "", WIFEXITED(st) ? WEXITSTATUS(st) : -1); fflush(stdout);
	return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}
int main(void) {
	mount("proc", "/proc", "proc", 0, NULL); mount("sysfs", "/sys", "sysfs", 0, NULL); mount("devtmpfs", "/dev", "devtmpfs", 0, NULL);
	int bad = 0;
	if (load("/qcom-wdt.ko", "")) bad = 1;
	struct stat s;
	printf("A6L_QEMU_WDT qcom_wdt driver %s\n", stat("/sys/bus/platform/drivers/qcom_wdt", &s) ? "MISSING" : "registered");
	if (load("/softdog.ko", "soft_margin=60")) bad = 1;
	char *info[] = {"/a6l_wdtctl", "info", NULL, NULL};
	char *contract[] = {"/a6l_wdtctl", "pet", "30", "10", "1", NULL};   /* watchdogd 10 20 -> SETTIMEOUT 30 */
	char *pet[] = {"/a6l_wdtctl", "pet", "4", "1", "6", NULL};          /* 6 pings at 1 s with a 4 s timeout: no reset */
	char *bite[] = {"/a6l_wdtctl", "bite", "3", NULL};
	if (run(info)) bad = 1;
	if (run(contract)) bad = 1;
	if (run(pet)) bad = 1;
	printf("A6L_QEMU_WDT survived the pet run%s\n", bad ? " (with errors)" : ""); fflush(stdout);
	if (bad) { printf("A6L_QEMU_WDT FAIL before bite\n"); fflush(stdout); sync(); syscall(SYS_reboot, 0xfee1dead, 672274793, 0x4321fedc, NULL); }
	run(bite);   /* never returns if the watchdog works */
	printf("A6L_QEMU_WDT FAIL bite returned\n"); fflush(stdout);
	for (;;) pause();
}
