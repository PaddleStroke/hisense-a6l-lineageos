// SPDX-License-Identifier: Apache-2.0
/*
 * a6l_wavlevel (audio6, 25 Sep 2026): level meter for 16-bit PCM WAV files, runs on the phone (static aarch64) and on
 * the host. Prints one line per channel:
 *   A6L_AU6_LEVEL <file> ch<n> frames=<n> peak=<dBFS> rms=<dBFS> rms_steady=<dBFS> dc=<lsb> verdict=<SILENT|QUIET|OK|CLIP>
 * rms is DC-removed. rms_steady ignores the first 0.5 s (ADC/HPF start-up transient). With -b it also prints the
 * rms/peak of each 0.5 s block. Verdict: SILENT = steady rms < -80 dBFS (no analog signal at all, only the
 * quantisation floor, as the 25 Sep audio5 headset capture: rms ~0.8 LSB), QUIET = peak < -40 dBFS,
 * CLIP = peak >= -0.1 dBFS, OK otherwise.
 * usage: a6l_wavlevel [-b] file.wav...
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t le32(const unsigned char *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t le16(const unsigned char *p) { return (uint16_t)(p[0] | p[1] << 8); }
static double db(double v) { return v > 0 ? 20.0 * log10(v / 32768.0) : -144.0; }

static int level(const char *path, int blocks)
{
	FILE *f = fopen(path, "rb");
	unsigned char h[12], ch[8];
	unsigned int chans = 0, rate = 0, bits = 0;
	long data_len = -1;

	if (!f) { printf("A6L_AU6_LEVEL %s ERROR open\n", path); return 1; }
	if (fread(h, 1, 12, f) != 12 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4)) {
		printf("A6L_AU6_LEVEL %s ERROR not a RIFF/WAVE file\n", path); fclose(f); return 1;
	}
	while (fread(ch, 1, 8, f) == 8) {
		uint32_t len = le32(ch + 4);
		if (!memcmp(ch, "fmt ", 4)) {
			unsigned char fm[40];
			uint32_t n = len < sizeof(fm) ? len : sizeof(fm);
			if (n < 16 || fread(fm, 1, n, f) != n) break;
			chans = le16(fm + 2); rate = le32(fm + 4); bits = le16(fm + 14);
			if (len > n) fseek(f, (long)(len - n + (len & 1)), SEEK_CUR);
			else if (len & 1) fseek(f, 1, SEEK_CUR);
		} else if (!memcmp(ch, "data", 4)) {
			data_len = len; break;
		} else {
			fseek(f, (long)(len + (len & 1)), SEEK_CUR);
		}
	}
	if (data_len < 0 || bits != 16 || chans < 1 || chans > 8 || rate < 1000) {
		printf("A6L_AU6_LEVEL %s ERROR unsupported (bits=%u ch=%u rate=%u data=%ld)\n", path, bits, chans, rate, data_len);
		fclose(f); return 1;
	}
	/* tinycap leaves the data length 0 if interrupted: then read to EOF */
	long frames_max = data_len > 0 ? data_len / (2 * (long)chans) : -1;
	long skip = rate / 2, blk = rate / 2, n = 0;
	double sum[8] = {0}, sum2[8] = {0}, ssum[8] = {0}, ssum2[8] = {0}, bsum[8] = {0}, bsum2[8] = {0};
	int peak[8] = {0}, bpeak[8] = {0};
	long sn = 0, bn = 0;
	int16_t fr[8];
	unsigned char raw[16];

	while ((frames_max < 0 || n < frames_max) && fread(raw, 2, chans, f) == chans) {
		for (unsigned int c = 0; c < chans; c++) {
			fr[c] = (int16_t)le16(raw + 2 * c);
			int a = fr[c] < 0 ? -fr[c] : fr[c];
			sum[c] += fr[c]; sum2[c] += (double)fr[c] * fr[c];
			if (a > peak[c]) peak[c] = a;
			if (n >= skip) { ssum[c] += fr[c]; ssum2[c] += (double)fr[c] * fr[c]; }
			bsum[c] += fr[c]; bsum2[c] += (double)fr[c] * fr[c];
			if (a > bpeak[c]) bpeak[c] = a;
		}
		if (n >= skip) sn++;
		n++; bn++;
		if (bn == blk) {
			if (blocks)
				for (unsigned int c = 0; c < chans; c++) {
					double m = bsum[c] / bn, v = bsum2[c] / bn - m * m;
					printf("  block %5.2fs ch%u rms=%6.1f peak=%6.1f dBFS\n", (double)(n - bn) / rate, c,
					       db(sqrt(v > 0 ? v : 0)), db(bpeak[c]));
				}
			memset(bsum, 0, sizeof(bsum)); memset(bsum2, 0, sizeof(bsum2)); memset(bpeak, 0, sizeof(bpeak)); bn = 0;
		}
	}
	fclose(f);
	if (n == 0) { printf("A6L_AU6_LEVEL %s ERROR no samples\n", path); return 1; }
	for (unsigned int c = 0; c < chans; c++) {
		double m = sum[c] / n, v = sum2[c] / n - m * m, rms = db(sqrt(v > 0 ? v : 0));
		double srms = -144.0;
		if (sn > 0) { double sm = ssum[c] / sn, sv = ssum2[c] / sn - sm * sm; srms = db(sqrt(sv > 0 ? sv : 0)); }
		double pk = db(peak[c]);
		const char *verdict = pk >= -0.1 ? "CLIP" : (sn > 0 && srms < -80.0) ? "SILENT" : pk < -40.0 ? "QUIET" : "OK";
		printf("A6L_AU6_LEVEL %s ch%u rate=%u frames=%ld peak=%.1f rms=%.1f rms_steady=%.1f dc=%.1f verdict=%s\n",
		       path, c, rate, n, pk, rms, srms, m, verdict);
	}
	return 0;
}

int main(int argc, char **argv)
{
	int blocks = 0, rc = 0, i = 1;
	if (i < argc && !strcmp(argv[i], "-b")) { blocks = 1; i++; }
	if (i >= argc) { fprintf(stderr, "usage: %s [-b] file.wav...\n", argv[0]); return 2; }
	for (; i < argc; i++) rc |= level(argv[i], blocks);
	return rc;
}
