// SPDX-License-Identifier: GPL-2.0
/*
 * a6l_afotp: READ-ONLY dump of the Hisense A6L main camera module EEPROM and decode of the AF calibration
 * (camera phase 3). Layout from stock vendor/lib/libmmcamera_imx576_hmct_eeprom.so (camera_config.xml
 * EepromName imx576_hmct):
 *   slave 0xB0 (8-bit) = 0x58 on CCI0, 16-bit word addresses, two reads concatenated into one buffer:
 *   0x0000 x 0x0B05 bytes, then 0x1800 x 0x0422 bytes (buffer 0x0B05..0x0F26).
 *   AF block (imx576_hmct_eeprom_format_afdata): 0x708 flag (1 = valid), 0x709 macro DAC (big endian 16),
 *   0x70B infinity DAC (big endian 16), 0x720 checksum = sum(0x709..0x71F) & 0xFF.
 *   (stock: "AF : macro %d infinity %d (no starting DAC set to infinity)", margins -0.25 / +0.05.)
 *
 *   a6l_afotp -b <i2c bus> [-a 0x58] [-n chunk] [-o dump.bin]    read the EEPROM (only address-pointer writes)
 *   a6l_afotp -F dump.bin                                         decode an existing dump (host tests)
 * Prints A6L_AF_OTP_READ bytes=<n>, A6L_AF_OTP flag=<f> macro=<m> infinity=<i> csum=<OK|BAD> (calc/stored),
 * A6L_AF_OTP_YAML infinityDac: <i> macroDac: <m> (only when flag == 1, checksum OK and 0 < inf < macro < 1024).
 * Exit 0 = AF data valid, 1 = read OK but AF data invalid, 2 = usage, 3 = I/O error.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifdef __linux__
#include <linux/i2c.h>
#include <linux/i2c-dev.h>
#include <sys/ioctl.h>
#endif

#define OTP_LEN0 0x0B05
#define OTP_ADDR1 0x1800
#define OTP_LEN1 0x0422
#define OTP_TOTAL (OTP_LEN0 + OTP_LEN1)

#define AF_FLAG 0x708
#define AF_MACRO 0x709
#define AF_INF 0x70B
#define AF_CSUM 0x720

static int decode(const uint8_t *b, size_t len)
{
	unsigned int sum = 0, i, macro, inf;
	int ok;

	if (len < AF_CSUM + 1) {
		printf("A6L_AF_OTP short buffer %zu\n", len);
		return 1;
	}
	for (i = AF_MACRO; i < AF_CSUM; i++)
		sum += b[i];
	macro = (unsigned int)b[AF_MACRO] << 8 | b[AF_MACRO + 1];
	inf = (unsigned int)b[AF_INF] << 8 | b[AF_INF + 1];
	ok = (sum & 0xff) == b[AF_CSUM];
	printf("A6L_AF_OTP flag=%u macro=%u infinity=%u csum=%s (calc 0x%02x stored 0x%02x)\n", b[AF_FLAG], macro, inf,
	       ok ? "OK" : "BAD", sum & 0xff, b[AF_CSUM]);
	if (b[AF_FLAG] == 1 && ok && inf > 0 && inf < macro && macro < 1024) {
		printf("A6L_AF_OTP_YAML infinityDac: %u macroDac: %u\n", inf, macro);
		return 0;
	}
	printf("A6L_AF_OTP_INVALID (keep the stock no-OTP range 227..627)\n");
	return 1;
}

#ifdef __linux__
static int read_range(int fd, unsigned int addr7, unsigned int start, uint8_t *dst, unsigned int len,
		      unsigned int chunk)
{
	unsigned int off;

	for (off = 0; off < len; off += chunk) {
		unsigned int n = len - off < chunk ? len - off : chunk, a = start + off;
		uint8_t ab[2] = { (uint8_t)(a >> 8), (uint8_t)a };
		struct i2c_msg msgs[2] = {
			{ .addr = (uint16_t)addr7, .flags = 0, .len = 2, .buf = ab },
			{ .addr = (uint16_t)addr7, .flags = I2C_M_RD, .len = (uint16_t)n, .buf = dst + off },
		};
		struct i2c_rdwr_ioctl_data x = { .msgs = msgs, .nmsgs = 2 };

		if (ioctl(fd, I2C_RDWR, &x) != 2) {
			fprintf(stderr, "a6l_afotp: read 0x%04x+%u: %s\n", a, n, strerror(errno));
			return -1;
		}
	}
	return 0;
}
#endif

int main(int argc, char **argv)
{
	const char *in = NULL, *out = NULL;
	int bus = -1, opt;
	unsigned int addr7 = 0x58, chunk = 8;
	uint8_t *b;
	size_t len = 0;

	while ((opt = getopt(argc, argv, "b:a:n:o:F:")) != -1) {
		switch (opt) {
		case 'b': bus = atoi(optarg); break;
		case 'a': addr7 = (unsigned int)strtoul(optarg, NULL, 0); break;
		case 'n': chunk = (unsigned int)strtoul(optarg, NULL, 0); break;
		case 'o': out = optarg; break;
		case 'F': in = optarg; break;
		default:
			fprintf(stderr, "usage: a6l_afotp -b bus [-a 0x58] [-n chunk] [-o dump] | -F dump\n");
			return 2;
		}
	}
	if ((bus < 0) == !in || addr7 < 0x08 || addr7 > 0x77 || chunk < 1 || chunk > 256) {
		fprintf(stderr, "usage: a6l_afotp -b bus [-a 0x58] [-n chunk] [-o dump] | -F dump\n");
		return 2;
	}
	b = calloc(1, OTP_TOTAL);
	if (!b)
		return 3;

	if (in) {
		FILE *f = fopen(in, "rb");
		if (!f) {
			fprintf(stderr, "a6l_afotp: %s: %s\n", in, strerror(errno));
			free(b);
			return 3;
		}
		len = fread(b, 1, OTP_TOTAL, f);
		fclose(f);
	} else {
#ifdef __linux__
		char dev[32];
		int fd;

		snprintf(dev, sizeof(dev), "/dev/i2c-%d", bus);
		fd = open(dev, O_RDWR);
		if (fd < 0) {
			fprintf(stderr, "a6l_afotp: %s: %s\n", dev, strerror(errno));
			free(b);
			return 3;
		}
		if (read_range(fd, addr7, 0x0000, b, OTP_LEN0, chunk) ||
		    read_range(fd, addr7, OTP_ADDR1, b + OTP_LEN0, OTP_LEN1, chunk)) {
			close(fd);
			free(b);
			return 3;
		}
		close(fd);
		len = OTP_TOTAL;
#else
		free(b);
		return 3;
#endif
		printf("A6L_AF_OTP_READ bytes=%zu bus=%d addr=0x%02x\n", len, bus, addr7);
		if (out) {
			FILE *f = fopen(out, "wb");
			if (!f || fwrite(b, 1, len, f) != len) {
				fprintf(stderr, "a6l_afotp: %s: %s\n", out, strerror(errno));
				if (f)
					fclose(f);
				free(b);
				return 3;
			}
			fclose(f);
		}
	}
	opt = decode(b, len);
	free(b);
	return opt;
}
