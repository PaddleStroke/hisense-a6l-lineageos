// SPDX-License-Identifier: GPL-2.0
/*
 * A6L kernel-gaps QEMU functional test: static /init for an initramfs booted with the A6L V67 or r5 Image
 * (tests/qemu/run-qemu-gaps.sh). Loads the modules in /m and checks the netd / BatteryStats / storaged / vold contracts:
 *  xt_quota2     : iptables "-m quota2 ! --quota N --name X -j DROP" (raw IPT_SO_SET_REPLACE, as netd's iptables does),
 *                  packets pass until the quota is used, then DROP; NETLINK_NFLOG group 1 alert (nlmsg_type 112,
 *                  libsysutils ulog_packet_msg_t, prefix = name, outdev = lo); /proc/net/xt_quota/<name> read/write;
 *                  rule removal frees the counter; rmmod.
 *  uid_sys_stats : dead-task accounting through the sched_process_exit probe (kCFI-checked indirect call) under
 *                  fork/exit load with concurrent readers; show_uid_stat / uid_io stats / procstat / remove_uid_range; rmmod.
 *  dm-default-key: (if /m/dm-default-key.ko) vold v2 table "aes-xts-plain64 <key> 0 <loop> 0 3 allow_discards
 *                  sector_size:4096 iv_large_sectors" on a loop device; data written through it reads back through a
 *                  dm-crypt table with the same key (on-disk format = dm-crypt aes-xts-plain64, 4K, large IVs), raw
 *                  != plaintext; status line; teardown; rmmod.
 * Prints "A6L_GAPS_QEMU PASS" or "A6L_GAPS_QEMU FAIL <n>" then powers off.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <linux/netlink.h>
#include <linux/netfilter_ipv4/ip_tables.h>
#include <linux/dm-ioctl.h>
#include <linux/loop.h>
#include <linux/fs.h>

static int fails;
#define CHECK(c, ...) do { if (c) { printf("ok   "); } else { printf("FAIL "); fails++; } printf(__VA_ARGS__); printf("\n"); fflush(stdout); } while (0)

static int load(const char *p)
{
	int fd = open(p, O_RDONLY | O_CLOEXEC), r;
	if (fd < 0) return -errno;
	r = syscall(SYS_finit_module, fd, "", 0);
	r = r ? -errno : 0; close(fd); return r;
}
static int unload(const char *n) { return syscall(SYS_delete_module, n, O_NONBLOCK) ? -errno : 0; }
static int exists(const char *p) { struct stat st; return stat(p, &st) == 0; }
static ssize_t rd(const char *p, char *b, size_t n)
{
	int fd = open(p, O_RDONLY); ssize_t t = 0, r;
	if (fd < 0) return -errno;
	while (t < (ssize_t)n - 1 && (r = read(fd, b + t, n - 1 - t)) > 0) t += r;
	b[t < 0 ? 0 : t] = 0; close(fd); return t;
}
static int wr(const char *p, const char *s)
{
	int fd = open(p, O_WRONLY), r;
	if (fd < 0) return -errno;
	r = write(fd, s, strlen(s)); r = r < 0 ? -errno : 0; close(fd); return r;
}

/* ---------------- xt_quota2 ---------------- */
struct a6l_quota_mtinfo2 { char name[15]; uint8_t flags; uint64_t quota __attribute__((aligned(8))); void *master __attribute__((aligned(8))); };
typedef struct { unsigned long mark; long ts, tus; unsigned int hook; char in[IFNAMSIZ], out[IFNAMSIZ]; size_t len; char prefix[32];
		 unsigned char mac_len, mac[80]; unsigned char payload[]; } ulog_t;
_Static_assert(sizeof(ulog_t) == 192, "libsysutils ulog_packet_msg_t");
_Static_assert(sizeof(struct a6l_quota_mtinfo2) == 32, "xt_quota_mtinfo2");

#define STD_SZ (sizeof(struct ipt_entry) + XT_ALIGN(sizeof(struct xt_standard_target)))
#define QM_SZ XT_ALIGN(sizeof(struct xt_entry_match) + sizeof(struct a6l_quota_mtinfo2))
#define ERR_SZ (sizeof(struct ipt_entry) + XT_ALIGN(sizeof(struct xt_error_target)))

