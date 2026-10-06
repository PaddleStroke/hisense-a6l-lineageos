// SPDX-License-Identifier: GPL-2.0
/*
 * a6l_afsharp: focus measure of one captured frame (Hisense A6L camera phase 3, AF sweep test).
 *
 *   a6l_afsharp -f FMT -w W -h H [-s stride] [-d step] [-p pos] [-r roi%] file
 *     FMT: XRGB8888 ARGB8888 XBGR8888 ABGR8888 (4 B/px), RGB888 BGR888 (3 B/px): green byte = byte 1
 *          NV12 / GREY (8-bit luma plane), RAW10P (MIPI CSI-2 packed, 2x2 block average = half-resolution luma)
 *     -d  difference spacing in pixels (default 1), -r centre ROI size in % of W and H (default 50)
 *   Prints: A6L_AF_SHARP pos=<p> sharp=<msd/mean^2> msd=<mean squared difference> mean=<mean> n=<samples>
 *   The measure = mean of (dx^2 + dy^2) over the ROI of the 8-bit green/luma plane, divided by mean^2
 *   (exposure-independent to first order). Same idea as the SoftISP statistic of AF patch 0101, at full density.
 * Exit 0 on success, 2 on usage errors, 3 on I/O errors.
 */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void usage(void)
{
	fprintf(stderr, "usage: a6l_afsharp -f FMT -w W -h H [-s stride] [-d step] [-p pos] [-r roi%%] file\n");
	exit(2);
}

/* Build the 8-bit analysis plane (width pw, height ph) from the frame. */
static uint8_t *plane_from(const uint8_t *buf, size_t len, const char *fmt, int w, int h, int stride,
			   int *pw, int *ph)
{
	int bpp = 0, raw10 = 0;
	uint8_t *p;
	int x, y;

	if (!strcmp(fmt, "XRGB8888") || !strcmp(fmt, "ARGB8888") || !strcmp(fmt, "XBGR8888") ||
	    !strcmp(fmt, "ABGR8888"))
		bpp = 4;
	else if (!strcmp(fmt, "RGB888") || !strcmp(fmt, "BGR888"))
		bpp = 3;
	else if (!strcmp(fmt, "NV12") || !strcmp(fmt, "GREY"))
		bpp = 1;
	else if (!strcmp(fmt, "RAW10P"))
		raw10 = 1;
	else
		return NULL;

	if (raw10) {
		if (w % 4 || h % 2)
			return NULL;
		if (!stride)
			stride = w * 5 / 4;
		if ((size_t)stride * h > len || stride < w * 5 / 4)
			return NULL;
		*pw = w / 2;
		*ph = h / 2;
		p = malloc((size_t)*pw * *ph);
		if (!p)
			return NULL;
		for (y = 0; y < *ph; y++) {
			const uint8_t *r0 = buf + (size_t)(2 * y) * stride;
			const uint8_t *r1 = r0 + stride;
			for (x = 0; x < *pw; x++) {
				/* pixels 2x, 2x+1 of a 4-pixel / 5-byte group: MSB bytes at (group*5 + i) */
				int g = (2 * x) / 4, i = (2 * x) % 4;
				const uint8_t *a = r0 + g * 5 + i, *b = r1 + g * 5 + i;
				p[(size_t)y * *pw + x] = (uint8_t)((a[0] + a[1] + b[0] + b[1] + 2) / 4);
			}
		}
		return p;
	}

	if (!stride)
		stride = w * bpp;
	if (stride < w * bpp || (size_t)stride * (h - 1) + (size_t)w * bpp > len)
		return NULL;
	*pw = w;
	*ph = h;
	p = malloc((size_t)w * h);
	if (!p)
		return NULL;
	for (y = 0; y < h; y++) {
		const uint8_t *r = buf + (size_t)y * stride;
		for (x = 0; x < w; x++)
			p[(size_t)y * w + x] = bpp == 1 ? r[x] : r[x * bpp + 1];
	}
	return p;
}

int main(int argc, char **argv)
{
	const char *fmt = NULL, *pos = "-", *file;
	int w = 0, h = 0, stride = 0, step = 1, roi = 50, opt, pw, ph, x, y;
	FILE *f;
	uint8_t *buf, *p;
	size_t len, cap = 1 << 20;
	double sum = 0, sq = 0, n = 0, mean, msd;

	while ((opt = getopt(argc, argv, "f:w:h:s:d:p:r:")) != -1) {
		switch (opt) {
		case 'f': fmt = optarg; break;
		case 'w': w = atoi(optarg); break;
		case 'h': h = atoi(optarg); break;
		case 's': stride = atoi(optarg); break;
		case 'd': step = atoi(optarg); break;
		case 'p': pos = optarg; break;
		case 'r': roi = atoi(optarg); break;
		default: usage();
		}
	}
	if (!fmt || w <= 0 || h <= 0 || step < 1 || roi < 5 || roi > 100 || optind != argc - 1)
		usage();
	file = argv[optind];

	f = fopen(file, "rb");
	if (!f) {
		fprintf(stderr, "a6l_afsharp: %s: %s\n", file, strerror(errno));
		return 3;
	}
	buf = malloc(cap);
	len = 0;
	while (buf) {
		size_t r = fread(buf + len, 1, cap - len, f);
		len += r;
		if (len < cap)
			break;
		cap *= 2;
		uint8_t *nb = realloc(buf, cap);
		if (!nb) {
			free(buf);
			buf = NULL;
		} else {
			buf = nb;
		}
	}
	fclose(f);
	if (!buf) {
		fprintf(stderr, "a6l_afsharp: out of memory\n");
		return 3;
	}

	p = plane_from(buf, len, fmt, w, h, stride, &pw, &ph);
	free(buf);
	if (!p) {
		fprintf(stderr, "a6l_afsharp: bad format/size (%s %dx%d stride %d, %zu bytes)\n", fmt, w, h, stride, len);
		return 2;
	}

	{
		int rw = pw * roi / 100, rh = ph * roi / 100;
		int x0 = (pw - rw) / 2, y0 = (ph - rh) / 2;
		for (y = y0; y < y0 + rh && y + step < ph; y++) {
			for (x = x0; x < x0 + rw && x + step < pw; x++) {
				int v = p[(size_t)y * pw + x];
				int dx = p[(size_t)y * pw + x + step] - v;
				int dy = p[(size_t)(y + step) * pw + x] - v;
				sum += v;
				sq += (double)dx * dx + (double)dy * dy;
				n++;
			}
		}
	}
	free(p);
	if (n < 1) {
		fprintf(stderr, "a6l_afsharp: empty ROI\n");
		return 2;
	}
	mean = sum / n;
	msd = sq / n;
	printf("A6L_AF_SHARP pos=%s sharp=%.6f msd=%.3f mean=%.2f n=%.0f\n", pos,
	       msd / (mean < 4 ? 16.0 : mean * mean), msd, mean, n);
	return 0;
}
