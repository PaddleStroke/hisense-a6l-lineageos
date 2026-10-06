// a6l_i2cprobe: READ-ONLY I2C register read (stk agent, 25 Sep 2026).
// usage: a6l_i2cprobe /dev/i2c-N ADDR REG [COUNT]   (hex ADDR/REG) -> prints "OK addr reg: bytes" or "NAK addr: errno"
// One combined write(reg)+read transaction (I2C_RDWR); never writes a register value.
#include <errno.h>
#include <fcntl.h>
#include <linux/i2c.h>
#include <linux/i2c-dev.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

int main(int argc, char **argv)
{
	if (argc < 4) {
		fprintf(stderr, "usage: %s /dev/i2c-N addr reg [count]\n", argv[0]);
		return 2;
	}
	unsigned addr = strtoul(argv[2], NULL, 16), reg = strtoul(argv[3], NULL, 16);
	int n = argc > 4 ? atoi(argv[4]) : 1;
	if (addr > 0x7f || reg > 0xff || n < 1 || n > 32)
		return 2;
	int fd = open(argv[1], O_RDWR);
	if (fd < 0) {
		printf("ERR open %s: %s\n", argv[1], strerror(errno));
		return 1;
	}
	unsigned char r = reg, buf[32];
	struct i2c_msg m[2] = {
		{ .addr = addr, .flags = 0, .len = 1, .buf = &r },
		{ .addr = addr, .flags = I2C_M_RD, .len = n, .buf = buf },
	};
	struct i2c_rdwr_ioctl_data x = { .msgs = m, .nmsgs = 2 };
	if (ioctl(fd, I2C_RDWR, &x) < 0) {
		printf("NAK 0x%02x reg 0x%02x: %s\n", addr, reg, strerror(errno));
		close(fd);
		return 1;
	}
	printf("OK 0x%02x reg 0x%02x:", addr, reg);
	for (int i = 0; i < n; i++)
		printf(" %02x", buf[i]);
	printf("\n");
	close(fd);
	return 0;
}
