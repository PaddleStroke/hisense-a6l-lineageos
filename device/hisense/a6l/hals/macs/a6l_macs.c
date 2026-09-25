// SPDX-License-Identifier: Apache-2.0
// a6l_macs: apply the Hisense A6L factory Wi-Fi MAC (and a Bluetooth public address) on a mainline kernel.
//
// Stock stores the WLAN MAC as text in /persist/wlan_mac.bin ("Intf0MacAddress=XXXXXXXXXXXX", written by
// diag_ext/ftmdaemon). Mainline ath10k_snoc has no source for it (no DT local-mac-address, ABL adds none), so it
// picks a random address. This tool reads the persist data READ-ONLY (a mounted wlan_mac.bin, or a raw scan of the
// persist partition, never mounting and never writing it) and:
//   wlan: waits for the netdev, and sets the address with SIOCSIFHWADDR only while the interface is DOWN.
//   bt:   WCN3990 comes up with the QCA default BD address, so the kernel marks hci0 "unconfigured" and the AOSP
//         Bluetooth HAL (which lists only configured controllers) never finds it. Send MGMT SET_PUBLIC_ADDRESS
//         (what `btmgmt public-addr` does) so the controller becomes configured before the HAL starts.
// Usage:
//   a6l_macs show [-p SRC]                      print the persist WLAN MAC and the derived BT address
//   a6l_macs wlan [-p SRC] [-i wlan0] [-w SEC] [-n]
//   a6l_macs bt   [-p SRC] [-a xx:xx:..] [-x 0] [-w SEC] [-n]
//   SRC = file or block device (default: /mnt/vendor/persist/wlan_mac.bin, then /dev/block/by-name/persist)
//   -n  dry run (parse and report only).  Exit codes: 0 ok/applied, 1 usage, 2 no MAC, 3 iface/controller missing,
//       4 iface busy (up), 5 apply failed.
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <net/if_arp.h>

#define TAG "A6L_MACS "
static const char *kDefaultSrc[] = {"/mnt/vendor/persist/wlan_mac.bin", "/persist/wlan_mac.bin",
                                    "/dev/block/by-name/persist", NULL};
static const char kKey[] = "Intf0MacAddress=";
#define SCAN_LIMIT (64u << 20) /* persist is 32 MB on the A6L */