static unsigned char *put_std(unsigned char *p, size_t match_sz, int verdict)
{
	struct ipt_entry *e = (void *)p;
	struct xt_standard_target *t = (void *)(p + sizeof(*e) + match_sz);
	memset(e, 0, sizeof(*e) + match_sz + XT_ALIGN(sizeof(*t)));
	e->target_offset = sizeof(*e) + match_sz;
	e->next_offset = e->target_offset + XT_ALIGN(sizeof(*t));
	t->target.u.target_size = XT_ALIGN(sizeof(*t));
	t->verdict = verdict;
	return p + e->next_offset;
}

/* filter table: INPUT/FORWARD policy ACCEPT; OUTPUT [quota rule if name] + policy ACCEPT */
static int set_filter(int s, const char *name, uint64_t quota)
{
	struct ipt_getinfo info = { .name = "filter" };
	socklen_t l = sizeof(info);
	if (getsockopt(s, SOL_IP, IPT_SO_GET_INFO, &info, &l)) return -errno;
	size_t qsz = name ? sizeof(struct ipt_entry) + QM_SZ + XT_ALIGN(sizeof(struct xt_standard_target)) : 0;
	size_t size = 3 * STD_SZ + qsz + ERR_SZ;
	struct ipt_replace *r = calloc(1, sizeof(*r) + size);
	struct xt_counters *cnt = calloc(info.num_entries, sizeof(*cnt));
	strcpy(r->name, "filter"); r->valid_hooks = info.valid_hooks; r->num_entries = name ? 5 : 4; r->size = size;
	r->num_counters = info.num_entries; r->counters = cnt;
	unsigned char *p = (unsigned char *)r->entries, *b = p;
	r->hook_entry[NF_INET_LOCAL_IN] = r->underflow[NF_INET_LOCAL_IN] = p - b; p = put_std(p, 0, -NF_ACCEPT - 1);
	r->hook_entry[NF_INET_FORWARD] = r->underflow[NF_INET_FORWARD] = p - b; p = put_std(p, 0, -NF_ACCEPT - 1);
	r->hook_entry[NF_INET_LOCAL_OUT] = p - b;
	if (name) {
		struct xt_entry_match *m = (void *)(p + sizeof(struct ipt_entry));
		unsigned char *n = put_std(p, QM_SZ, -NF_DROP - 1);
		struct a6l_quota_mtinfo2 *q = (void *)m->data;
		m->u.match_size = QM_SZ; strcpy(m->u.user.name, "quota2"); m->u.user.revision = 3;
		strncpy(q->name, name, sizeof(q->name) - 1); q->flags = 1 /* XT_QUOTA_INVERT: "! --quota" */; q->quota = quota;
		p = n;
	}
	r->underflow[NF_INET_LOCAL_OUT] = p - b; p = put_std(p, 0, -NF_ACCEPT - 1);
	{
		struct ipt_entry *e = (void *)p; struct xt_error_target *t = (void *)(p + sizeof(*e));
		e->target_offset = sizeof(*e); e->next_offset = ERR_SZ;
		t->target.u.user.target_size = XT_ALIGN(sizeof(*t)); strcpy(t->target.u.user.name, XT_ERROR_TARGET);
		strcpy(t->errorname, "ERROR"); p += ERR_SZ;
	}
	int ret = setsockopt(s, SOL_IP, IPT_SO_SET_REPLACE, r, sizeof(*r) + size) ? -errno : 0;
	free(r); free(cnt); return ret;
}

static int udp_send(int u, int len)
{
	char buf[2048] = {0};
	struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(9), .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
	return sendto(u, buf, len, 0, (void *)&a, sizeof(a)) == len ? 0 : -errno;
}

