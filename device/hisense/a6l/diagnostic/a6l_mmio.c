// SPDX-License-Identifier: Apache-2.0
// A6L bring-up: /dev/mem register reader. Reads: any 4-byte aligned address inside the allowlisted
// MMSS clock controller / MDSS windows. Writes: MMCC only, and only with A6L_MMIO_WRITE=1 in the environment.
// Usage: a6l_mmio r <hexaddr> [count]   |   a6l_mmio w <hexaddr> <hexval>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
static const struct { uint64_t base, size; int writable; } win[] = {
    { 0x0c8c0000, 0x40000, 1 },  /* MMCC */
    { 0x0c828014, 0x4, 1 },      /* MMSS_MISC DSI ULPS clamp / PHY reset control (stock: qcom,mmss-ulp-clamp-ctrl-offset = 0x14) */
    { 0x0c828000, 0xac, 0 },     /* MMSS_MISC, read-only */
    { 0x0c901004, 0x4, 1 },      /* MDP DISP_INTF_SEL only (experiment: DPU never programs it; INTF2 -> DSI) */
    { 0x0c900000, 0xb0000, 0 },  /* MDSS: MDP, DSI0/1, PHYs */
    { 0x00100000, 0x94000, 0 },  /* GCC */
};
/* r5 bug hunt round2 kernel-drivers (29 Sep 2026): strict hex/decimal parsing (a typo such as "w <addr> 0xzz" used to
 * write 0 to a live clock register; "1ffffffff" was truncated), and an overflow-safe window check (addr + 4*n could wrap
 * past 2^64 and pass). A6L_MMIO_CHECK_ONLY=1 validates the arguments and stops before /dev/mem (host tests). */
static int parse_u64(const char *s, int base, uint64_t max, uint64_t *out) {
    char *end; unsigned long long v;
    if (!s || !*s || *s == '-' || *s == '+' || *s == ' ') return -1;
    errno = 0; v = strtoull(s, &end, base);
    if (errno || *end || v > max) return -1;
    *out = v; return 0;
}
int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: %s r <addr> [count] | w <addr> <val>\n", argv[0]); return 2; }
    if (strcmp(argv[1], "r") && strcmp(argv[1], "w")) { fprintf(stderr, "mode must be r or w\n"); return 2; }
    int wr = argv[1][0] == 'w';
    uint64_t addr, n = 1, v = 0;
    if ((wr && argc != 4) || (!wr && argc > 4)) { fprintf(stderr, "usage: %s r <addr> [count] | w <addr> <val>\n", argv[0]); return 2; }
    if (parse_u64(argv[2], 16, UINT64_MAX, &addr)) { fprintf(stderr, "bad address\n"); return 2; }
    if (!wr && argc > 3 && parse_u64(argv[3], 0, 4096, &n)) { fprintf(stderr, "bad count\n"); return 2; }
    if (wr && parse_u64(argv[3], 16, UINT32_MAX, &v)) { fprintf(stderr, "bad value (hex, at most 32 bits)\n"); return 2; }
    if ((addr & 3) || n == 0 || n > 4096) { fprintf(stderr, "bad address/count\n"); return 2; }
    int ok = 0;
    for (unsigned i = 0; i < sizeof win / sizeof *win; i++)
        if (addr >= win[i].base && addr - win[i].base <= win[i].size && 4 * n <= win[i].size - (addr - win[i].base) &&
            (!wr || win[i].writable)) ok = 1;
    if (!ok) { fprintf(stderr, "address outside allowlist\n"); return 3; }
    if (wr) { const char *e = getenv("A6L_MMIO_WRITE"); if (!e || strcmp(e, "1")) { fprintf(stderr, "writes need A6L_MMIO_WRITE=1\n"); return 3; } }
    { const char *c = getenv("A6L_MMIO_CHECK_ONLY");
      if (c && !strcmp(c, "1")) { printf("CHECK_OK %c %08llx n=%llu v=%08llx\n", wr ? 'w' : 'r', (unsigned long long)addr, (unsigned long long)n, (unsigned long long)v); return 0; } }
    int fd = open("/dev/mem", (wr ? O_RDWR : O_RDONLY) | O_SYNC);
    if (fd < 0) { perror("/dev/mem"); return 1; }
    uint64_t page = addr & ~0xfffULL, span = ((addr + 4 * n - page) + 0xfff) & ~0xfffULL;
    volatile uint32_t *m = mmap(0, span, wr ? PROT_READ | PROT_WRITE : PROT_READ, MAP_SHARED, fd, page);
    if (m == MAP_FAILED) { perror("mmap"); return 1; }
    volatile uint32_t *p = (volatile uint32_t *)((volatile char *)m + (addr - page));
    if (wr) { printf("%08llx: %08x -> ", (unsigned long long)addr, p[0]); p[0] = (uint32_t)v; printf("%08x (readback %08x)\n", (uint32_t)v, p[0]); }
    else for (uint64_t i = 0; i < n; i++) printf("%08llx: %08x\n", (unsigned long long)(addr + 4 * i), p[i]);
    return 0;
}
