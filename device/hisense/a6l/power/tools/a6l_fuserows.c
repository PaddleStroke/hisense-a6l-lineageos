// SPDX-License-Identifier: GPL-2.0-only
/*
 * a6l_fuserows (agent power, 26 Sep 2026): read ONLY the CPR fuse rows the stock kernel itself reads, from the mainline
 * qfprom nvmem (V74: /sys/bus/nvmem/devices/qfprom0/nvmem = qfprom@780000, size 0x621c).
 *
 * Why: on 24 Sep `dd bs=64 skip=256 count=9` (bytes 0x4000..0x423f = corrected rows 0..71) RESET the phone. Rows 0..37
 * (0x784000..0x78412f: access-control / read-write-permission / secure-key rows) are XPU-protected; the stock kernel never
 * touches them. It reads, through its cprh "fuse_base" mapping 0x784000, only rows 38 (speed bin, 0x784130; also the OSM
 * "pwrcl_efuse"/"perfcl_efuse" reg 0x784130 len 8) and 65..71 (CPR init voltage / quotient / RO select / offsets /
 * fusing rev). The mainline GPU already reads row 52 (gpu-speed-bin@41a0) on every V74 boot without trouble.
 * This tool pread()s exactly those 8 rows (8 bytes each, no seek-by-reading, nothing else), and writes a 72-row
 * file (row 0 at offset 0, other rows zero) for `a6l_cpr_openloop.py a6l-fuserows.bin --offset 0`.
 * usage: a6l_fuserows [out.bin] [nvmem path]
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CORR_BASE 0x4000u
#define NROWS 72
static const unsigned rows[] = { 38, 65, 66, 67, 68, 69, 70, 71 };

static int find_qfprom(char *out, size_t n)
{
	DIR *d = opendir("/sys/bus/nvmem/devices");
	struct dirent *e;

	if (!d)
		return -1;
	while ((e = readdir(d))) {
		if (!strncmp(e->d_name, "qfprom", 6)) {
			snprintf(out, n, "/sys/bus/nvmem/devices/%s/nvmem", e->d_name);
			closedir(d);
			return 0;
		}
	}
	closedir(d);
	return -1;
}

int main(int argc, char **argv)
{
	const char *outp = argc > 1 ? argv[1] : "/tmp/a6l-fuserows.bin";
	char path[512];
	uint8_t img[NROWS * 8];
	struct stat st;
	unsigned i;
	int fd, of;

	if (argc > 2)
		snprintf(path, sizeof(path), "%s", argv[2]);
	else if (find_qfprom(path, sizeof(path))) {
		printf("A6L_FUSEROWS_FAIL no /sys/bus/nvmem/devices/qfprom*\n");
		return 1;
	}
	fd = open(path, O_RDONLY);
	if (fd < 0) {
		printf("A6L_FUSEROWS_FAIL open %s: %s\n", path, strerror(errno));
		return 1;
	}
	if (fstat(fd, &st) == 0)
		printf("A6L_FUSEROWS nvmem %s size=0x%llx\n", path, (unsigned long long)st.st_size);
	if (st.st_size && st.st_size < (off_t)(CORR_BASE + NROWS * 8)) {
		printf("A6L_FUSEROWS_FAIL nvmem too small (not qfprom@780000?)\n");
		return 1;
	}
	memset(img, 0, sizeof(img));
	for (i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
		off_t off = CORR_BASE + rows[i] * 8;
		ssize_t r;

		printf("A6L_FUSEROWS reading row %u (0x%lx) ...\n", rows[i], (long)(0x780000 + off));
		fflush(stdout);	/* the last printed row is the culprit if the phone resets */
		sync();
		r = pread(fd, img + rows[i] * 8, 8, off);
		if (r != 8) {
			printf("A6L_FUSEROWS_FAIL row %u: %zd (%s)\n", rows[i], r, strerror(errno));
			return 1;
		}
		printf("A6L_FUSEROWS row %2u 0x%06lx = %016llx\n", rows[i], (long)(0x780000 + off),
		       (unsigned long long)((uint64_t)img[rows[i] * 8] | (uint64_t)img[rows[i] * 8 + 1] << 8 |
		       (uint64_t)img[rows[i] * 8 + 2] << 16 | (uint64_t)img[rows[i] * 8 + 3] << 24 |
		       (uint64_t)img[rows[i] * 8 + 4] << 32 | (uint64_t)img[rows[i] * 8 + 5] << 40 |
		       (uint64_t)img[rows[i] * 8 + 6] << 48 | (uint64_t)img[rows[i] * 8 + 7] << 56));
	}
	close(fd);
	of = open(outp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (of < 0 || write(of, img, sizeof(img)) != (ssize_t)sizeof(img)) {
		printf("A6L_FUSEROWS_FAIL write %s\n", outp);
		return 1;
	}
	close(of);
	printf("A6L_FUSEROWS_DONE %s (%u bytes; rows 38,65..71 only)\n", outp, (unsigned)sizeof(img));
	return 0;
}
