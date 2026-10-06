// SPDX-License-Identifier: GPL-2.0-only
/*
 * a6l_imu (sens agent, 26 Sep 2026): ATTENDED readout of the ADSP/SMGR motion sensors through IIO buffers
 * (drivers/iio/{accel,gyro,magnetometer}/qcom_smgr_{accel,gyro,mag}.c: names qcom-smgr-accel / -gyro / -mag, channels x/y/z s32 + timestamp,
 * NO _raw attributes: data only arrives through the kfifo buffer, scale = 1/65536, SMGR frame = NED).
 *
 * usage: a6l_imu [-n phase] [-s seconds] [-c flat|turn|tilt|none] [-m map] [-p print_ms] [-u auto|gauss|ut]
 *   -m map : raw SMGR axes -> Android axes, 3 tokens, default "+y+x-z" (classic QTI SMGR->Android: X=y, Y=x, Z=-z).
 *            The flat phase prints which raw axis carries gravity so the map can be corrected on the spot.
 *   -u     : magnetometer unit of scale*raw. auto: |B| < 3 -> gauss (x100 to uT), else uT.
 * env A6L_IMU_SYS (default /sys/bus/iio/devices) and A6L_IMU_DEV (default /dev) for the host dry-run.
 * Markers: A6L_IMU_<CHECK>_PASS|FAIL ... ; everything else is informative.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <time.h>
#include <unistd.h>

#define NS 3
enum { ACC, GYR, MAG };
static const char *const sname[NS] = { "qcom-smgr-accel", "qcom-smgr-gyro", "qcom-smgr-mag" };
static const char *const stype[NS] = { "accel", "anglvel", "magn" };
static const char *const sshort[NS] = { "acc", "gyr", "mag" };

struct chan { int index, bytes, shift, bits, sgn, be; int axis; /* 0..2, 3 = timestamp */ };
struct sens {
	char sys[256];
	int num, fd, rec, nch, found;
	struct chan ch[4];
	double scale;
	long n;			/* samples */
	double last[3];		/* mapped, final unit */
	double sum[3], sum2[3], min[3], max[3], magsum, magmin, magmax;
	double normmax;
};
static struct sens S[NS];
static const char *sysroot = "/sys/bus/iio/devices", *devroot = "/dev";
static int map_src[3] = { 1, 0, 2 }, map_sgn[3] = { 1, 1, -1 };
static double magmul = 0;	/* 0 = auto */

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }

static int rd(const char *path, char *buf, size_t n)
{
	int fd = open(path, O_RDONLY);
	ssize_t r;
	if (fd < 0) return -1;
	r = read(fd, buf, n - 1);
	close(fd);
	if (r < 0) return -1;
	buf[r] = 0;
	while (r > 0 && (buf[r - 1] == '\n' || buf[r - 1] == ' ')) buf[--r] = 0;
	return 0;
}
static int wr(const char *path, const char *v)
{
	int fd = open(path, O_WRONLY);
	ssize_t r;
	if (fd < 0) return -1;
	r = write(fd, v, strlen(v));
	close(fd);
	return r < 0 ? -1 : 0;
}
static int wrf(struct sens *s, const char *rel, const char *v)
{
	char p[512];
	snprintf(p, sizeof(p), "%s/%s", s->sys, rel);
	return wr(p, v);
}
static int rdf(struct sens *s, const char *rel, char *buf, size_t n)
{
	char p[512];
	snprintf(p, sizeof(p), "%s/%s", s->sys, rel);
	return rd(p, buf, n);
}

static int parse_map(const char *m)
{
	int i, used = 0;
	if (strlen(m) != 6) return -1;
	for (i = 0; i < 3; i++) {
		char sg = m[2 * i], ax = m[2 * i + 1];
		if ((sg != '+' && sg != '-') || ax < 'x' || ax > 'z') return -1;
		map_sgn[i] = sg == '-' ? -1 : 1;
		map_src[i] = ax - 'x';
		if (used & (1 << map_src[i])) return -1;
		used |= 1 << map_src[i];
	}
	return 0;
}