static void test_quota2(void)
{
	char b[256]; int r;
	printf("== xt_quota2\n");
	r = load("/m/xt_quota2.ko"); CHECK(r == 0, "insmod xt_quota2 (%d)", r); if (r) return;
	CHECK(exists("/proc/net/xt_quota"), "/proc/net/xt_quota exists");
	int s = socket(AF_INET, SOCK_RAW, IPPROTO_RAW), u = socket(AF_INET, SOCK_DGRAM, 0);
	struct ifreq ifr = { .ifr_name = "lo" };
	ioctl(u, SIOCGIFFLAGS, &ifr); ifr.ifr_flags |= IFF_UP; CHECK(ioctl(u, SIOCSIFFLAGS, &ifr) == 0, "lo up");
	int nl = socket(AF_NETLINK, SOCK_DGRAM, NETLINK_NFLOG);
	struct sockaddr_nl sa = { .nl_family = AF_NETLINK, .nl_groups = 1 /* netd NFLOG_QUOTA_GROUP */ };
	CHECK(nl >= 0 && bind(nl, (void *)&sa, sizeof(sa)) == 0, "NETLINK_NFLOG group 1 socket (%s)", strerror(errno));
	r = set_filter(s, "a6ltest", 1000); CHECK(r == 0, "IPT_SO_SET_REPLACE with -m quota2 ! --quota 1000 --name a6ltest -j DROP (%d)", r);
	rd("/proc/net/xt_quota/a6ltest", b, sizeof(b)); CHECK(!strcmp(b, "1000\n"), "proc counter = 1000 (%s)", b);
	/* a listener on port 9: otherwise every datagram also triggers an ICMP port-unreachable that goes through OUTPUT */
	int sink = socket(AF_INET, SOCK_DGRAM, 0);
	struct sockaddr_in sa9 = { .sin_family = AF_INET, .sin_port = htons(9), .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
	CHECK(bind(sink, (void *)&sa9, sizeof(sa9)) == 0, "udp sink on 127.0.0.1:9");
	int ok = 0; for (int i = 0; i < 7; i++) ok += udp_send(u, 100) == 0;
	CHECK(ok == 7, "7 x 128-byte packets pass (%d)", ok);
	rd("/proc/net/xt_quota/a6ltest", b, sizeof(b)); CHECK(!strcmp(b, "104\n"), "counter 1000-7*128 = 104 (%s)", b);
	r = udp_send(u, 100); CHECK(r == -EPERM, "8th packet dropped (quota exhausted) (%d)", r);
	rd("/proc/net/xt_quota/a6ltest", b, sizeof(b)); CHECK(!strcmp(b, "0\n"), "counter 0 (%s)", b);
	char m[4096]; ssize_t n = recv(nl, m, sizeof(m), MSG_DONTWAIT);
	struct nlmsghdr *h = (void *)m; ulog_t *pm = NLMSG_DATA(h);
	CHECK(n >= (ssize_t)NLMSG_SPACE(sizeof(ulog_t)) && h->nlmsg_type == 112 && !strcmp(pm->prefix, "a6ltest") && !strcmp(pm->out, "lo"),
	      "NFLOG alert: len %zd type %d prefix '%s' out '%s'", n, n > 0 ? h->nlmsg_type : -1, n > 0 ? pm->prefix : "", n > 0 ? pm->out : "");
	n = recv(nl, m, sizeof(m), MSG_DONTWAIT); r = udp_send(u, 100);
	CHECK(n < 0 && r == -EPERM && recv(nl, m, sizeof(m), MSG_DONTWAIT) < 0, "alert sent once only, still dropping");
	CHECK(wr("/proc/net/xt_quota/a6ltest", "5000\n") == 0, "netd-style proc write 5000");
	rd("/proc/net/xt_quota/a6ltest", b, sizeof(b)); CHECK(!strcmp(b, "5000\n"), "counter 5000 (%s)", b);
	CHECK(udp_send(u, 100) == 0, "traffic passes again after the quota write");
	CHECK(wr("/proc/net/xt_quota/a6ltest", "junk") == -EINVAL, "garbage write rejected");
	r = set_filter(s, NULL, 0); CHECK(r == 0, "rule removed (%d)", r);
	CHECK(!exists("/proc/net/xt_quota/a6ltest"), "counter proc entry freed with the last rule");
	close(s); close(u); close(nl); close(sink);
	r = unload("xt_quota2"); CHECK(r == 0, "rmmod xt_quota2 (%d)", r);
}

/* ---------------- uid_sys_stats ---------------- */
static void child_work(uid_t uid, int ms, int bytes)
{
	if (setresgid(uid, uid, uid) || setresuid(uid, uid, uid)) _exit(2);
	struct timespec t0, t; clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t0);
	volatile unsigned long x = 0;
	do { for (int i = 0; i < 100000; i++) x += i; clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t); }
	while ((t.tv_sec - t0.tv_sec) * 1000 + (t.tv_nsec - t0.tv_nsec) / 1000000 < ms);
	char path[64], buf[4096] = {1}; snprintf(path, sizeof(path), "/tmp/w%d", getpid());
	int fd = open(path, O_CREAT | O_WRONLY, 0600);
	for (int w = 0; w < bytes; w += sizeof(buf)) if (write(fd, buf, sizeof(buf)) < 0) break;
	close(fd); unlink(path); _exit(0);
}
static int line_for(const char *file, const char *prefix, char *out, size_t n)
{
	static char b[1 << 16]; rd(file, b, sizeof(b));
	for (char *l = strtok(b, "\n"); l; l = strtok(NULL, "\n"))
		if (!strncmp(l, prefix, strlen(prefix))) { snprintf(out, n, "%s", l); return 1; }
	return 0;
}
static void test_uid(void)
{
	char l[512]; int r;
	printf("== uid_sys_stats\n");
	r = load("/m/uid_sys_stats.ko"); CHECK(r == 0, "insmod uid_sys_stats (%d)", r); if (r) return;
	pid_t p = fork(); if (!p) child_work(1234, 300, 256 * 1024); waitpid(p, NULL, 0);
	unsigned long long ut = 0, st = 0;
	int have = line_for("/proc/uid_cputime/show_uid_stat", "1234:", l, sizeof(l));
	if (have) sscanf(l, "1234: %llu %llu", &ut, &st);
	CHECK(have && ut + st >= 250000, "dead task cputime accounted: '%s'", have ? l : "(none)");
	unsigned long long v[10] = {0};
	have = line_for("/proc/uid_io/stats", "1234 ", l, sizeof(l));
	if (have) sscanf(l, "1234 %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7], &v[8], &v[9]);
	CHECK(have && v[1] >= 256 * 1024, "dead task io (fg wchar) accounted: '%s'", have ? l : "(none)");
	CHECK(wr("/proc/uid_procstat/set", "1234 1") == 0, "procstat 1234 -> background");
	CHECK(wr("/proc/uid_procstat/set", "1234 7") == -EINVAL, "procstat bad state rejected");
	p = fork(); if (!p) child_work(1234, 50, 128 * 1024); waitpid(p, NULL, 0);
	have = line_for("/proc/uid_io/stats", "1234 ", l, sizeof(l));
	if (have) sscanf(l, "1234 %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7], &v[8], &v[9]);
	CHECK(have && v[5] >= 128 * 1024 && v[1] >= 256 * 1024 && v[1] < 384 * 1024, "background bucket gets the new io: '%s'", l);
	/* stress: 400 short-lived tasks of 8 uids with 2 concurrent readers (trylock/deferral path) */
	pid_t rdr[2];
	for (int i = 0; i < 2; i++) if (!(rdr[i] = fork())) { char x[64]; for (;;) { line_for("/proc/uid_cputime/show_uid_stat", "x", x, 64); line_for("/proc/uid_io/stats", "x", x, 64); } }
	for (int i = 0; i < 400; i++) { p = fork(); if (!p) child_work(20000 + i % 8, 1, 4096); waitpid(p, NULL, 0); }
	for (int i = 0; i < 2; i++) { kill(rdr[i], 9); waitpid(rdr[i], NULL, 0); }
	sleep(1);
	int seen = 0; for (int i = 0; i < 8; i++) { char pf[16]; snprintf(pf, sizeof(pf), "%d ", 20000 + i); seen += line_for("/proc/uid_io/stats", pf, l, sizeof(l)); }
	CHECK(seen == 8, "stress: 400 exits of 8 uids with concurrent readers, all 8 uids listed (%d)", seen);
	CHECK(wr("/proc/uid_cputime/remove_uid_range", "5-1") == -EINVAL, "bad remove range rejected");
	CHECK(wr("/proc/uid_cputime/remove_uid_range", "1234-1234") == 0 && !line_for("/proc/uid_cputime/show_uid_stat", "1234:", l, sizeof(l)),
	      "remove_uid_range 1234-1234 drops the uid");
	r = unload("uid_sys_stats"); CHECK(r == 0 && !exists("/proc/uid_cputime"), "rmmod uid_sys_stats (%d)", r);
	p = fork(); if (!p) _exit(0); waitpid(p, NULL, 0); CHECK(1, "task exit after rmmod (probe unregistered)");
}

/* ---------------- dm-default-key ---------------- */
static const char KEY[] = "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f"
			  "f0e1d2c3b4a5968778695a4b3c2d1e0fffeeddccbbaa99887766554433221100";
static int dm(int ctl, unsigned cmd, const char *name, const char *type, const char *params, uint64_t len, struct dm_ioctl *out, char *status, size_t sn)
{
	char buf[16384] = {0}; struct dm_ioctl *io = (void *)buf;
	io->version[0] = 4; io->data_size = sizeof(buf); io->data_start = sizeof(*io);
	snprintf(io->name, sizeof(io->name), "%s", name);
	if (type) {
		struct dm_target_spec *t = (void *)(buf + io->data_start);
		io->target_count = 1; t->sector_start = 0; t->length = len; snprintf(t->target_type, sizeof(t->target_type), "%s", type);
		strcpy((char *)(t + 1), params);
	}
	if (cmd == DM_TABLE_STATUS) io->flags = DM_STATUS_TABLE_FLAG;
	if (ioctl(ctl, cmd, io)) return -errno;
	if (out) *out = *io;
	if (status && io->target_count) { struct dm_target_spec *t = (void *)(buf + io->data_start); snprintf(status, sn, "%s", (char *)(t + 1)); }
	return 0;
}
static void test_dmdk(void)
{
	int r; char tbl[512], st[512]; struct dm_ioctl io;
	if (!exists("/m/dm-default-key.ko")) { printf("== dm-default-key: not in this kernel set (V67 has no BLK_INLINE_ENCRYPTION), skipped\n"); return; }
	printf("== dm-default-key\n");
	r = load("/m/dm-default-key.ko"); CHECK(r == 0, "insmod dm-default-key (%d)", r); if (r) return;
	const size_t SZ = 8 << 20; const uint64_t SECT = SZ / 512;
	int f = open("/tmp/disk", O_CREAT | O_RDWR, 0600); CHECK(ftruncate(f, SZ) == 0, "8 MiB backing file");
	int lc = open("/dev/loop-control", O_RDWR); int ln = ioctl(lc, LOOP_CTL_GET_FREE);
	char lp[32]; snprintf(lp, sizeof(lp), "/dev/loop%d", ln);
	int lf = open(lp, O_RDWR); CHECK(lf >= 0 && ioctl(lf, LOOP_SET_FD, f) == 0, "loop %s on tmpfs file", lp);
	struct stat lst; fstat(lf, &lst);
	int ctl = open("/dev/mapper/control", O_RDWR); CHECK(ctl >= 0, "/dev/mapper/control");
	snprintf(tbl, sizeof(tbl), "aes-xts-plain64 %s 0 %u:%u 0 3 allow_discards sector_size:4096 iv_large_sectors", KEY, major(lst.st_rdev), minor(lst.st_rdev));
	r = dm(ctl, DM_DEV_CREATE, "dk", NULL, NULL, 0, &io, NULL, 0); CHECK(r == 0, "DM_DEV_CREATE dk (%d)", r);
	r = dm(ctl, DM_TABLE_LOAD, "dk", "default-key", tbl, SECT, NULL, NULL, 0); CHECK(r == 0, "DM_TABLE_LOAD default-key (vold v2 options) (%d)", r);
	r = dm(ctl, DM_DEV_SUSPEND, "dk", NULL, NULL, 0, &io, NULL, 0); CHECK(r == 0, "resume dk (%d)", r);
	r = dm(ctl, DM_TABLE_STATUS, "dk", NULL, NULL, 0, NULL, st, sizeof(st));
	char exp[256]; snprintf(exp, sizeof(exp), "aes-xts-plain64 - 0 %u:%u 0 3 allow_discards sector_size:4096 iv_large_sectors", major(lst.st_rdev), minor(lst.st_rdev));
	CHECK(r == 0 && !strcmp(st, exp), "table status '%s' (key omitted)", st);
	char dp[32]; snprintf(dp, sizeof(dp), "/dev/dm-%u", minor(io.dev));
	unsigned char *pat = malloc(SZ), *got = malloc(SZ);
	for (size_t i = 0; i < SZ; i++) pat[i] = (unsigned char)(i * 131 + (i >> 12));
	int d = open(dp, O_RDWR); ssize_t w = pwrite(d, pat, SZ, 0); fsync(d); close(d);
	CHECK(w == (ssize_t)SZ, "write 8 MiB through %s (%zd)", dp, w);
	CHECK(pread(f, got, SZ, 0) == (ssize_t)SZ && memcmp(got, pat, 4096) && memcmp(got + SZ - 4096, pat + SZ - 4096, 4096), "backing data is encrypted");
	snprintf(tbl, sizeof(tbl), "aes-xts-plain64 %s 0 %u:%u 0 2 sector_size:4096 iv_large_sectors", KEY, major(lst.st_rdev), minor(lst.st_rdev));
	struct dm_ioctl io2;
	r = dm(ctl, DM_DEV_CREATE, "dc", NULL, NULL, 0, &io2, NULL, 0) || dm(ctl, DM_TABLE_LOAD, "dc", "crypt", tbl, SECT, NULL, NULL, 0) ||
	    dm(ctl, DM_DEV_SUSPEND, "dc", NULL, NULL, 0, &io2, NULL, 0);
	CHECK(r == 0, "dm-crypt 'dc' with the same key (%d)", r);
	snprintf(dp, sizeof(dp), "/dev/dm-%u", minor(io2.dev));
	d = open(dp, O_RDONLY); memset(got, 0, SZ); ssize_t g = pread(d, got, SZ, 0); close(d);
	size_t bad = 0, first = SZ; for (size_t i = 0; i < SZ; i += 512) if (memcmp(got + i, pat + i, 512)) { bad++; if (first == SZ) first = i; }
	CHECK(g == (ssize_t)SZ && !bad, "dm-crypt reads back the plaintext: on-disk format = aes-xts-plain64/4K/iv_large_sectors (bad 512B blocks %zu, first at %zu)", bad, first);
	snprintf(dp, sizeof(dp), "/dev/dm-%u", minor(io.dev));
	d = open(dp, O_RDONLY | O_DIRECT); unsigned char *al = aligned_alloc(4096, SZ); g = pread(d, al, SZ, 0); close(d);
	CHECK(g == (ssize_t)SZ && !memcmp(al, pat, SZ), "default-key O_DIRECT read back = plaintext (%zd %s)", g, g < 0 ? strerror(errno) : "");
	d = open(dp, O_RDONLY); ioctl(d, BLKFLSBUF, 0); memset(got, 0, SZ); g = pread(d, got, SZ, 0); close(d);
	CHECK(g == (ssize_t)SZ && !memcmp(got, pat, SZ), "default-key buffered read back after BLKFLSBUF = plaintext");
	free(al);
	r = dm(ctl, DM_DEV_REMOVE, "dc", NULL, NULL, 0, NULL, NULL, 0) | dm(ctl, DM_DEV_REMOVE, "dk", NULL, NULL, 0, NULL, NULL, 0);
	CHECK(r == 0, "remove dm devices");
	snprintf(tbl, sizeof(tbl), "aes-xts-plain64 %s 0 %u:%u 0 1 sector_size:4096", KEY, major(lst.st_rdev), minor(lst.st_rdev));
	dm(ctl, DM_DEV_CREATE, "bad", NULL, NULL, 0, NULL, NULL, 0);
	CHECK(dm(ctl, DM_TABLE_LOAD, "bad", "default-key", tbl, SECT, NULL, NULL, 0) == -EINVAL, "sector_size without iv_large_sectors rejected");
	dm(ctl, DM_DEV_REMOVE, "bad", NULL, NULL, 0, NULL, NULL, 0);
	ioctl(lf, LOOP_CLR_FD, 0); close(lf); close(ctl);
	r = unload("dm_default_key"); CHECK(r == 0, "rmmod dm-default-key (%d)", r);
}

int main(void)
{
	mount("proc", "/proc", "proc", 0, NULL); mount("sysfs", "/sys", "sysfs", 0, NULL);
	mount("devtmpfs", "/dev", "devtmpfs", 0, NULL); mount("tmpfs", "/tmp", "tmpfs", 0, "size=64m");
	setvbuf(stdout, NULL, _IOLBF, 0);
	char b[256]; rd("/proc/version", b, sizeof(b)); printf("A6L_GAPS_QEMU start: %s", b);
	test_quota2(); test_uid(); test_dmdk();
	rd("/proc/sys/kernel/tainted", b, sizeof(b)); printf("tainted=%s", b);
	printf(fails ? "A6L_GAPS_QEMU FAIL %d\n" : "A6L_GAPS_QEMU PASS\n", fails); fflush(stdout);
	sync(); reboot(RB_POWER_OFF); return 0;
}