static int hexval(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int valid_unicast(const uint8_t m[6]) {
    static const uint8_t zero[6], ff[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    if (!memcmp(m, zero, 6) || !memcmp(m, ff, 6)) return 0;
    if (m[0] & 1) return 0; /* multicast */
    /* WCNSS_qcom_cfg.ini placeholder */
    static const uint8_t ph[6] = {0x00, 0x0a, 0xf5, 0x89, 0x89, 0xff};
    if (!memcmp(m, ph, 6)) return 0;
    return 1;
}

/* Streaming scan for kKey followed by 12 hex digits. Handles matches across buffer boundaries. */
static int find_mac(const char *path, uint8_t mac[6]) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -errno;
    enum { BUF = 1 << 16, KEEP = sizeof(kKey) - 1 + 12 };
    static char buf[BUF + KEEP];
    size_t have = 0, total = 0;
    int found = 0;
    for (;;) {
        ssize_t n = read(fd, buf + have, BUF);
        if (n <= 0) break;
        total += (size_t)n;
        size_t len = have + (size_t)n;
        for (size_t i = 0; i + KEEP <= len; i++) {
            if (buf[i] != 'I' || memcmp(buf + i, kKey, sizeof(kKey) - 1)) continue;
            const char *h = buf + i + sizeof(kKey) - 1;
            int ok = 1;
            for (int k = 0; k < 6 && ok; k++) {
                int a = hexval(h[2 * k]), b = hexval(h[2 * k + 1]);
                if (a < 0 || b < 0) ok = 0; else mac[k] = (uint8_t)(a << 4 | b);
            }
            if (ok && valid_unicast(mac)) { found = 1; break; }
        }
        if (found || total >= SCAN_LIMIT) break;
        have = len < KEEP ? len : KEEP;
        memmove(buf, buf + len - have, have);
    }
    close(fd);
    return found ? 0 : -ENOENT;
}

static int load_mac(const char *src, uint8_t mac[6], const char **used) {
    if (src) { *used = src; return find_mac(src, mac); }
    for (int i = 0; kDefaultSrc[i]; i++) {
        if (find_mac(kDefaultSrc[i], mac) == 0) { *used = kDefaultSrc[i]; return 0; }
    }
    *used = "(none)";
    return -ENOENT;
}

static int parse_mac(const char *s, uint8_t m[6]) {
    unsigned v[6];
    if (sscanf(s, "%2x:%2x:%2x:%2x:%2x:%2x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6) return -1;
    for (int i = 0; i < 6; i++) m[i] = (uint8_t)v[i];
    return valid_unicast(m) ? 0 : -1;
}

/* BT address = WLAN MAC + 1 (Qualcomm factory convention; not verified against stock NV 447 on this unit). */
static void derive_bt(const uint8_t w[6], uint8_t b[6]) {
    memcpy(b, w, 6);
    for (int i = 5; i >= 3; i--) { if (++b[i] != 0) break; } /* carry within the NIC part only */
}

static void fmt(const uint8_t m[6], char out[18]) {
    snprintf(out, 18, "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
}

static int wait_path(const char *p, int secs) {
    struct stat st;
    for (int t = 0; t <= secs * 10; t++) {
        if (stat(p, &st) == 0) return 0;
        if (t < secs * 10) usleep(100000);
    }
    return -1;
}

static int do_wlan(const uint8_t mac[6], const char *ifn, int wait, int dry) {
    char p[128], s[18];
    snprintf(p, sizeof(p), "/sys/class/net/%s", ifn);
    if (wait_path(p, wait) < 0) { printf(TAG "wlan %s missing after %ds\n", ifn, wait); return 3; }
    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) { perror(TAG "socket"); return 5; }
    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, ifn, IFNAMSIZ - 1);
    if (ioctl(fd, SIOCGIFHWADDR, &ifr) == 0) {
        uint8_t cur[6]; memcpy(cur, ifr.ifr_hwaddr.sa_data, 6); fmt(cur, s);
        printf(TAG "wlan %s current %s\n", ifn, s);
        if (!memcmp(cur, mac, 6)) { printf(TAG "wlan already set\n"); close(fd); return 0; }
    }
    if (ioctl(fd, SIOCGIFFLAGS, &ifr) == 0 && (ifr.ifr_flags & IFF_UP)) {
        printf(TAG "wlan %s is UP; not touching it (the Wi-Fi HAL owns it now)\n", ifn); close(fd); return 4;
    }
    fmt(mac, s);
    if (dry) { printf(TAG "wlan dry-run would set %s\n", s); close(fd); return 0; }
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, ifn, IFNAMSIZ - 1);
    ifr.ifr_hwaddr.sa_family = ARPHRD_ETHER;
    memcpy(ifr.ifr_hwaddr.sa_data, mac, 6);
    if (ioctl(fd, SIOCSIFHWADDR, &ifr) < 0) { printf(TAG "wlan set %s failed: %s\n", s, strerror(errno)); close(fd); return 5; }
    printf(TAG "wlan %s set %s\n", ifn, s);
    close(fd);
    return 0;
}

/* ---- Bluetooth management socket (see linux include/net/bluetooth/mgmt.h) ---- */
#ifndef AF_BLUETOOTH
#define AF_BLUETOOTH 31
#endif
#define BTPROTO_HCI 1
#define HCI_CHANNEL_CONTROL 3
#define HCI_DEV_NONE 0xffff
#define MGMT_OP_READ_INDEX_LIST 0x0003
#define MGMT_OP_READ_INFO 0x0004
#define MGMT_OP_READ_UNCONF_INDEX_LIST 0x0036
#define MGMT_OP_READ_CONFIG_INFO 0x0037
#define MGMT_OP_SET_PUBLIC_ADDRESS 0x0039
#define MGMT_EV_CMD_COMPLETE 0x0001
#define MGMT_EV_CMD_STATUS 0x0002
struct sockaddr_hci { sa_family_t hci_family; unsigned short hci_dev, hci_channel; };
struct mgmt_hdr { uint16_t opcode, index, len; } __attribute__((packed));

static int mgmt_cmd(int fd, uint16_t op, uint16_t idx, const void *p, uint16_t plen, uint8_t *rsp, int *rlen,
                    uint8_t *status) {
    uint8_t b[512];
    struct mgmt_hdr h = {op, idx, plen};
    memcpy(b, &h, sizeof(h));
    if (plen) memcpy(b + sizeof(h), p, plen);
    if (write(fd, b, sizeof(h) + plen) < 0) return -errno;
    for (int tries = 0; tries < 50; tries++) {
        struct pollfd pfd = {fd, POLLIN, 0};
        if (poll(&pfd, 1, 3000) <= 0) return -ETIMEDOUT;
        ssize_t n = read(fd, b, sizeof(b));
        if (n < (ssize_t)sizeof(h)) continue;
        struct mgmt_hdr e; memcpy(&e, b, sizeof(e));
        if ((e.opcode != MGMT_EV_CMD_COMPLETE && e.opcode != MGMT_EV_CMD_STATUS) || n < (ssize_t)sizeof(h) + 3) continue;
        uint16_t eop; memcpy(&eop, b + sizeof(h), 2);
        if (eop != op) continue;
        *status = b[sizeof(h) + 2];
        int dl = (int)n - (int)sizeof(h) - 3;
        if (rsp && rlen) { if (dl > *rlen) dl = *rlen; memcpy(rsp, b + sizeof(h) + 3, dl > 0 ? dl : 0); *rlen = dl; }
        return 0;
    }
    return -EIO;
}

static int index_in(int fd, uint16_t op, uint16_t want) {
    uint8_t r[256]; int rl = sizeof(r); uint8_t st = 0xff;
    if (mgmt_cmd(fd, op, HCI_DEV_NONE, NULL, 0, r, &rl, &st) < 0 || st || rl < 2) return 0;
    uint16_t n; memcpy(&n, r, 2);
    for (int i = 0; i < n && 2 + 2 * i + 1 < rl; i++) { uint16_t x; memcpy(&x, r + 2 + 2 * i, 2); if (x == want) return 1; }
    return 0;
}

static int do_bt(const uint8_t addr[6], uint16_t idx, int wait, int dry) {
    char s[18];
    fmt(addr, s);
    int fd = socket(AF_BLUETOOTH, SOCK_RAW | SOCK_CLOEXEC, BTPROTO_HCI);
    if (fd < 0) { printf(TAG "bt socket: %s (bluetooth.ko loaded? CAP_NET_ADMIN?)\n", strerror(errno)); return 5; }
    struct sockaddr_hci a = {AF_BLUETOOTH, HCI_DEV_NONE, HCI_CHANNEL_CONTROL};
    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) < 0) { printf(TAG "bt bind control: %s\n", strerror(errno)); close(fd); return 5; }
    int conf = 0, unconf = 0;
    for (int t = 0; t <= wait * 2; t++) {
        conf = index_in(fd, MGMT_OP_READ_INDEX_LIST, idx);
        unconf = index_in(fd, MGMT_OP_READ_UNCONF_INDEX_LIST, idx);
        if (conf || unconf) break;
        if (t < wait * 2) usleep(500000);
    }
    if (!conf && !unconf) { printf(TAG "bt hci%u not present after %ds\n", idx, wait); close(fd); return 3; }
    printf(TAG "bt hci%u %s\n", idx, unconf ? "UNCONFIGURED (public address missing)" : "configured");
    if (conf) {
        uint8_t r[280]; int rl = sizeof(r); uint8_t st = 0xff;
        if (mgmt_cmd(fd, MGMT_OP_READ_INFO, idx, NULL, 0, r, &rl, &st) == 0 && !st && rl >= 6) {
            char c[18]; uint8_t be[6]; for (int i = 0; i < 6; i++) be[i] = r[5 - i]; fmt(be, c);
            printf(TAG "bt hci%u current address %s\n", idx, c);
            if (!memcmp(be, addr, 6)) { printf(TAG "bt already set\n"); close(fd); return 0; }
        }
    }
    if (dry) { printf(TAG "bt dry-run would set %s\n", s); close(fd); return 0; }
    uint8_t le[6]; for (int i = 0; i < 6; i++) le[i] = addr[5 - i];
    uint8_t st = 0xff;
    int rc = mgmt_cmd(fd, MGMT_OP_SET_PUBLIC_ADDRESS, idx, le, 6, NULL, NULL, &st);
    close(fd);
    if (rc < 0 || st) { printf(TAG "bt set %s failed rc=%d mgmt_status=0x%02x\n", s, rc, st); return 5; }
    printf(TAG "bt hci%u set %s\n", idx, s);
    return 0;
}

