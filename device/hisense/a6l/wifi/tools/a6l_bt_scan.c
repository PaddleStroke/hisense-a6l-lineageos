// SPDX-License-Identifier: GPL-2.0
/*
 * a6l-bt-scan: Bluetooth discovery through the kernel management socket (no bluetoothd, no BlueZ libs).
 * wifi agent, 26 Sep 2026. Static (NDK r27c). For the V74/V75 test recovery after run-bt.sh (hci0 present).
 *
 *   a6l-bt-scan [-x HCI] [-t SECONDS] [-a BDADDR] [-k]
 *     -x  controller index (default 0)
 *     -t  discovery time in seconds (default 15); discovery is restarted when the kernel ends a round
 *     -a  public address to set if the controller is UNCONFIGURED (WCN3990 reports the QCA default address,
 *         btqca sets HCI_QUIRK_USE_BDADDR_PROPERTY, the kernel keeps hci0 unconfigured until MGMT SET_PUBLIC_ADDRESS).
 *         The address goes to the controller's RAM (qca_set_bdaddr, EDL_WRITE_BD_ADDR); nothing is stored.
 *     -k  keep the controller powered at the end (default: power off)
 * Steps: [unconfigured -> SET_PUBLIC_ADDRESS] -> SET_LE 1 -> SET_BREDR 1 -> SET_POWERED 1 -> START_DISCOVERY 0x07
 * (BR/EDR inquiry + LE public + LE random) -> DEVICE_FOUND events -> STOP -> power off. Prints one line per device.
 * Lines: A6L_BT_* ; A6L_BT_SCAN_PASS n=<devices> when at least one device was found. Exit 0 pass, 1 no device, 2+ error.
 * Build with -DA6L_BT_SELFTEST for a host self-test of the event parser (no Bluetooth socket needed).
 */
#include <errno.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#ifndef AF_BLUETOOTH
#define AF_BLUETOOTH 31
#endif
#define BTPROTO_HCI 1
#define HCI_DEV_NONE 0xffff
#define HCI_CHANNEL_CONTROL 3

#define MGMT_OP_READ_INDEX_LIST 0x0003
#define MGMT_OP_READ_INFO 0x0004
#define MGMT_OP_SET_POWERED 0x0005
#define MGMT_OP_SET_LE 0x000D
#define MGMT_OP_START_DISCOVERY 0x0023
#define MGMT_OP_STOP_DISCOVERY 0x0024
#define MGMT_OP_SET_BREDR 0x002A
#define MGMT_OP_READ_UNCONF_INDEX_LIST 0x0036
#define MGMT_OP_SET_PUBLIC_ADDRESS 0x0039
#define MGMT_EV_CMD_COMPLETE 0x0001
#define MGMT_EV_CMD_STATUS 0x0002
#define MGMT_EV_INDEX_ADDED 0x0004
#define MGMT_EV_DEVICE_FOUND 0x0012
#define MGMT_EV_DISCOVERING 0x0013

struct sockaddr_hci { sa_family_t hci_family; unsigned short hci_dev, hci_channel; };
struct mgmt_hdr { uint16_t opcode, index, len; } __attribute__((packed));

#define MAXDEV 128
struct dev { uint8_t a[6]; uint8_t type; int8_t rssi; int seen; char name[64]; uint32_t cod; int has_cod; uint16_t company; int has_company; };
static struct dev devs[MAXDEV];
static int ndev;
static int discovering = -1;
static uint16_t g_idx;

static __attribute__((unused)) double now_s(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }
static void fmt_le(const uint8_t *le, char *s) { sprintf(s, "%02X:%02X:%02X:%02X:%02X:%02X", le[5], le[4], le[3], le[2], le[1], le[0]); }
static const char *atype(uint8_t t) { return t == 0 ? "BR/EDR" : t == 1 ? "LE-public" : t == 2 ? "LE-random" : "?"; }

static void clean_name(char *dst, const uint8_t *src, int n, int cap)
{
	int j = 0;
	for (int i = 0; i < n && j < cap - 1; i++) {
		uint8_t c = src[i];
		if (!c) break;
		dst[j++] = (c >= 0x20 && c < 0x7f) ? (char)c : (c >= 0x80 ? (char)c : '.');
	}
	dst[j] = 0;
}

