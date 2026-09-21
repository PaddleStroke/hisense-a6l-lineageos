// SPDX-License-Identifier: Apache-2.0
// A6L bring-up: /dev/mem register reader. Reads: any 4-byte aligned address inside the allowlisted
// MMSS clock controller / MDSS windows. Writes: MMCC only, and only with A6L_MMIO_WRITE=1 in the environment.
// Usage: a6l_mmio r <hexaddr> [count]   |   a6l_mmio w <hexaddr> <hexval>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
static const struct { uint64_t base, size; int writable; } win[] = {
    { 0x0c8c0000, 0x40000, 1 },  /* MMCC */
    { 0x0c900000, 0xb0000, 0 },  /* MDSS: MDP, DSI0/1, PHYs */
    { 0x00100000, 0x94000, 0 },  /* GCC */
};
int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: %s r <addr> [count] | w <addr> <val>\n", argv[0]); return 2; }
    int wr = argv[1][0] == 'w';
    uint64_t addr = strtoull(argv[2], 0, 16);
    unsigned long n = (!wr && argc > 3) ? strtoul(argv[3], 0, 0) : 1;
    if (wr && argc < 4) return 2;
    if ((addr & 3) || n == 0 || n > 4096) { fprintf(stderr, "bad address/count\n"); return 2; }
    int ok = 0;
    for (unsigned i = 0; i < sizeof win / sizeof *win; i++)
        if (addr >= win[i].base && addr + 4 * n <= win[i].base + win[i].size && (!wr || win[i].writable)) ok = 1;
    if (!ok) { fprintf(stderr, "address outside allowlist\n"); return 3; }
    if (wr) { const char *e = getenv("A6L_MMIO_WRITE"); if (!e || strcmp(e, "1")) { fprintf(stderr, "writes need A6L_MMIO_WRITE=1\n"); return 3; } }
    int fd = open("/dev/mem", (wr ? O_RDWR : O_RDONLY) | O_SYNC);
    if (fd < 0) { perror("/dev/mem"); return 1; }
    uint64_t page = addr & ~0xfffULL, span = ((addr + 4 * n - page) + 0xfff) & ~0xfffULL;
    volatile uint32_t *m = mmap(0, span, wr ? PROT_READ | PROT_WRITE : PROT_READ, MAP_SHARED, fd, page);
    if (m == MAP_FAILED) { perror("mmap"); return 1; }
    volatile uint32_t *p = (volatile uint32_t *)((volatile char *)m + (addr - page));
    if (wr) { uint32_t v = strtoul(argv[3], 0, 16); printf("%08llx: %08x -> ", (unsigned long long)addr, p[0]); p[0] = v; printf("%08x (readback %08x)\n", v, p[0]); }
    else for (unsigned long i = 0; i < n; i++) printf("%08llx: %08x\n", (unsigned long long)(addr + 4 * i), p[i]);
    return 0;
}