static void find_devices(void)
{
	DIR *d = opendir(sysroot);
	struct dirent *e;
	char p[512], nm[64];
	int i;
	if (!d) { printf("A6L_IMU_FAIL cannot open %s\n", sysroot); exit(2); }
	while ((e = readdir(d))) {
		if (strncmp(e->d_name, "iio:device", 10)) continue;
		snprintf(p, sizeof(p), "%s/%s/name", sysroot, e->d_name);
		if (rd(p, nm, sizeof(nm))) continue;
		for (i = 0; i < NS; i++)
			if (!strcmp(nm, sname[i]) && !S[i].found) {
				S[i].found = 1;
				S[i].num = atoi(e->d_name + 10);
				snprintf(S[i].sys, sizeof(S[i].sys), "%s/%s", sysroot, e->d_name);
			}
	}
	closedir(d);
}

/* "le:s32/32>>0" */
static int parse_type(const char *t, struct chan *c)
{
	char e[3] = { 0 }, sg;
	if (sscanf(t, "%2[lb]e:%c%d/%d>>%d", e, &sg, &c->bits, &c->bytes, &c->shift) != 5) {
		/* sscanf %2[lb] consumed only 1 char: retry simple form */
		if (sscanf(t, "%c%*c:%c%d/%d>>%d", &e[0], &sg, &c->bits, &c->bytes, &c->shift) != 5) return -1;
	}
	c->be = e[0] == 'b';
	c->sgn = sg == 's';
	c->bytes /= 8;
	return 0;
}

static int setup(struct sens *s, int t)
{
	static const char ax[3] = { 'x', 'y', 'z' };
	char p[512], b[64], rel[128];
	int i, off, a;
	struct stat st;

	wrf(s, "buffer/enable", "0");
	s->nch = 0;
	for (a = 0; a < 4; a++) {
		struct chan *c = &s->ch[s->nch];
		if (a < 3) snprintf(rel, sizeof(rel), "scan_elements/in_%s_%c", stype[t], ax[a]);
		else snprintf(rel, sizeof(rel), "scan_elements/in_timestamp");
		snprintf(p, sizeof(p), "%s_en", rel);
		if (wrf(s, p, "1")) { if (a < 3) { printf("A6L_IMU_FAIL %s: cannot enable %s\n", sname[t], p); return -1; } continue; }
		snprintf(p, sizeof(p), "%s_index", rel);
		if (rdf(s, p, b, sizeof(b))) return -1;
		c->index = atoi(b);
		snprintf(p, sizeof(p), "%s_type", rel);
		if (rdf(s, p, b, sizeof(b)) || parse_type(b, c)) { printf("A6L_IMU_FAIL %s: bad type '%s'\n", sname[t], b); return -1; }
		c->axis = a;
		s->nch++;
	}
	/* sort by scan index, then lay out with natural alignment (IIO rules) */
	for (i = 0; i < s->nch; i++)
		for (a = i + 1; a < s->nch; a++)
			if (s->ch[a].index < s->ch[i].index) { struct chan x = s->ch[i]; s->ch[i] = s->ch[a]; s->ch[a] = x; }
	off = 0;
	for (i = 0; i < s->nch; i++) {
		int bb = s->ch[i].bytes;
		off = (off + bb - 1) / bb * bb;
		s->ch[i].shift |= off << 8;	/* stash offset in the upper bits */
		off += bb;
	}
	{
		int maxb = 1;
		for (i = 0; i < s->nch; i++) if (s->ch[i].bytes > maxb) maxb = s->ch[i].bytes;
		s->rec = (off + maxb - 1) / maxb * maxb;
	}
	snprintf(p, sizeof(p), "in_%s_scale", stype[t]);
	s->scale = rdf(s, p, b, sizeof(b)) ? 1.0 / 65536 : atof(b);
	if (s->scale == 0) s->scale = 1.0 / 65536;
	wrf(s, "buffer/length", "256");
	if (wrf(s, "buffer/enable", "1")) { printf("A6L_IMU_FAIL %s: buffer/enable failed (%s)\n", sname[t], strerror(errno)); return -1; }
	snprintf(p, sizeof(p), "%s/iio:device%d", devroot, s->num);
	if (stat(p, &st)) {
		char dv[32];
		unsigned ma, mi;
		if (!rdf(s, "dev", dv, sizeof(dv)) && sscanf(dv, "%u:%u", &ma, &mi) == 2) mknod(p, S_IFCHR | 0600, makedev(ma, mi));
	}
	s->fd = open(p, O_RDONLY | O_NONBLOCK);
	if (s->fd < 0) { printf("A6L_IMU_FAIL %s: open %s: %s\n", sname[t], p, strerror(errno)); return -1; }
	snprintf(p, sizeof(p), "in_%s_sampling_frequency", stype[t]);
	if (rdf(s, p, b, sizeof(b))) strcpy(b, "?");
	printf("A6L_IMU_DEV %s iio:device%d rec=%dB scale=%.9g rate=%s\n", sname[t], s->num, s->rec, s->scale, b);
	return 0;
}