/* DEVICE_FOUND payload: addr[6] type[1] rssi[1] flags[4] eir_len[2] eir[] */
static void on_device_found(const uint8_t *p, int n)
{
	if (n < 14) return;
	uint16_t el = p[12] | p[13] << 8;
	if (14 + el > n) el = n - 14;
	struct dev *d = NULL;
	for (int i = 0; i < ndev; i++) if (!memcmp(devs[i].a, p, 6) && devs[i].type == p[6]) d = &devs[i];
	int is_new = !d;
	if (!d) { if (ndev >= MAXDEV) return; d = &devs[ndev++]; memset(d, 0, sizeof(*d)); memcpy(d->a, p, 6); d->type = p[6]; }
	d->rssi = (int8_t)p[7]; d->seen++;
	const uint8_t *e = p + 14;
	for (int o = 0; o + 1 < el;) {
		int fl = e[o];
		if (!fl || o + 1 + fl > el) break;
		int t = e[o + 1]; const uint8_t *v = e + o + 2; int vl = fl - 1;
		if ((t == 0x09 || (t == 0x08 && !d->name[0])) && vl > 0) clean_name(d->name, v, vl, sizeof(d->name));
		if (t == 0x0D && vl >= 3) { d->cod = v[0] | v[1] << 8 | v[2] << 16; d->has_cod = 1; }
		if (t == 0xFF && vl >= 2) { d->company = v[0] | v[1] << 8; d->has_company = 1; }
		o += 1 + fl;
	}
	if (is_new) {
		char s[18]; fmt_le(d->a, s);
		printf("A6L_BT_FOUND %s %s rssi=%d%s%s\n", s, atype(d->type), d->rssi, d->name[0] ? " name=" : "", d->name);
	}
}

static void on_event(const uint8_t *b, int n)
{
	struct mgmt_hdr h;
	if (n < (int)sizeof(h)) return;
	memcpy(&h, b, sizeof(h));
	const uint8_t *p = b + sizeof(h); int pl = n - (int)sizeof(h);
	if (h.index != g_idx && h.index != HCI_DEV_NONE) return;
	if (h.opcode == MGMT_EV_DEVICE_FOUND) on_device_found(p, pl);
	else if (h.opcode == MGMT_EV_DISCOVERING && pl >= 2) { discovering = p[1]; printf("A6L_BT_DISCOVERING type=0x%02x %d\n", p[0], p[1]); }
	else if (h.opcode == MGMT_EV_INDEX_ADDED) printf("A6L_BT_INDEX_ADDED hci%u\n", h.index);
}

#ifndef A6L_BT_SELFTEST
static int mgmt_cmd(int fd, uint16_t op, uint16_t idx, const void *prm, uint16_t plen, uint8_t *rsp, int *rlen, uint8_t *status)
{
	uint8_t b[1024];
	struct mgmt_hdr h = { op, idx, plen };
	memcpy(b, &h, sizeof(h));
	if (plen) memcpy(b + sizeof(h), prm, plen);
	if (write(fd, b, sizeof(h) + plen) < 0) return -errno;
	double end = now_s() + 5;
	while (now_s() < end) {
		struct pollfd pfd = { fd, POLLIN, 0 };
		if (poll(&pfd, 1, 500) <= 0) continue;
		ssize_t n = read(fd, b, sizeof(b));
		if (n < (ssize_t)sizeof(h)) continue;
		struct mgmt_hdr e; memcpy(&e, b, sizeof(e));
		if (e.opcode != MGMT_EV_CMD_COMPLETE && e.opcode != MGMT_EV_CMD_STATUS) { on_event(b, (int)n); continue; }
		if (n < (ssize_t)sizeof(h) + 3) continue;
		uint16_t eop = b[6] | b[7] << 8;
		if (eop != op) continue;
		*status = b[8];
		int dl = (int)n - (int)sizeof(h) - 3;
		if (rsp && rlen) { if (dl > *rlen) dl = *rlen; if (dl > 0) memcpy(rsp, b + 9, dl); *rlen = dl > 0 ? dl : 0; }
		return 0;
	}
	return -ETIMEDOUT;
}

static int index_in(int fd, uint16_t op, uint16_t want)
{
	uint8_t r[256]; int rl = sizeof(r); uint8_t st = 0xff;
	if (mgmt_cmd(fd, op, HCI_DEV_NONE, NULL, 0, r, &rl, &st) < 0 || st || rl < 2) return 0;
	int n = r[0] | r[1] << 8;
	for (int i = 0; i < n && 3 + 2 * i < rl; i++) if ((r[2 + 2 * i] | r[3 + 2 * i] << 8) == want) return 1;
	return 0;
}