static void usage(void) {
    fprintf(stderr, "usage: a6l_macs show|wlan|bt [-p SRC] [-i IFACE] [-a BDADDR] [-x HCI] [-w SEC] [-n]\n");
}

int main(int argc, char **argv) {
    if (argc < 2) { usage(); return 1; }
    const char *cmd = argv[1], *src = NULL, *ifn = "wlan0", *bdaddr = NULL;
    int wait = 0, dry = 0; unsigned idx = 0;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "-n")) dry = 1;
        else if (i + 1 < argc && !strcmp(argv[i], "-p")) src = argv[++i];
        else if (i + 1 < argc && !strcmp(argv[i], "-i")) ifn = argv[++i];
        else if (i + 1 < argc && !strcmp(argv[i], "-a")) bdaddr = argv[++i];
        else if (i + 1 < argc && !strcmp(argv[i], "-x")) idx = (unsigned)atoi(argv[++i]);
        else if (i + 1 < argc && !strcmp(argv[i], "-w")) wait = atoi(argv[++i]);
        else { usage(); return 1; }
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    uint8_t w[6], b[6]; char s[18];
    const char *used;
    int have = load_mac(src, w, &used) == 0;
    if (have) { fmt(w, s); printf(TAG "persist %s wlan %s\n", used, s); }
    else printf(TAG "persist no Intf0MacAddress found (src %s)\n", src ? src : "defaults");
    if (!strcmp(cmd, "show")) {
        if (!have) return 2;
        derive_bt(w, b); fmt(b, s); printf(TAG "derived bt %s\n", s);
        return 0;
    }
    if (!strcmp(cmd, "wlan")) return have ? do_wlan(w, ifn, wait, dry) : 2;
    if (!strcmp(cmd, "bt")) {
        if (bdaddr && *bdaddr) { if (parse_mac(bdaddr, b)) { printf(TAG "bad -a %s\n", bdaddr); return 1; } }
        else if (have) derive_bt(w, b);
        else return 2;
        return do_bt(b, (uint16_t)idx, wait, dry);
    }
    usage();
    return 1;
}