static int64_t getval(const uint8_t *r, const struct chan *c)
{
	int off = c->shift >> 8, sh = c->shift & 0xff, i;
	uint64_t v = 0;
	for (i = 0; i < c->bytes; i++) v |= (uint64_t)r[off + (c->be ? c->bytes - 1 - i : i)] << (8 * i);
	v >>= sh;
	if (c->bits < 64) {
		v &= (1ULL << c->bits) - 1;
		if (c->sgn && (v >> (c->bits - 1))) v |= ~((1ULL << c->bits) - 1);
	}
	return (int64_t)v;
}

/* per-phase trackers */
static double hdg_last = -1, hdg_unwrap = 0, gz_int = 0, gz_sum = 0;
static int bins_raw[12], nb_raw;
static double mxmin = 1e9, mxmax = -1e9, mymin = 1e9, mymax = -1e9;
#define MAXH 20000
static float hx[MAXH], hy[MAXH];
static int nh;

static double heading(double mx, double my)
{
	double h = atan2(-mx, my) * 180.0 / M_PI;
	return h < 0 ? h + 360 : h;
}

static void sample(int t, const uint8_t *r, double tn)
{
	struct sens *s = &S[t];
	double raw[3] = { 0 }, v[3], n;
	int i;
	for (i = 0; i < s->nch; i++)
		if (s->ch[i].axis < 3) raw[s->ch[i].axis] = getval(r, &s->ch[i]) * s->scale;
	for (i = 0; i < 3; i++) v[i] = map_sgn[i] * raw[map_src[i]];
	if (t == MAG) {
		if (magmul == 0) magmul = sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]) < 3.0 ? 100.0 : 1.0;
		for (i = 0; i < 3; i++) v[i] *= magmul;
	}
	n = sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
	if (!s->n) for (i = 0; i < 3; i++) { s->min[i] = 1e9; s->max[i] = -1e9; }
	if (!s->n) { s->magmin = 1e9; s->magmax = -1e9; }
	for (i = 0; i < 3; i++) {
		s->last[i] = v[i]; s->sum[i] += v[i]; s->sum2[i] += v[i] * v[i];
		if (v[i] < s->min[i]) s->min[i] = v[i];
		if (v[i] > s->max[i]) s->max[i] = v[i];
	}
	s->magsum += n;
	if (n < s->magmin) s->magmin = n;
	if (n > s->magmax) s->magmax = n;
	s->n++;
	if (t == GYR) {
		(void)tn;
		gz_sum += v[2];	/* integrated at the end with the measured rate (reads come in bursts) */
	}
	if (t == MAG) {
		double h = heading(v[0], v[1]);
		if (hdg_last >= 0) { double d = h - hdg_last; if (d > 180) d -= 360; if (d < -180) d += 360; hdg_unwrap += d; }
		hdg_last = h;
		if (!(bins_raw[(int)(h / 30) % 12]++)) nb_raw++;
		if (v[0] < mxmin) mxmin = v[0];
		if (v[0] > mxmax) mxmax = v[0];
		if (v[1] < mymin) mymin = v[1];
		if (v[1] > mymax) mymax = v[1];
		if (nh < MAXH) { hx[nh] = v[0]; hy[nh] = v[1]; nh++; }
	}
}