static int set1(int fd, uint16_t op, uint8_t v, const char *what)
{
	uint8_t st = 0xff, r[8]; int rl = sizeof(r);
	int rc = mgmt_cmd(fd, op, g_idx, &v, 1, r, &rl, &st);
	uint32_t cur = rl >= 4 ? (uint32_t)(r[0] | r[1] << 8 | r[2] << 16 | (uint32_t)r[3] << 24) : 0;
	printf("A6L_BT_SET %s=%u rc=%d status=0x%02x settings=0x%08x\n", what, v, rc, st, cur);
	return rc < 0 ? rc : st;
}

static int parse_mac(const char *s, uint8_t le[6])
{
	unsigned v[6];
	if (sscanf(s, "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6) return -1;
	for (int i = 0; i < 6; i++) { if (v[i] > 255) return -1; le[5 - i] = (uint8_t)v[i]; }
	return 0;
}

static void pump(int fd, double secs)
{
	uint8_t b[1024];
	double end = now_s() + secs;
	while (now_s() < end) {
		struct pollfd pfd = { fd, POLLIN, 0 };
		int ms = (int)((end - now_s()) * 1000); if (ms < 1) ms = 1;
		if (poll(&pfd, 1, ms) <= 0) continue;
		ssize_t n = read(fd, b, sizeof(b));
		if (n > 0) on_event(b, (int)n);
	}
}

int main(int argc, char **argv)
{
	int secs = 15, keep = 0; const char *addr = NULL; unsigned idx = 0;
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-k")) keep = 1;
		else if (i + 1 < argc && !strcmp(argv[i], "-x")) idx = (unsigned)atoi(argv[++i]);
		else if (i + 1 < argc && !strcmp(argv[i], "-t")) secs = atoi(argv[++i]);
		else if (i + 1 < argc && !strcmp(argv[i], "-a")) addr = argv[++i];
		else { fprintf(stderr, "usage: a6l-bt-scan [-x HCI] [-t SECONDS] [-a BDADDR] [-k]\n"); return 2; }
	}
	if (secs < 1 || secs > 120) secs = 15;
	g_idx = (uint16_t)idx;
	setvbuf(stdout, NULL, _IOLBF, 0);
	int fd = socket(AF_BLUETOOTH, SOCK_RAW | SOCK_CLOEXEC, BTPROTO_HCI);
	if (fd < 0) { printf("A6L_BT_FAIL socket: %s (bluetooth.ko loaded?)\n", strerror(errno)); return 3; }
	struct sockaddr_hci sa = { AF_BLUETOOTH, HCI_DEV_NONE, HCI_CHANNEL_CONTROL };
	if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) { printf("A6L_BT_FAIL bind control: %s\n", strerror(errno)); return 3; }

	int conf = index_in(fd, MGMT_OP_READ_INDEX_LIST, g_idx), unconf = index_in(fd, MGMT_OP_READ_UNCONF_INDEX_LIST, g_idx);
	printf("A6L_BT_INDEX hci%u %s\n", idx, conf ? "configured" : unconf ? "UNCONFIGURED" : "absent");
	if (!conf && !unconf) return 4;
	if (!conf) {
		uint8_t le[6], st = 0xff;
		if (!addr || parse_mac(addr, le)) { printf("A6L_BT_FAIL hci%u unconfigured and no valid -a BDADDR\n", idx); return 4; }
		int rc = mgmt_cmd(fd, MGMT_OP_SET_PUBLIC_ADDRESS, g_idx, le, 6, NULL, NULL, &st);
		printf("A6L_BT_PUBLIC_ADDR %s rc=%d status=0x%02x\n", addr, rc, st);
		if (rc < 0 || st) return 4;
		for (int t = 0; t < 20 && !(conf = index_in(fd, MGMT_OP_READ_INDEX_LIST, g_idx)); t++) pump(fd, 0.5);
		printf("A6L_BT_INDEX hci%u %s after SET_PUBLIC_ADDRESS\n", idx, conf ? "configured" : "still unconfigured");
		if (!conf) return 4;
	}
	uint8_t r[300]; int rl = sizeof(r); uint8_t st = 0xff;
	if (mgmt_cmd(fd, MGMT_OP_READ_INFO, g_idx, NULL, 0, r, &rl, &st) == 0 && !st && rl >= 20) {
		char s[18]; fmt_le(r, s);
		uint32_t sup = r[9] | r[10] << 8 | r[11] << 16 | (uint32_t)r[12] << 24, cur = r[13] | r[14] << 8 | r[15] << 16 | (uint32_t)r[16] << 24;
		char nm[32] = ""; if (rl > 20) clean_name(nm, r + 20, rl - 20 < 31 ? rl - 20 : 31, sizeof(nm));
		printf("A6L_BT_INFO addr=%s hci_ver=%u manufacturer=%u supported=0x%08x current=0x%08x name=%s\n", s, r[6], r[7] | r[8] << 8, sup, cur, nm);
	} else printf("A6L_BT_INFO read failed status=0x%02x\n", st);

	set1(fd, MGMT_OP_SET_LE, 1, "le");
	set1(fd, MGMT_OP_SET_BREDR, 1, "bredr");
	if (set1(fd, MGMT_OP_SET_POWERED, 1, "powered")) { printf("A6L_BT_FAIL power on\n"); return 5; }

	uint8_t type = 0x07;
	double end = now_s() + secs;
	int rounds = 0;
	while (now_s() < end - 1) {
		st = 0xff;
		int rc = mgmt_cmd(fd, MGMT_OP_START_DISCOVERY, g_idx, &type, 1, NULL, NULL, &st);
		if ((rc < 0 || st) && type == 0x07) { printf("A6L_BT_DISCOVERY type 0x07 status=0x%02x rc=%d -> BR/EDR only\n", st, rc); type = 0x01; continue; }
		if (rc < 0 || st) { printf("A6L_BT_FAIL start discovery rc=%d status=0x%02x\n", rc, st); break; }
		rounds++; discovering = 1;
		while (now_s() < end && discovering != 0) pump(fd, 0.5);
	}
	if (discovering == 1) { st = 0xff; mgmt_cmd(fd, MGMT_OP_STOP_DISCOVERY, g_idx, &type, 1, NULL, NULL, &st); }
	printf("A6L_BT_DEVICES %d (rounds=%d, %ds, type=0x%02x)\n", ndev, rounds, secs, type);
	for (int i = 0; i < ndev; i++) {
		char s[18]; fmt_le(devs[i].a, s);
		printf("A6L_BT_DEV %s %-9s rssi=%4d seen=%d", s, atype(devs[i].type), devs[i].rssi, devs[i].seen);
		if (devs[i].has_cod) printf(" class=0x%06x", devs[i].cod);
		if (devs[i].has_company) printf(" company=0x%04x", devs[i].company);
		printf("%s%s\n", devs[i].name[0] ? " name=" : "", devs[i].name);
	}
	if (!keep) set1(fd, MGMT_OP_SET_POWERED, 0, "powered");
	close(fd);
	printf(ndev ? "A6L_BT_SCAN_PASS n=%d\n" : "A6L_BT_SCAN_NONE n=%d\n", ndev);
	return ndev ? 0 : 1;
}
#else
/* host self-test: feed canned mgmt events through the parser */
int main(void)
{
	g_idx = 0;
	uint8_t ev[64]; int n = 0;
	struct mgmt_hdr h = { MGMT_EV_DEVICE_FOUND, 0, 0 };
	uint8_t pl[] = { 0x46,0x40,0x99,0x7b,0xb3,0x7c, 0x00, (uint8_t)-60, 0,0,0,0,
		/* eir_len (set below) */ 0, 0, 4, 0x0D, 0x0c, 0x02, 0x5a, 10, 0x09, 'P','i','x','e','l',' ','9','x','x' };
	pl[12] = (uint8_t)(sizeof(pl) - 14);
	h.len = sizeof(pl); memcpy(ev, &h, 6); memcpy(ev + 6, pl, sizeof(pl)); n = 6 + sizeof(pl);
	on_event(ev, n); on_event(ev, n);
	uint8_t d[] = { 0x13,0x00,0x00,0x00,0x02,0x00, 0x07, 0x00 }; on_event(d, sizeof(d));
	/* truncated EIR must not crash */
	ev[6 + 12] = 200; on_event(ev, n);
	printf("selftest ndev=%d seen=%d name=%s cod=0x%06x discovering=%d\n", ndev, devs[0].seen, devs[0].name, devs[0].cod, discovering);
	return !(ndev == 1 && devs[0].seen == 3 && !strcmp(devs[0].name, "Pixel 9xx") && devs[0].cod == 0x5a020c && discovering == 0);
}
#endif
