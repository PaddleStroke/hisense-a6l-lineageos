// SPDX-License-Identifier: Apache-2.0
/* A6L e-ink bridge over I2C, replaying EXACTLY what the stock kernel does (dsi2dpi_init @ffffff80084c2ea0):
 *   tc358762_send_init_cmd (12 writes @0x0b) -> tc358762_read_id (reg 0x04a0) -> tc358767 table (25 writes @0x0f) if the id says so.
 * Stock calls this from mdss_dsi_on AFTER: GPIOs (42/45/56), DSI clocks, bridge reset, clock lane forced HS, tps65185 active.
 * Writes are limited to the two fixed stock tables; there is no arbitrary write path.
 * usage: a6l_dsi2dpi_init /dev/i2c-N id | dump | init762 | init767 */
#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <linux/i2c.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
static int fd;
struct rv { uint16_t reg; uint32_t val; };
static const struct rv t762[] = {{0x047c, 0}, {0x0210, 7}, {0x0164, 4}, {0x0168, 4}, {0x0144, 0}, {0x0148, 0}, {0x0114, 3}, {0x0450, 0x60}, {0x0420, 0x150}, {0x0464, 0x205}, {0x0104, 1}, {0x0204, 1}};
static const struct rv t767[] = {{0x0448, 0x86}, {0x06a0, 0x3080}, {0x0914, 0x11c201}, {0x0904, 0}, {0x0904, 4}, {0x0908, 1}, {0x0908, 5}, {0x0918, 0x110}, {0x0800, 0x1100}, {0x0800, 0x1100},
    {0x013c, 0x30005}, {0x0114, 3}, {0x0164, 4}, {0x0168, 4}, {0x016c, 4}, {0x0170, 4}, {0x0134, 7}, {0x0210, 7}, {0x0104, 1}, {0x0204, 1}, {0x0450, 0x03f00100}, {0x0454, 0x007d0005}, {0x045c, 0x00040002}, {0x0464, 1}, {0x0510, 1}};
static int rd(unsigned addr, unsigned reg, uint32_t *out) {
    uint8_t a[2] = {reg >> 8, reg & 0xff}, v[4] = {0}; struct i2c_msg m[2] = {{addr, 0, 2, a}, {addr, I2C_M_RD, 4, v}}; struct i2c_rdwr_ioctl_data d = {m, 2};
    if (ioctl(fd, I2C_RDWR, &d) < 0) return -1; *out = v[0] | v[1] << 8 | v[2] << 16 | (uint32_t)v[3] << 24; return 0;
}
static int wr(unsigned addr, unsigned reg, uint32_t val) {
    uint8_t b[6] = {reg >> 8, reg & 0xff, val, val >> 8, val >> 16, val >> 24}; struct i2c_msg m = {addr, 0, 6, b}; struct i2c_rdwr_ioctl_data d = {&m, 1};
    return ioctl(fd, I2C_RDWR, &d) < 0 ? -1 : 0;
}
static void table(unsigned addr, const struct rv *t, unsigned n) {
    for (unsigned i = 0; i < n; i++) { int r = wr(addr, t[i].reg, t[i].val); uint32_t back = 0; int rr = rd(addr, t[i].reg, &back);
        printf("A6L_D2D w %02x %04x=%08x %s readback=%s%08x\n", addr, t[i].reg, t[i].val, r ? "NAK" : "ok", rr ? "NAK " : "", back); }
}
int main(int argc, char **argv) {
    if (argc < 3) return 2; fd = open(argv[1], O_RDWR); if (fd < 0) { perror("i2c"); return 1; }
    if (!strcmp(argv[2], "id") || !strcmp(argv[2], "dump")) {
        for (unsigned a = 0x0b; a <= 0x0f; a += 4) { uint32_t v; if (rd(a, 0x04a0, &v)) printf("A6L_D2D addr %02x: NAK\n", a); else { printf("A6L_D2D addr %02x: IDREG=%08x\n", a, v);
            if (!strcmp(argv[2], "dump")) { static const uint16_t regs[] = {0x047c, 0x0210, 0x0164, 0x0168, 0x0114, 0x0450, 0x0420, 0x0424, 0x0428, 0x042c, 0x0464, 0x0104, 0x0204, 0x0214, 0x0218, 0x0300, 0x0044};
                for (unsigned i = 0; i < sizeof regs / sizeof *regs; i++) { uint32_t x; if (rd(a, regs[i], &x)) printf("  %04x: NAK\n", regs[i]); else printf("  %04x: %08x\n", regs[i], x); } } } }
        return 0;
    }
    if (!strcmp(argv[2], "r8") && argc == 5) {   /* read-only: 8-bit register dump, r8 <hexaddr> <count> */
        unsigned x = strtoul(argv[3], 0, 16), n = strtoul(argv[4], 0, 0); printf("A6L_D2D r8 %02x:", x);
        for (unsigned r = 0; r < n && r < 256; r++) { uint8_t reg = r, v = 0; struct i2c_msg m[2] = {{x, 0, 1, &reg}, {x, I2C_M_RD, 1, &v}}; struct i2c_rdwr_ioctl_data d = {m, 2}; if (ioctl(fd, I2C_RDWR, &d) < 0) printf(" --"); else printf(" %02x", v); } printf("\n"); return 0; }
    if (!strcmp(argv[2], "scan")) {   /* read-only presence scan: one 1-byte read per address */
        printf("A6L_D2D scan ack:"); for (unsigned x = 0x08; x <= 0x77; x++) { uint8_t v; struct i2c_msg m = {x, I2C_M_RD, 1, &v}; struct i2c_rdwr_ioctl_data d = {&m, 1}; if (ioctl(fd, I2C_RDWR, &d) >= 0) printf(" %02x", x); } printf("\n"); return 0; }
    if (!strcmp(argv[2], "init762")) { table(0x0b, t762, sizeof t762 / sizeof *t762); return 0; }
    if (!strcmp(argv[2], "init767")) { table(0x0f, t767, sizeof t767 / sizeof *t767); return 0; }
    return 2;
}