static double mean(struct sens *s, int i) { return s->n ? s->sum[i] / s->n : 0; }
static double sd(struct sens *s, int i)
{
	double m = mean(s, i), x;
	if (!s->n) return 0;
	x = s->sum2[i] / s->n - m * m;
	return x > 0 ? sqrt(x) : 0;
}

int main(int argc, char **argv)
{
	const char *phase = "phase", *check = "none", *unit = "auto";
	double secs = 10, t0, tp, pms = 500;
	int o, t, ok = 0, fails = 0;
	uint8_t buf[4096];

	while ((o = getopt(argc, argv, "n:s:c:m:p:u:")) != -1) {
		switch (o) {
		case 'n': phase = optarg; break;
		case 's': secs = atof(optarg); break;
		case 'c': check = optarg; break;
		case 'm': if (parse_map(optarg)) { fprintf(stderr, "bad map %s (e.g. +y+x-z)\n", optarg); return 2; } break;
		case 'p': pms = atof(optarg); break;
		case 'u': unit = optarg; break;
		default: fprintf(stderr, "usage: a6l_imu [-n phase] [-s secs] [-c flat|turn|tilt|none] [-m +y+x-z] [-p ms] [-u auto|gauss|ut]\n"); return 2;
		}
	}
	if (!strcmp(unit, "gauss")) magmul = 100; else if (!strcmp(unit, "ut")) magmul = 1;
	if (getenv("A6L_IMU_SYS")) sysroot = getenv("A6L_IMU_SYS");
	if (getenv("A6L_IMU_DEV")) devroot = getenv("A6L_IMU_DEV");
	setvbuf(stdout, NULL, _IOLBF, 0);
	find_devices();
	for (t = 0; t < NS; t++) {
		if (!S[t].found) { printf("A6L_IMU_MISSING %s (no IIO device with that name)\n", sname[t]); S[t].fd = -1; continue; }
		if (setup(&S[t], t)) { S[t].fd = -1; continue; }
		ok++;
	}
	if (!ok) { printf("A6L_IMU_FAIL no SMGR IIO device usable\n"); return 3; }
	printf("A6L_IMU_PHASE %s %.0fs map=%c%c%c%c%c%c (Android X,Y,Z from raw SMGR axes)\n", phase, secs,
	       map_sgn[0] < 0 ? '-' : '+', 'x' + map_src[0], map_sgn[1] < 0 ? '-' : '+', 'x' + map_src[1],
	       map_sgn[2] < 0 ? '-' : '+', 'x' + map_src[2]);
	t0 = tp = now();
	while (now() - t0 < secs) {
		struct pollfd p[NS];
		int np = 0, i;
		for (t = 0; t < NS; t++) { p[t].fd = S[t].fd; p[t].events = POLLIN; p[t].revents = 0; if (S[t].fd >= 0) np++; }
		(void)np;
		if (poll(p, NS, 50) < 0 && errno != EINTR) break;
		for (t = 0; t < NS; t++) {
			ssize_t r;
			if (S[t].fd < 0 || !(p[t].revents & POLLIN)) continue;
			r = read(S[t].fd, buf, sizeof(buf) / S[t].rec * S[t].rec);
			if (r <= 0) { if (r == 0) usleep(20000); continue; }
			for (i = 0; i + S[t].rec <= r; i += S[t].rec) sample(t, buf + i, now());
		}
		if ((now() - tp) * 1000 >= pms) {
			struct sens *a = &S[ACC], *g = &S[GYR], *m = &S[MAG];
			tp = now();
			printf("  t=%5.1f acc=(%6.2f %6.2f %6.2f) |g|=%5.2f  gyr=(%6.3f %6.3f %6.3f)  mag=(%6.1f %6.1f %6.1f)uT |B|=%5.1f hdg=%5.1f\n",
			       tp - t0, a->last[0], a->last[1], a->last[2],
			       sqrt(a->last[0] * a->last[0] + a->last[1] * a->last[1] + a->last[2] * a->last[2]),
			       g->last[0], g->last[1], g->last[2], m->last[0], m->last[1], m->last[2],
			       sqrt(m->last[0] * m->last[0] + m->last[1] * m->last[1] + m->last[2] * m->last[2]),
			       m->n ? heading(m->last[0], m->last[1]) : -1.0);
		}
	}
	for (t = 0; t < NS; t++) {
		struct sens *s = &S[t];
		double dt = now() - t0;
		if (S[t].fd >= 0) { close(S[t].fd); wrf(s, "buffer/enable", "0"); }
		if (!s->found) continue;
		printf("A6L_IMU_STAT %s %s n=%ld rate=%.1fHz mean=(%.3f %.3f %.3f) sd=(%.3f %.3f %.3f) min=(%.2f %.2f %.2f) max=(%.2f %.2f %.2f) |v| mean=%.3f min=%.3f max=%.3f\n",
		       phase, sshort[t], s->n, s->n / dt, mean(s, 0), mean(s, 1), mean(s, 2), sd(s, 0), sd(s, 1), sd(s, 2),
		       s->n ? s->min[0] : 0, s->n ? s->min[1] : 0, s->n ? s->min[2] : 0, s->n ? s->max[0] : 0, s->n ? s->max[1] : 0,
		       s->n ? s->max[2] : 0, s->n ? s->magsum / s->n : 0, s->n ? s->magmin : 0, s->n ? s->magmax : 0);
		if (!s->n) { printf("A6L_IMU_NODATA_FAIL %s: no samples in %.0f s (buffer enabled but SMGR sent nothing)\n", sname[t], secs); fails++; }
	}
	if (S[GYR].n) gz_int = gz_sum * (now() - t0) / S[GYR].n;
	if (S[MAG].n) printf("A6L_IMU_MAGUNIT scale*raw x%.0f -> uT (%s)\n", magmul, magmul == 100 ? "raw was gauss" : "raw taken as uT");

	if (!strcmp(check, "flat")) {
		struct sens *a = &S[ACC], *g = &S[GYR], *m = &S[MAG];
		double gm = a->n ? a->magsum / a->n : 0, wm = g->n ? g->magsum / g->n : 1e9, bm = m->n ? m->magsum / m->n : 0;
		int i, gi = 0;
		for (i = 1; i < 3; i++) if (fabs(mean(a, i)) > fabs(mean(a, gi))) gi = i;
		printf("A6L_IMU_GRAVITY on Android axis %c, sign %c (%.2f) -> %s\n", 'X' + gi, mean(a, gi) < 0 ? '-' : '+', mean(a, gi),
		       gi == 2 && mean(a, 2) > 0 ? "map OK (flat screen-up gives Z=+g)" :
		       "MAP WRONG for a phone lying screen-up: rerun with another -m (see doc) and report");
		if (gm >= 9.3 && gm <= 10.3) printf("A6L_IMU_ACCEL_PASS |g|=%.2f m/s2\n", gm); else { printf("A6L_IMU_ACCEL_FAIL |g|=%.2f (want 9.3-10.3)\n", gm); fails++; }
		if (g->n && wm < 0.05 && g->magmax < 0.15) printf("A6L_IMU_GYRO_REST_PASS mean|w|=%.4f max=%.4f rad/s\n", wm, g->magmax);
		else { printf("A6L_IMU_GYRO_REST_FAIL mean|w|=%.4f max=%.4f rad/s (want <0.05, max <0.15; did the phone move?)\n", g->n ? wm : -1, g->n ? g->magmax : -1); fails++; }
		if (m->n) printf("A6L_IMU_MAG_REST |B|=%.1f uT (sd %.2f %.2f %.2f), heading %.1f deg (uncalibrated)\n", bm, sd(m, 0), sd(m, 1), sd(m, 2), heading(mean(m, 0), mean(m, 1)));
	} else if (!strcmp(check, "turn")) {
		struct sens *g = &S[GYR], *m = &S[MAG];
		double cx = (mxmin + mxmax) / 2, cy = (mymin + mymax) / 2, rsum = 0, bm = m->n ? m->magsum / m->n : 0;
		int bins_c[12] = { 0 }, nbc = 0, i;
		for (i = 0; i < nh; i++) {
			double h = heading(hx[i] - cx, hy[i] - cy);
			rsum += sqrt((hx[i] - cx) * (hx[i] - cx) + (hy[i] - cy) * (hy[i] - cy));
			if (!(bins_c[(int)(h / 30) % 12]++)) nbc++;
		}
		if (g->n && g->magmax > 0.3) printf("A6L_IMU_GYRO_TURN_PASS max|w|=%.2f rad/s, integrated Z rotation %.0f deg\n", g->magmax, gz_int * 180 / M_PI);
		else { printf("A6L_IMU_GYRO_TURN_FAIL max|w|=%.3f rad/s (want >0.3 while turning)\n", g->n ? g->magmax : -1); fails++; }
		printf("A6L_IMU_MAG_TURN raw |B| mean=%.1f min=%.1f max=%.1f uT; horizontal hard-iron centre (%.1f, %.1f) uT, corrected horizontal radius %.1f uT\n",
		       bm, m->n ? m->magmin : 0, m->n ? m->magmax : 0, cx, cy, nh ? rsum / nh : 0);
		printf("A6L_IMU_HEADING sweep bins(30deg) raw=%d/12 corrected=%d/12, heading change %.0f deg, gyro Z %.0f deg (expect heading change ~ -gyroZ)\n",
		       nb_raw, nbc, hdg_unwrap, gz_int * 180 / M_PI);
		if (m->n && fabs(gz_int) > 1.5) {
			double ratio = -hdg_unwrap / (gz_int * 180 / M_PI);
			printf("A6L_IMU_FRAME %s (heading/gyro ratio %.2f; 1.0 = mag and gyro agree, -1 = one axis sign flipped)\n",
			       ratio > 0.6 && ratio < 1.4 ? "CONSISTENT" : "INCONSISTENT", ratio);
		}
		{
			int mag_ok = bm >= 25 && bm <= 65, hr_ok = nh && rsum / nh >= 10 && rsum / nh <= 40, sw_ok = nb_raw >= 10 || nbc >= 10;
			printf("A6L_IMU_MAG_MAGNITUDE_%s raw mean |B|=%.1f uT (want 25-65; hard iron can push it out, see corrected radius)\n", mag_ok ? "PASS" : "FAIL", bm);
			printf("A6L_IMU_MAG_SWEEP_%s raw %d/12 corrected %d/12 (want >=10/12 during a full turn)\n", sw_ok ? "PASS" : "FAIL", nb_raw, nbc);
			if (m->n && (mag_ok || hr_ok) && sw_ok) printf("A6L_IMU_MAG_PASS\n");
			else { printf("A6L_IMU_MAG_FAIL\n"); fails++; }
		}
	} else if (!strcmp(check, "tilt")) {
		struct sens *a = &S[ACC];
		double rx = a->n ? a->max[0] - a->min[0] : 0, ry = a->n ? a->max[1] - a->min[1] : 0;
		if (rx > 6 && ry > 6) printf("A6L_IMU_TILT_PASS accel X range %.1f, Y range %.1f m/s2\n", rx, ry);
		else { printf("A6L_IMU_TILT_FAIL accel X range %.1f, Y range %.1f m/s2 (want >6 each: tilt left/right AND forward/back)\n", rx, ry); fails++; }
	}
	printf("A6L_IMU_DONE %s fails=%d\n", phase, fails);
	return fails ? 1 : 0;
}
