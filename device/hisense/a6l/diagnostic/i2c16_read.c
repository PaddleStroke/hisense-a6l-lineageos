// SPDX-License-Identifier: Apache-2.0
/* A6L: READ-ONLY dump of 16-bit-addressed, 32-bit little-endian registers (Toshiba TC358762 style) over i2c-dev.
 * usage: a6l_i2c16_read /dev/i2c-N <hexaddr7> <hexreg> [<hexreg> ...]      No write path exists in this tool. */
#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <linux/i2c.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
int main(int argc, char **argv) {
    if (argc < 4) return 2;
    int fd = open(argv[1], O_RDWR); if (fd < 0) { perror("i2c"); return 1; }
    unsigned addr = strtoul(argv[2], 0, 16); if (addr < 0x08 || addr > 0x77) return 2;
    for (int i = 3; i < argc; i++) {
        unsigned reg = strtoul(argv[i], 0, 16); uint8_t a[2] = {reg >> 8, reg & 0xff}, v[4] = {0};
        struct i2c_msg m[2] = {{addr, 0, 2, a}, {addr, I2C_M_RD, 4, v}}; struct i2c_rdwr_ioctl_data d = {m, 2};
        if (ioctl(fd, I2C_RDWR, &d) < 0) printf("%04x: NAK\n", reg); else printf("%04x: %02x%02x%02x%02x\n", reg, v[3], v[2], v[1], v[0]);
    }
    return 0;
}
