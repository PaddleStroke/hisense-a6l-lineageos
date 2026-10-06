/*
 * Host tests for sensors.a6l (agent senshal, 26 Sep 2026). Drives the real HAL module (HAL_MODULE_INFO_SYM) against a
 * fake IIO tree made by mkfake_hal.py: the poll thread runs like the LineageOS 2.0 sub-HAL wrapper's, control calls
 * come from the main thread like binder calls. usage: test_sensors_a6l <case> (env A6L_SYSROOT/DEVROOT/DATADIR).
 * Prints A6L_T_PASS/FAIL lines; exit 0 = all checks passed.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <linux/iio/events.h>
#include <linux/iio/types.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <hardware/sensors.h>
#include "a6l_motion.h"

extern struct sensors_module_t HAL_MODULE_INFO_SYM;
static sensors_poll_device_1_t *dev;
#define MAXE 40000
static sensors_event_t ev[MAXE];
static int nev;
static pthread_mutex_t lk = PTHREAD_MUTEX_INITIALIZER;
static int fails;

#define CHECK(cond, ...) do { if (cond) { printf("A6L_T_PASS "); printf(__VA_ARGS__); printf("\n"); } \
	else { printf("A6L_T_FAIL "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

/* feeds src/iio:deviceN into dev/iio:deviceN at the real sample rate (SMGR -> kfifo stand-in) */
static void *feeder(void *arg)
{
	const char *root = arg;
	char p[600], name[64];
	struct { int fd, rate, rec; long sent, total; char *data; } st[8];
	int ns = 0, rate, rec;
	snprintf(p, sizeof(p), "%s/src/feed.txt", root);
	FILE *f = fopen(p, "r");
	if (!f)
		return NULL;
	while (ns < 8 && fscanf(f, "%63s %d %d", name, &rate, &rec) == 3) {
		snprintf(p, sizeof(p), "%s/src/%s", root, name);
		FILE *s = fopen(p, "rb");
		if (!s)
			continue;
		fseek(s, 0, SEEK_END);
		long sz = ftell(s);
		fseek(s, 0, SEEK_SET);
		st[ns].data = malloc(sz);
		if (fread(st[ns].data, 1, sz, s) != (size_t)sz) sz = 0;
		fclose(s);
		snprintf(p, sizeof(p), "%s/dev/%s", root, name);
		st[ns].fd = open(p, O_WRONLY | O_APPEND);
		st[ns].rate = rate;
		st[ns].rec = rec;
		st[ns].sent = 0;
		st[ns].total = sz / rec;
		ns++;
	}
	fclose(f);
	int64_t t0 = a6l_now_ns();
	for (;;) {
		int busy = 0;
		double el = (a6l_now_ns() - t0) / 1e9;
		for (int i = 0; i < ns; i++) {
			long due = (long)(el * st[i].rate);
			if (due > st[i].total) due = st[i].total;
			if (due > st[i].sent) {
				if (write(st[i].fd, st[i].data + st[i].sent * st[i].rec, (due - st[i].sent) * st[i].rec) < 0) return NULL;
				st[i].sent = due;
			}
			busy |= st[i].sent < st[i].total;
		}
		if (!busy)
			return NULL;
		usleep(5000);
	}
}

static int stop_poll;	/* __atomic: set before a wake-up event, the poller exits after storing it */
static pthread_t poll_thread;

static void *poller(void *arg)
{
	(void)arg;
	sensors_event_t b[128];
	for (;;) {
		int n = dev->poll(&dev->v0, b, 128);
		pthread_mutex_lock(&lk);
		for (int i = 0; i < n && nev < MAXE; i++)
			ev[nev++] = b[i];
		pthread_mutex_unlock(&lk);
		if (__atomic_load_n(&stop_poll, __ATOMIC_ACQUIRE))
			return NULL;
	}
	return NULL;
}

/* test race fix (r5 pass2): close() frees the device, so the poll thread must be out of poll() first. The HAL's poll
 * only returns with events: request a flush of an ACTIVE sensor, whose FLUSH_COMPLETE wakes the poller */
static void stop_poller(int active_handle)
{
	__atomic_store_n(&stop_poll, 1, __ATOMIC_RELEASE);
	dev->flush(dev, active_handle);
	pthread_join(poll_thread, NULL);
}

static void settle(void)	/* wait until no new events for 300 ms */
{
	int last = -1;
	for (int i = 0; i < 100; i++) {
		usleep(300000);
		pthread_mutex_lock(&lk);
		int n = nev;
		pthread_mutex_unlock(&lk);
		if (n == last)
			return;
		last = n;
	}
}

static int count_of(int h, int from)
{
	int k = 0;
	for (int i = from; i < nev; i++)
		k += ev[i].sensor == h && ev[i].type != SENSOR_TYPE_META_DATA;
	return k;
}

static int mono_ok(int h, int64_t *mean_dt)
{
	int64_t last = 0, first = 0;
	int k = 0;
	for (int i = 0; i < nev; i++) {
		if (ev[i].sensor != h || ev[i].type == SENSOR_TYPE_META_DATA)
			continue;
		if (k && ev[i].timestamp <= last)
			return 0;
		if (!k)
			first = ev[i].timestamp;
		last = ev[i].timestamp;
		k++;
	}
	if (mean_dt)
		*mean_dt = k > 1 ? (last - first) / (k - 1) : 0;
	return last <= a6l_now_ns();
}

static void open_hal(void)
{
	hw_device_t *d;
	int r = HAL_MODULE_INFO_SYM.common.methods->open(&HAL_MODULE_INFO_SYM.common, SENSORS_HARDWARE_POLL, &d);
	if (r) {
		printf("A6L_T_FAIL open %d\n", r);
		exit(1);
	}
	dev = (sensors_poll_device_1_t *)d;
	pthread_create(&poll_thread, NULL, poller, NULL);
}

static int on(int h, int64_t period_ms, int64_t lat_ms)
{
	int r = dev->batch(dev, h, 0, period_ms * 1000000, lat_ms * 1000000);
	if (r)
		return r;
	return dev->activate(&dev->v0, h, 1);
}

/* r5 F4 helpers */
static int wait_count(int h, int from, int want, int ms)	/* until >= want events of h after index from */
{
	for (int t = 0; t < ms; t += 50) {
		pthread_mutex_lock(&lk);
		int k = count_of(h, from);
		pthread_mutex_unlock(&lk);
		if (k >= want)
			return k;
		usleep(50000);
	}
	pthread_mutex_lock(&lk);
	int k = count_of(h, from);
	pthread_mutex_unlock(&lk);
	return k;
}

static int hal_fd_of(const char *path)	/* the HAL's fd for path, found through /proc/self/fd */
{
	char want[4096], p[80], l[4096];	/* realpath() needs PATH_MAX */
	if (!realpath(path, want))
		return -1;
	DIR *d = opendir("/proc/self/fd");
	struct dirent *e;
	int fd = -1;
	while (d && (e = readdir(d))) {
		snprintf(p, sizeof(p), "/proc/self/fd/%s", e->d_name);
		ssize_t n = readlink(p, l, sizeof(l) - 1);
		if (n <= 0)
			continue;
		l[n] = 0;
		if (strcmp(l, want))
			continue;
		/* the feeder writes the same file: only the HAL's read-only fd counts */
		snprintf(p, sizeof(p), "/proc/self/fdinfo/%s", e->d_name);
		FILE *f = fopen(p, "r");
		unsigned flags = O_WRONLY;
		while (f && fgets(l, sizeof(l), f))
			if (sscanf(l, "flags: %o", &flags) == 1)
				break;
		if (f)
			fclose(f);
		if ((flags & O_ACCMODE) == O_RDONLY)
			fd = atoi(e->d_name);
	}
	if (d)
		closedir(d);
	return fd;
}

static char rd1(const char *rel)	/* first char of $A6L_SYSROOT/rel */
{
	char p[600], b[8] = "";
	snprintf(p, sizeof(p), "%s/%s", getenv("A6L_SYSROOT"), rel);
	FILE *f = fopen(p, "r");
	if (f) { if (!fgets(b, sizeof(b), f)) b[0] = 0; fclose(f); }
	return b[0];
}

static int last_prox(int from, float *dist)	/* number of proximity events after from, last distance */
{
	int k = 0;
	pthread_mutex_lock(&lk);
	for (int i = from; i < nev; i++)
		if (ev[i].sensor == A6L_H_PROX && ev[i].type == SENSOR_TYPE_PROXIMITY) { k++; *dist = ev[i].distance; }
	pthread_mutex_unlock(&lk);
	return k;
}

#define G_TEST 9.80665
static double vnorm(const float *v) { return sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); }

int main(int argc, char **argv)
{
	const char *c = argc > 1 ? argv[1] : "list";
	setvbuf(stdout, NULL, _IOLBF, 0);

	if (!strcmp(c, "fit")) {	/* pure unit tests: sphere fit + planar rejection + tick mapping */
		float p[64][3], cc[3], r, rms;
		srand(7);
		for (int i = 0; i < 64; i++) {
			double u = 2.0 * rand() / RAND_MAX - 1, ph = 2 * M_PI * rand() / RAND_MAX, s = sqrt(1 - u * u);
			p[i][0] = 10 + 45 * s * cos(ph);
			p[i][1] = -20 + 45 * s * sin(ph);
			p[i][2] = 5 + 45 * u;
		}
		CHECK(!a6l_sphere_fit((const float (*)[3])p, 64, cc, &r, &rms) && fabsf(cc[0] - 10) < 0.01f &&
		      fabsf(cc[1] + 20) < 0.01f && fabsf(cc[2] - 5) < 0.01f && fabsf(r - 45) < 0.01f,
		      "sphere fit exact centre (%.3f %.3f %.3f) r=%.3f", cc[0], cc[1], cc[2], r);
		struct a6l_magcal mc;
		a6l_magcal_init(&mc, "/nonexistent/x");
		for (int i = 0; i < 400; i++) {	/* planar turn only */
			float b[3] = { (float)(8 + 21 * cos(i * 0.05)), (float)(-5 + 21 * sin(i * 0.05)), -39 };
			a6l_magcal_add(&mc, b, 1);
		}
		CHECK(!mc.have_bias && mc.accuracy == SENSOR_STATUS_UNRELIABLE,
		      "planar-only data does not produce a bias (bins=%d, acc=%d)", mc.bins, mc.accuracy);
		struct a6l_tsmap t;
		a6l_tsmap_reset(&t, 32768);
		int64_t now0 = 1000000000000LL, lastts = 0;
		int mono = 1;
		double maxerr = 0;
		/* true tick rate 19.2 MHz / 586 = 32764.5 Hz (0.01 % off) with 2..9 ms delivery jitter, wrap after 1 s */
		for (int k = 0; k < 2000; k++) {
			double tt = k * 0.005;
			uint32_t tick = (uint32_t)((uint64_t)((1ULL << 32) - 32768) + (uint64_t)llround(tt * 32764.5));
			int64_t now = now0 + (int64_t)(tt * 1e9) + 2000000 + (k * 7919 % 7) * 1000000;
			uint64_t e = a6l_tsmap_unwrap(&t, tick);
			a6l_tsmap_anchor(&t, e, now);
			int64_t ts = a6l_tsmap_ns(&t, e);
			if (ts <= lastts)
				mono = 0;
			lastts = ts;
			double err = fabs((double)(ts - (now0 + (int64_t)(tt * 1e9) + 2000000)));
			if (k > 10 && err > maxerr)
				maxerr = err;
		}
		CHECK(mono && maxerr < 2e6, "tick map across u32 wrap: monotonic, max error vs true time %.2f ms", maxerr / 1e6);
		a6l_tsmap_reset(&t, 32768);	/* ticks actually 1 kHz: must be measured and adopted after 3 s */
		for (int k = 0; k <= 500; k++) {
			uint64_t e = a6l_tsmap_unwrap(&t, (uint32_t)(k * 10));
			a6l_tsmap_anchor(&t, e, now0 + (int64_t)k * 10000000 + 1000000);
		}
		CHECK(fabs(t.hz - 1000) < 5, "wrong tick rate detected and replaced (%.1f Hz)", t.hz);
		return fails ? 1 : 0;
	}

	/* ------------------------------------------------ r5 deep review F30/F31/F32: pure cases on the real engine */
	if (!strcmp(c, "backlog")) {	/* F30: 1..700 scans waiting in the kfifo when the poll thread runs */
		static const int ns[] = { 1, 64, 65, 128, 256, 700 };
		char path[600];
		snprintf(path, sizeof(path), "%s/backlog.bin", getenv("A6L_T_ROOT") ? getenv("A6L_T_ROOT") : "/tmp");
		for (int t = 0; t < 7; t++) {
			int nrec = t < 6 ? ns[t] : 256;
			struct a6l_motion *m = calloc(1, sizeof(*m));
			a6l_motion_init(m);
			struct a6l_iio *d = &m->d[A6L_ACC];
			d->rec = 24; d->nch = 4; d->has_ts = 1; d->scale = 1; d->native_ns = 5e6; d->on = 1; d->epoch = 1;
			for (int i = 0; i < 4; i++)
				d->ch[i] = (struct a6l_chan){ .index = i, .bytes = i == 3 ? 8 : 4, .bits = 32, .off = i == 3 ? 16 : i * 4,
							      .axis = i };
			FILE *f = fopen(path, "wb");
			for (int k = 0; f && k < nrec; k++) {	/* 200 Hz, ticks across the u32 wrap */
				uint8_t r[24] = { 0 };
				uint32_t tick = (uint32_t)((1ULL << 32) - 16384 + (uint64_t)k * 32768 / 200);
				int32_t x = k;
				memcpy(r, &x, 4);
				memcpy(r + 16, &tick, 4);
				fwrite(r, 1, 24, f);
			}
			if (f) fclose(f);
			d->fd = open(path, O_RDONLY | O_CLOEXEC);
			struct a6l_mh *h = &m->h[A6L_H_ACCEL - A6L_H_FIRST_MOTION];
			h->enabled = 1;
			h->period = 5000000;
			if (t == 6)
				h->act_ns = a6l_now_ns() + 1000000000LL;	/* F31: scans older than the activation */
			a6l_motion_service(m);
			int64_t now = a6l_now_ns();
			static sensors_event_t out[1024];
			int n = a6l_motion_pop(m, out, 1024), sp_ok = 1, mono = 1;
			for (int k = 1; k < n; k++) {
				int64_t dt = out[k].timestamp - out[k - 1].timestamp;
				mono &= dt > 0 && out[k].acceleration.y > out[k - 1].acceleration.y;
				sp_ok &= llabs(dt - 5000000) < 50000;
			}
			if (t == 6)
				CHECK(n == 0, "F31: scans acquired before the activation are not delivered (%d)", n);
			else if (nrec <= A6L_RBATCH)
				CHECK(n == nrec && mono && sp_ok && out[n - 1].timestamp <= now,
				      "F30: backlog of %d scans -> %d events, 5.000 ms acquisition spacing kept (%s), last <= now",
				      nrec, n, sp_ok ? "yes" : "NO");
			else
				CHECK(n == nrec && mono, "F30: %d scans (> one %d-scan batch) -> %d events, none lost, increasing",
				      nrec, A6L_RBATCH, n);
			close(d->fd);
			d->fd = -1;
			d->on = 0;
			free(m);
		}
		return fails ? 1 : 0;
	}
	if (!strcmp(c, "queue")) {	/* F31: disable/flush boundaries and overflow */
		struct a6l_motion *m = calloc(1, sizeof(*m));
		a6l_motion_init(m);
		sensors_event_t e, out[8];
		CHECK(!a6l_motion_activate(m, A6L_H_ACCEL, 1) && !a6l_motion_activate(m, A6L_H_GYRO, 1), "activate accel + gyro");
		memset(&e, 0, sizeof(e));
		e.sensor = A6L_H_ACCEL; e.type = SENSOR_TYPE_ACCELEROMETER;
		a6l_evq_push(&m->q, &e);
		e.sensor = A6L_H_GYRO; e.type = SENSOR_TYPE_GYROSCOPE;
		a6l_evq_push(&m->q, &e);
		e.sensor = A6L_H_ACCEL; e.type = SENSOR_TYPE_ACCELEROMETER;
		a6l_evq_push(&m->q, &e);
		CHECK(!a6l_motion_flush(m, A6L_H_ACCEL) && !a6l_motion_flush(m, A6L_H_ACCEL), "flush accel x2 (pending)");
		CHECK(!a6l_motion_activate(m, A6L_H_ACCEL, 0), "deactivate accel with 2 queued samples + 2 pending flushes");
		int n = a6l_motion_pop(m, out, 8), acc = 0, fc = 0, gyr = 0;
		for (int i = 0; i < n; i++) {
			acc += out[i].sensor == A6L_H_ACCEL && out[i].type != SENSOR_TYPE_META_DATA;
			gyr += out[i].sensor == A6L_H_GYRO;
			fc += out[i].type == SENSOR_TYPE_META_DATA && out[i].meta_data.sensor == A6L_H_ACCEL;
		}
		CHECK(acc == 0 && gyr == 1 && fc == 2 && n == 3,
		      "no accel sample after its disable, gyro kept, both accepted flushes completed (acc %d gyro %d fc %d)",
		      acc, gyr, fc);
		CHECK(a6l_motion_flush(m, A6L_H_ACCEL) == -EINVAL, "flush after disable -> -EINVAL, nothing queued (%d)", m->q.n);
		memset(&m->q, 0, sizeof(m->q));	/* overflow: a completion between samples survives 2048 more samples */
		e.sensor = A6L_H_GYRO; e.type = SENSOR_TYPE_GYROSCOPE;
		for (int i = 0; i < 5; i++) { e.timestamp = i; a6l_evq_push(&m->q, &e); }
		sensors_event_t fcev;
		memset(&fcev, 0, sizeof(fcev));
		fcev.type = SENSOR_TYPE_META_DATA; fcev.meta_data.what = META_DATA_FLUSH_COMPLETE; fcev.meta_data.sensor = A6L_H_GYRO;
		a6l_evq_push(&m->q, &fcev);
		for (int i = 0; i < A6L_EVQ; i++) { e.timestamp = 100 + i; a6l_evq_push(&m->q, &e); }
		int metas = 0, pos = -1, k = 0, order = 1;
		int64_t last = -1;
		while (a6l_evq_pop(&m->q, &e, 1)) {
			if (e.type == SENSOR_TYPE_META_DATA) { metas++; pos = k; }
			else { order &= e.timestamp > last; last = e.timestamp; }
			k++;
		}
		CHECK(metas == 1 && pos == 0 && k == A6L_EVQ && m->q.dropped == 6 && order,
		      "overflow drops the 6 oldest samples, keeps the FLUSH_COMPLETE first (metas %d at %d, n %d, dropped %ld)",
		      metas, pos, k, m->q.dropped);
		free(m);
		return fails ? 1 : 0;
	}
	if (!strcmp(c, "gyrocal")) {	/* F32: bias vs slow rotation, synthetic 200 Hz gyro/accel + 100 Hz mag */
		/* scenario: bias b, rotation rate wz about gravity (yaw) or wx about X (tilt); mag on/off; accel on/off */
		struct sc { const char *name; double b[3], wz, wx; int mag, acc, want; } sc[] = {
			{ "rest + bias, accel + mag", { 0.02, -0.01, 0.03 }, 0, 0, 1, 1, 1 },
			{ "rest + bias, accel only", { 0.02, -0.01, 0.03 }, 0, 0, 0, 1, 2 },
			{ "slow yaw 0.04 rad/s, accel + mag", { 0, 0, 0 }, 0.04, 0, 1, 1, 2 },
			{ "slow yaw 0.04 rad/s, accel only (reviewer case)", { 0, 0, 0 }, 0.04, 0, 0, 1, 2 },
			{ "slow tilt 0.04 rad/s about X", { 0, 0, 0 }, 0, 0.04, 1, 1, -1 },
			{ "rest, no accel evidence", { 0.02, -0.01, 0.03 }, 0, 0, 1, 0, -2 },
		};
		unsigned seed = 99;
		for (size_t t = 0; t < sizeof(sc) / sizeof(sc[0]); t++) {
			struct a6l_motion *m = calloc(1, sizeof(*m));
			int last = 0;
			for (int k = 0; k <= 240; k++) {	/* 1.2 s */
				double tt = k * 0.005, nz[9];
				int64_t ts = 5000000000LL + (int64_t)k * 5000000;
				for (int i = 0; i < 9; i++) {	/* ~N(0,1) */
					double a = 0;
					for (int j = 0; j < 12; j++) { seed = seed * 1103515245u + 12345u; a += (seed >> 8 & 0xffff) / 65536.0; }
					nz[i] = a - 6;
				}
				double th = sc[t].wx * tt, ps = sc[t].wz * tt;
				double acc[3] = { 0.02 * nz[0], G_TEST * sin(th) + 0.02 * nz[1], G_TEST * cos(th) + 0.02 * nz[2] };
				double mag[3] = { 21 * sin(ps) + 0.3 * nz[3], 21 * cos(ps) + 0.3 * nz[4], -42 + 0.3 * nz[5] };
				double w[3] = { sc[t].b[0] + sc[t].wx + 0.002 * nz[6], sc[t].b[1] + 0.002 * nz[7],
						sc[t].b[2] + sc[t].wz + 0.002 * nz[8] };
				if (sc[t].acc)
					a6l_ring_add(&m->acc_ring, ts, acc);
				if (sc[t].mag && !(k & 1))
					a6l_ring_add(&m->mag_ring, ts, mag);
				a6l_gyro_bias_feed(m, w, ts);
				int r = a6l_gyro_cal_eval(m, ts);
				if (r)
					last = r;
			}
			double cz = sc[t].b[2] + sc[t].wz - (m->gb_valid ? m->gb[2] : 0);
			int ok = last == sc[t].want;
			if (sc[t].want == 1)
				ok &= fabs(m->gb[0] - sc[t].b[0]) < 0.002 && fabs(m->gb[1] - sc[t].b[1]) < 0.002 &&
				      fabs(m->gb[2] - sc[t].b[2]) < 0.002 && m->gb_full;
			else if (sc[t].want == 2)
				ok &= fabs(m->gb[0] - sc[t].b[0]) < 0.002 && fabs(m->gb[1] - sc[t].b[1]) < 0.002 &&
				      fabs(m->gb[2]) < 0.001 && !m->gb_full && fabs(cz - (sc[t].b[2] + sc[t].wz)) < 0.001;
			else
				ok &= !m->gb_valid;
			CHECK(ok, "F32 %s: eval %d (want %d), bias (%.4f %.4f %.4f) full=%d, calibrated z %.4f (true rotation %.2f)",
			      sc[t].name, last, sc[t].want, m->gb[0], m->gb[1], m->gb[2], m->gb_full, cz, sc[t].wz);
			free(m);
		}
		return fails ? 1 : 0;
	}

	double S = getenv("A6L_T_SECS") ? atof(getenv("A6L_T_SECS")) : 1;
	if (getenv("A6L_T_ROOT")) {
		pthread_t ft;
		pthread_create(&ft, NULL, feeder, (void *)getenv("A6L_T_ROOT"));
	}
	open_hal();
	const struct sensor_t *list;
	int n = HAL_MODULE_INFO_SYM.get_sensors_list(&HAL_MODULE_INFO_SYM, &list);

	if (!strcmp(c, "list")) {
		int types[40] = { 0 }, dup = 0;
		for (int i = 0; i < n; i++) {
			for (int k = 0; k < i; k++)
				dup |= list[k].handle == list[i].handle;
			if (list[i].type < 40)
				types[list[i].type]++;
			printf("  sensor %d %-44s type %2d minDelay %6d flags 0x%llx\n", list[i].handle, list[i].name, list[i].type,
			       list[i].minDelay, (unsigned long long)list[i].flags);
		}
		CHECK(n == 8 && !dup, "8 sensors, unique handles (n=%d)", n);
		CHECK(types[SENSOR_TYPE_ACCELEROMETER] == 1 && types[SENSOR_TYPE_GYROSCOPE] == 1 &&
		      types[SENSOR_TYPE_MAGNETIC_FIELD] == 1 && types[SENSOR_TYPE_PROXIMITY] == 1 &&
		      types[SENSOR_TYPE_LIGHT] == 1 && types[SENSOR_TYPE_ACCELEROMETER_UNCALIBRATED] == 1 &&
		      types[SENSOR_TYPE_GYROSCOPE_UNCALIBRATED] == 1 && types[SENSOR_TYPE_MAGNETIC_FIELD_UNCALIBRATED] == 1,
		      "one of each: accel gyro mag prox light + 3 uncalibrated");
		int wake_prox = 0;
		for (int i = 0; i < n; i++)
			if (list[i].type == SENSOR_TYPE_PROXIMITY)
				wake_prox = (list[i].flags & SENSOR_FLAG_WAKE_UP) != 0;
		CHECK(wake_prox, "front proximity stays WAKE-UP");
	} else if (!strcmp(c, "motionoff")) {
		CHECK(n == 2, "persist.vendor.a6l.sensors.motion=0 hides the motion sensors (n=%d)", n);
	} else if (!strcmp(c, "flat")) {
		CHECK(!on(A6L_H_ACCEL, 20, 0) && !on(A6L_H_GYRO, 5, 0) && !on(A6L_H_GYRO_UNCAL, 100, 0) &&
		      !on(A6L_H_ACCEL_UNCAL, 1000, 0) && !on(A6L_H_MAG_UNCAL, 1000, 0),
		      "activate accel 50 Hz, gyro 200 Hz, gyro-uncal 10 Hz, accel-uncal 1 Hz, mag-uncal 1 Hz (F32 yaw evidence)");
		settle();
		int na = count_of(A6L_H_ACCEL, 0), ng = count_of(A6L_H_GYRO, 0), ngu = count_of(A6L_H_GYRO_UNCAL, 0),
		    nau = count_of(A6L_H_ACCEL_UNCAL, 0);
		CHECK(na >= 50 * S * 0.96 && na <= 50 * S + 2, "accel decimated 200->50 Hz over %.0f s: %d events (want ~%.0f)", S, na, 50 * S);
		CHECK(ng >= 200 * S * 0.96 && ng <= 200 * S, "gyro at native 200 Hz: %d events", ng);
		CHECK(ngu >= 10 * S * 0.95 - 1 && ngu <= 10 * S + 1, "gyro-uncal 10 Hz: %d events", ngu);
		CHECK(nau >= S - 1 && nau <= S + 1, "accel-uncal 1 Hz: %d events", nau);
		int64_t dt;
		int mono = mono_ok(A6L_H_ACCEL, &dt);
		CHECK(mono && llabs(dt - 20000000) < 300000, "accel timestamps increasing, <= now, mean spacing %.3f ms", dt / 1e6);
		mono = mono_ok(A6L_H_GYRO, &dt);
		CHECK(mono && llabs(dt - 5000000) < 100000, "gyro timestamps across the u32 tick wrap, spacing %.3f ms", dt / 1e6);
		double s[3] = { 0 }, gs[3] = { 0 }, gb[3] = { 0 };
		int k = 0, kg = 0, gstat = -1;
		for (int i = 0; i < nev; i++) {
			if (ev[i].sensor == A6L_H_ACCEL) {
				for (int j = 0; j < 3; j++) s[j] += ev[i].acceleration.v[j];
				k++;
			}
			if (ev[i].sensor == A6L_H_GYRO && i > nev / 2) {
				for (int j = 0; j < 3; j++) gs[j] += ev[i].gyro.v[j];
				kg++;
				gstat = ev[i].gyro.status;
			}
			if (ev[i].sensor == A6L_H_GYRO_UNCAL) {
				gb[0] = ev[i].uncalibrated_gyro.x_bias;
				gb[1] = ev[i].uncalibrated_gyro.y_bias;
				gb[2] = ev[i].uncalibrated_gyro.z_bias;
			}
		}
		CHECK(k && fabs(s[0] / k) < 0.05 && fabs(s[1] / k) < 0.05 && fabs(s[2] / k - 9.80665) < 0.05,
		      "flat screen-up: accel mean (%.3f %.3f %.3f) m/s2 = (0 0 +g) with map +y+x-z", s[0] / k, s[1] / k, s[2] / k);
		CHECK(fabs(gb[0] - 0.01) < 0.003 && fabs(gb[1] + 0.02) < 0.003 && fabs(gb[2] - 0.005) < 0.003,
		      "gyro bias estimated at rest (%.4f %.4f %.4f), true (0.01 -0.02 0.005)", gb[0], gb[1], gb[2]);
		CHECK(kg && fabs(gs[0] / kg) < 0.003 && fabs(gs[1] / kg) < 0.003 && fabs(gs[2] / kg) < 0.003,
		      "calibrated gyro at rest ~0 (%.4f %.4f %.4f)", gs[0] / kg, gs[1] / kg, gs[2] / kg);
		CHECK(gstat == SENSOR_STATUS_ACCURACY_HIGH, "F32: rest confirmed by accel + mag -> gyro accuracy HIGH (%d)", gstat);
		CHECK(!dev->activate(&dev->v0, A6L_H_ACCEL, 0) && !dev->activate(&dev->v0, A6L_H_GYRO, 0) &&
		      !dev->activate(&dev->v0, A6L_H_GYRO_UNCAL, 0) && !dev->activate(&dev->v0, A6L_H_ACCEL_UNCAL, 0) &&
		      !dev->activate(&dev->v0, A6L_H_MAG_UNCAL, 0),
		      "deactivate all");
		usleep(200000);
		char b[8] = "";
		FILE *f = fopen("sys/iio:device3/buffer/enable", "r");
		if (!f) {
			char p[512];
			snprintf(p, sizeof(p), "%s/iio:device3/buffer/enable", getenv("A6L_SYSROOT"));
			f = fopen(p, "r");
		}
		if (f) { if (!fgets(b, sizeof(b), f)) b[0] = 0; fclose(f); }
		CHECK(b[0] == '0', "accel IIO buffer disabled after the last client left (enable=%c)", b[0]);
	} else if (!strcmp(c, "slowyaw")) {
		/* F32 end to end: table turning at 0.04 rad/s about gravity, gyro bias (0.01 -0.02 0.005) */
		CHECK(!on(A6L_H_GYRO, 5, 0) && !on(A6L_H_GYRO_UNCAL, 100, 0) && !on(A6L_H_MAG_UNCAL, 1000, 0),
		      "activate gyro 200 Hz, gyro-uncal 10 Hz, mag-uncal 1 Hz (accel runs for the calibration only)");
		settle();
		double gs[3] = { 0 }, gb[3] = { 0 };
		int kg = 0, gstat = -1;
		for (int i = nev / 2; i < nev; i++) {
			if (ev[i].sensor == A6L_H_GYRO) {
				for (int j = 0; j < 3; j++) gs[j] += ev[i].gyro.v[j];
				kg++;
				gstat = ev[i].gyro.status;
			}
			if (ev[i].sensor == A6L_H_GYRO_UNCAL)
				for (int j = 0; j < 3; j++) gb[j] = ev[i].uncalibrated_gyro.bias[j];
		}
		CHECK(count_of(A6L_H_ACCEL, 0) == 0, "no accel events without an accel client (%d)", count_of(A6L_H_ACCEL, 0));
		CHECK(fabs(gb[0] - 0.01) < 0.003 && fabs(gb[1] + 0.02) < 0.003 && fabs(gb[2]) < 0.001,
		      "tilt-free axes learned (%.4f %.4f), yaw bias NOT learned from the turn (%.4f)", gb[0], gb[1], gb[2]);
		CHECK(kg && fabs(gs[0] / kg) < 0.003 && fabs(gs[1] / kg) < 0.003 && fabs(gs[2] / kg - 0.045) < 0.003,
		      "calibrated gyro keeps the real 0.04 rad/s turn (+0.005 unlearned bias): z %.4f", kg ? gs[2] / kg : 0);
		CHECK(gstat == SENSOR_STATUS_ACCURACY_MEDIUM, "partial calibration -> accuracy MEDIUM (%d)", gstat);
	} else if (!strcmp(c, "map")) {
		CHECK(!on(A6L_H_ACCEL, 20, 0), "activate accel");
		settle();
		double z = 0; int k = 0;
		for (int i = 0; i < nev; i++) if (ev[i].sensor == A6L_H_ACCEL) { z += ev[i].acceleration.z; k++; }
		CHECK(k && fabs(z / k + 9.80665) < 0.05, "map override +x+y+z (property) gives Z = raw z = -g (%.3f)", k ? z / k : 0);
	} else if (!strcmp(c, "rotate")) {
		CHECK(!on(A6L_H_MAG, 20, 0) && !on(A6L_H_MAG_UNCAL, 20, 0) && !on(A6L_H_ACCEL, 20, 0) && !on(A6L_H_GYRO, 20, 0),
		      "activate mag + mag-uncal + accel + gyro at 50 Hz");
		settle();
		int nm = count_of(A6L_H_MAG, 0);
		CHECK(nm >= 50 * S * 0.96 && nm <= 50 * S + 1, "mag 100->50 Hz: %d events", nm);
		/* last 2 s: calibrated |B| ~ 46.96 uT, bias ~ hard iron, accuracy >= MEDIUM, uncal - bias = cal */
		double bmin = 1e9, bmax = 0, amax = 0, wmax = 0;
		float bias[3] = { 0 };
		int acc = -1, consistent = 1, k = 0, first_acc = -1;
		int64_t tlast = 0;
		for (int i = nev - 1; i >= 0; i--) if (ev[i].sensor == A6L_H_MAG) { tlast = ev[i].timestamp; break; }
		for (int i = 0; i < nev; i++) {
			if (ev[i].sensor == A6L_H_MAG && first_acc < 0) first_acc = ev[i].magnetic.status;
			if (ev[i].timestamp < tlast - 2000000000LL) continue;
			if (ev[i].sensor == A6L_H_MAG) {
				double b = vnorm(ev[i].magnetic.v);
				if (b < bmin) bmin = b;
				if (b > bmax) bmax = b;
				acc = ev[i].magnetic.status;
				k++;
			}
			if (ev[i].sensor == A6L_H_MAG_UNCAL) {
				bias[0] = ev[i].uncalibrated_magnetic.x_bias;
				bias[1] = ev[i].uncalibrated_magnetic.y_bias;
				bias[2] = ev[i].uncalibrated_magnetic.z_bias;
				for (int j = i - 3; j < i + 4; j++)	/* matching calibrated event (same timestamp) */
					if (j >= 0 && j < nev && ev[j].sensor == A6L_H_MAG && ev[j].timestamp == ev[i].timestamp)
						for (int a = 0; a < 3; a++)
							if (fabsf(ev[i].uncalibrated_magnetic.uncalib[a] - bias[a] - ev[j].magnetic.v[a]) > 1e-3f)
								consistent = 0;
			}
			if (ev[i].sensor == A6L_H_ACCEL) { double a = vnorm(ev[i].acceleration.v); if (fabs(a - 9.80665) > amax) amax = fabs(a - 9.80665); }
			if (ev[i].sensor == A6L_H_GYRO) { double w = vnorm(ev[i].gyro.v); if (w > wmax) wmax = w; }
		}
		CHECK(first_acc == SENSOR_STATUS_UNRELIABLE, "no stored bias: first mag event UNRELIABLE (%d)", first_acc);
		CHECK(fabsf(bias[0] - 8) < 1.0f && fabsf(bias[1] + 5) < 1.0f && fabsf(bias[2] - 3) < 1.0f,
		      "hard-iron bias found (%.2f %.2f %.2f) uT, true (8 -5 3)", bias[0], bias[1], bias[2]);
		CHECK(acc >= SENSOR_STATUS_ACCURACY_MEDIUM, "mag accuracy after tumbling = %d (>= MEDIUM)", acc);
		CHECK(k && bmin > 44.5 && bmax < 49.5, "calibrated |B| %.1f..%.1f uT (true 46.96) over the last 2 s", bmin, bmax);
		CHECK(consistent, "uncalibrated - bias == calibrated");
		CHECK(amax < 0.2, "accel |a| stays g while tumbling (max dev %.3f)", amax);
		CHECK(wmax > 1.0, "gyro sees the rotation (max |w| %.2f rad/s)", wmax);
		stop_poller(A6L_H_MAG);	/* no poll() in flight on the freed device (was a flaky use-after-free) */
		dev->common.close(&dev->common);	/* saves if dirty */
		char p[512], b[160] = "";
		snprintf(p, sizeof(p), "%s/mag_bias", getenv("A6L_DATADIR"));
		FILE *f = fopen(p, "r");
		if (f) { if (!fgets(b, sizeof(b), f)) b[0] = 0; fclose(f); }
		CHECK(!strncmp(b, "A6L_MAGCAL 1 ", 13), "bias persisted: %s", b);
		_exit(fails ? 1 : 0);
	} else if (!strcmp(c, "reload")) {
		CHECK(!on(A6L_H_MAG, 20, 0), "activate mag with the persisted bias");
		usleep(100000);
		settle();
		int first = -1;
		float v0[3] = { 0 };
		for (int i = 0; i < nev; i++)
			if (ev[i].sensor == A6L_H_MAG) { first = ev[i].magnetic.status; memcpy(v0, ev[i].magnetic.v, sizeof(v0)); break; }
		CHECK(first == SENSOR_STATUS_ACCURACY_LOW, "first mag event uses the loaded bias with accuracy LOW (%d)", first);
		CHECK(fabs(vnorm(v0) - 46.96) < 2.5, "first calibrated sample already ~46.96 uT (%.2f)", vnorm(v0));
	} else if (!strcmp(c, "unit")) {
		CHECK(!on(A6L_H_MAG_UNCAL, 100, 0), "activate mag-uncal with mag_unit=ut on gauss data");
		settle();
		double b = 0; int k = 0;
		for (int i = 0; i < nev; i++) if (ev[i].sensor == A6L_H_MAG_UNCAL) { b += vnorm(ev[i].uncalibrated_magnetic.uncalib); k++; }
		CHECK(k && b / k < 1.0, "mag_unit=ut forces no x100 (mean |B| %.3f)", k ? b / k : 0);
	} else if (!strcmp(c, "flush")) {
		CHECK(dev->flush(dev, A6L_H_GYRO) == -EINVAL, "flush of an inactive continuous sensor -> -EINVAL");
		CHECK(!on(A6L_H_ACCEL, 10, 2000), "activate accel 100 Hz, latency 2 s (watermark)");
		settle();
		int before = nev;
		CHECK(!dev->flush(dev, A6L_H_ACCEL) && !dev->flush(dev, A6L_H_ACCEL), "flush x2");
		settle();
		int metas = 0, order = 1;
		for (int i = before; i < nev; i++)
			if (ev[i].type == SENSOR_TYPE_META_DATA) {
				metas += ev[i].meta_data.what == META_DATA_FLUSH_COMPLETE && ev[i].meta_data.sensor == A6L_H_ACCEL;
			} else if (metas) {
				order = 0;
			}
		CHECK(metas == 2 && order, "2 FLUSH_COMPLETE for accel, after its data (%d)", metas);
		char p[512], b[16] = "";
		snprintf(p, sizeof(p), "%s/iio:device3/buffer/watermark", getenv("A6L_SYSROOT"));
		FILE *f = fopen(p, "r");
		if (f) { if (!fgets(b, sizeof(b), f)) b[0] = 0; fclose(f); }
		CHECK(atoi(b) > 1, "latency 2 s raised the kfifo watermark to %d", atoi(b));
		CHECK(!dev->batch(dev, A6L_H_ACCEL, 0, 10000000, 0), "batch latency 0 while active");
		usleep(300000);
		f = fopen(p, "r");
		b[0] = 0;
		if (f) { if (!fgets(b, sizeof(b), f)) b[0] = 0; fclose(f); }
		CHECK(atoi(b) == 1, "latency 0 re-enables the buffer with watermark 1 (%d)", atoi(b));
	} else if (!strcmp(c, "missing")) {
		/* r5 F4: a missing/late node keeps the activation (background retry) instead of failing it */
		CHECK(dev->activate(&dev->v0, A6L_H_MAG, 1) == 0, "no qcom-smgr-mag yet -> activate remembered (0), retried");
		CHECK(!on(A6L_H_ACCEL, 10, 0), "accel (100 Hz) still works");
		settle();
		CHECK(count_of(A6L_H_ACCEL, 0) > 100 * S * 0.9, "accel events %d", count_of(A6L_H_ACCEL, 0));
		CHECK(count_of(A6L_H_MAG, 0) == 0, "no mag events while the node is absent (%d)", count_of(A6L_H_MAG, 0));
	} else if (!strcmp(c, "readerr")) {
		/* r5 F4: the only active motion FD fails with a read error (EISDIR, same branch as EIO/ENODEV), light and
		 * proximity off: the retry must run by itself (old code: poll(wake pipe only, -1) forever) */
		char p[600];
		snprintf(p, sizeof(p), "%s/iio:device3", getenv("A6L_DEVROOT"));
		CHECK(!on(A6L_H_ACCEL, 20, 0), "activate accel only");
		int k0 = wait_count(A6L_H_ACCEL, 0, 25, 3000);
		CHECK(k0 >= 25, "accel flowing before the fault (%d)", k0);
		int hfd = hal_fd_of(p), dfd = open(getenv("A6L_SYSROOT"), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
		CHECK(hfd >= 0 && dfd >= 0 && dup2(dfd, hfd) == hfd, "inject: HAL accel fd %d now reads EISDIR", hfd);
		close(dfd);
		usleep(300000);	/* the HAL hits the error, closes the device, schedules a 1 s retry */
		pthread_mutex_lock(&lk);
		int mark = nev;
		pthread_mutex_unlock(&lk);
		int k1 = wait_count(A6L_H_ACCEL, mark, 25, 4000);
		CHECK(k1 >= 25, "accel recovered automatically after the read error, no second activate (%d events)", k1);
	} else if (!strcmp(c, "late")) {
		/* r5 F4: HAL starts before ADSP/SMGR: the accel node appears ~1 s after activate */
		char a[600], b[600];
		snprintf(a, sizeof(a), "%s/iio:device3", getenv("A6L_SYSROOT"));
		snprintf(b, sizeof(b), "%s/late-iio3", getenv("A6L_T_ROOT"));
		CHECK(!rename(a, b), "hide the accel IIO node");
		CHECK(!on(A6L_H_ACCEL, 20, 0), "activate accel with the node absent -> 0 (request remembered)");
		usleep(1000000);
		CHECK(count_of(A6L_H_ACCEL, 0) == 0, "no accel events while absent (%d)", count_of(A6L_H_ACCEL, 0));
		CHECK(!rename(b, a), "node appears (late ADSP)");
		int k = wait_count(A6L_H_ACCEL, 0, 25, 5000);
		CHECK(k >= 25, "accel starts by itself within the 2 s retry, no second activate (%d events)", k);
	} else if (!strcmp(c, "proxfd")) {
		/* r5 F3/F4: proximity with no valid reading and no event fd at activate, both recovering later */
		char raw[600], hold[600], fifo[600];
		float d = -1;
		snprintf(raw, sizeof(raw), "%s/iio:device7/in_proximity_raw", getenv("A6L_SYSROOT"));
		snprintf(hold, sizeof(hold), "%s/prox-raw.hold", getenv("A6L_T_ROOT"));
		snprintf(fifo, sizeof(fifo), "%s/iio:device7", getenv("A6L_DEVROOT"));
		CHECK(rd1("iio:device7/events/in_proximity_thresh_rising_en") == '0', "HAL open disables the PS interrupt (no client)");
		CHECK(!rename(raw, hold), "in_proximity_raw unreadable, no event chardev");
		CHECK(!dev->activate(&dev->v0, A6L_H_PROX, 1), "activate proximity -> 0");
		CHECK(rd1("iio:device7/events/in_proximity_thresh_rising_en") == '1' &&
		      rd1("iio:device7/events/in_proximity_thresh_falling_en") == '1', "activate enables the PS interrupt (wake path)");
		usleep(600000);
		CHECK(last_prox(0, &d) == 0, "no initial event without a valid reading (%d)", last_prox(0, &d));
		CHECK(!rename(hold, raw), "reading becomes valid (raw 40 < 600)");
		usleep(600000);
		int k = last_prox(0, &d);
		CHECK(k == 1 && d == 5.0f, "initial event kept until valid: 1 event, far (%d, %.1f)", k, d);
		CHECK(!mkfifo(fifo, 0600), "event chardev appears (FIFO stand-in)");
		int w = -1;
		for (int t = 0; t < 40 && w < 0; t++) {	/* ENXIO until the HAL has reopened it */
			w = open(fifo, O_WRONLY | O_NONBLOCK | O_CLOEXEC);
			if (w < 0)
				usleep(100000);
		}
		CHECK(w >= 0, "HAL reopened the event fd from its poll loop (no second activate)");
		usleep(400000);
		k = last_prox(0, &d);
		CHECK(k == 2 && d == 5.0f, "state re-reported after event fd recovery (%d, %.1f)", k, d);
		struct iio_event_data e = { .id = ((uint64_t)IIO_EV_TYPE_THRESH << 56) | ((uint64_t)IIO_EV_DIR_RISING << 48) |
						  ((uint64_t)IIO_PROXIMITY << 32) | 1, .timestamp = 1 };
		CHECK(w >= 0 && write(w, &e, sizeof(e)) == sizeof(e), "kernel pushes a near (rising) event");
		usleep(400000);
		k = last_prox(0, &d);
		CHECK(k == 3 && d == 0.0f, "near delivered through the recovered fd (%d, %.1f)", k, d);
		CHECK(!dev->activate(&dev->v0, A6L_H_PROX, 0) && rd1("iio:device7/events/in_proximity_thresh_rising_en") == '0',
		      "deactivate disables the PS interrupt (driver may fully standby in suspend)");
		if (w >= 0)
			close(w);
	} else if (!strcmp(c, "psint")) {
		/* r5 round6 F51: failed PS interrupt enable/disable = pending request retried from the poll loop */
		char en[2][600], sv[2][600], fifo[600];
		float d = -1;
		const char *dirn[2] = { "rising", "falling" };
		for (int i = 0; i < 2; i++) {
			snprintf(en[i], sizeof(en[i]), "%s/iio:device7/events/in_proximity_thresh_%s_en", getenv("A6L_SYSROOT"), dirn[i]);
			snprintf(sv[i], sizeof(sv[i]), "%s.saved", en[i]);
		}
		snprintf(fifo, sizeof(fifo), "%s/iio:device7", getenv("A6L_DEVROOT"));
		CHECK(!mkfifo(fifo, 0600), "event chardev present (FIFO stand-in)");
		CHECK(rd1("iio:device7/events/in_proximity_thresh_rising_en") == '0', "HAL open disables the PS interrupt");
#define BREAK_EN() do { for (int i = 0; i < 2; i++) { rename(en[i], sv[i]); mkdir(en[i], 0700); } } while (0)
#define FIX_EN() do { for (int i = 0; i < 2; i++) { rmdir(en[i]); rename(sv[i], en[i]); } } while (0)
		BREAK_EN();
		CHECK(!dev->activate(&dev->v0, A6L_H_PROX, 1), "activate proximity with the interrupt enable failing -> 0 (request kept)");
		usleep(700000);
		int k = last_prox(0, &d);
		CHECK(k == 1 && d == 5.0f, "initial state reported (%d, %.1f)", k, d);
		FIX_EN();
		int t;
		for (t = 0; t < 40 && !(rd1("iio:device7/events/in_proximity_thresh_rising_en") == '1' &&
					rd1("iio:device7/events/in_proximity_thresh_falling_en") == '1'); t++)
			usleep(100000);
		CHECK(t < 40, "interrupt enable applied by the poll loop after recovery, no second activate (%d ms)", t * 100);
		usleep(500000);
		k = last_prox(0, &d);
		CHECK(k == 2 && d == 5.0f, "current state re-reported after the interrupt recovered (%d, %.1f)", k, d);
		int w = open(fifo, O_WRONLY | O_NONBLOCK | O_CLOEXEC);
		struct iio_event_data e = { .id = ((uint64_t)IIO_EV_TYPE_THRESH << 56) | ((uint64_t)IIO_EV_DIR_RISING << 48) |
						  ((uint64_t)IIO_PROXIMITY << 32) | 1, .timestamp = 1 };
		CHECK(w >= 0 && write(w, &e, sizeof(e)) == sizeof(e), "kernel pushes a near event");
		usleep(400000);
		k = last_prox(0, &d);
		CHECK(k == 3 && d == 0.0f, "near delivered (%d, %.1f)", k, d);
		/* failed disable: retried until applied */
		BREAK_EN();
		CHECK(!dev->activate(&dev->v0, A6L_H_PROX, 0), "deactivate with the interrupt disable failing -> 0");
		usleep(300000);
		FIX_EN();
		for (t = 0; t < 60 && rd1("iio:device7/events/in_proximity_thresh_rising_en") != '0'; t++)
			usleep(100000);
		CHECK(t < 60 && rd1("iio:device7/events/in_proximity_thresh_falling_en") == '0',
		      "interrupt disable applied by the poll loop (%d ms)", t * 100);
		/* enable failing then immediately disabled: the pending enable must not be applied later */
		BREAK_EN();
		CHECK(!dev->activate(&dev->v0, A6L_H_PROX, 1) && !dev->activate(&dev->v0, A6L_H_PROX, 0), "enable (failing) + disable");
		FIX_EN();
		usleep(1500000);
		CHECK(rd1("iio:device7/events/in_proximity_thresh_rising_en") == '0' &&
		      rd1("iio:device7/events/in_proximity_thresh_falling_en") == '0', "stays disabled (pending enable cancelled)");
		if (w >= 0)
			close(w);
	} else if (!strcmp(c, "unreg")) {
		/* r5 round6 F4: IIO unregister gives no poll error; re-register at another index, proximity the only client */
		char fifo[600], fifo2[600], sys7[600], sys9[600];
		float d = -1;
		snprintf(fifo, sizeof(fifo), "%s/iio:device7", getenv("A6L_DEVROOT"));
		snprintf(fifo2, sizeof(fifo2), "%s/iio:device9", getenv("A6L_DEVROOT"));
		snprintf(sys7, sizeof(sys7), "%s/iio:device7", getenv("A6L_SYSROOT"));
		snprintf(sys9, sizeof(sys9), "%s/iio:device9", getenv("A6L_SYSROOT"));
		CHECK(!mkfifo(fifo, 0600), "event chardev present (FIFO stand-in)");
		CHECK(!dev->activate(&dev->v0, A6L_H_PROX, 1), "activate proximity (only client)");
		usleep(500000);
		int k = last_prox(0, &d);
		CHECK(k == 1, "initial event delivered (%d)", k);
		struct rusage r0, r1;
		getrusage(RUSAGE_SELF, &r0);
		/* unregister: the old event fd stays open and silent (like iio_event_poll's empty mask); device comes back as 9 */
		CHECK(!rename(sys7, sys9), "driver unregistered + re-registered as iio:device9");
		FILE *f;
		char p9[700];
		for (int i = 0; i < 2; i++) {
			snprintf(p9, sizeof(p9), "%s/events/in_proximity_thresh_%s_en", sys9, i ? "falling" : "rising");
			if ((f = fopen(p9, "w"))) { fputs("0\n", f); fclose(f); }	/* fresh probe state != requested */
		}
		CHECK(!mkfifo(fifo2, 0600), "new event chardev");
		int w = -1;
		for (int t = 0; t < 80 && w < 0; t++) {
			w = open(fifo2, O_WRONLY | O_NONBLOCK | O_CLOEXEC);
			if (w < 0)
				usleep(100000);
		}
		CHECK(w >= 0, "HAL re-discovered the device by name and opened the new event fd (no activate, no other client)");
		usleep(600000);
		CHECK(rd1("iio:device9/events/in_proximity_thresh_rising_en") == '1', "requested interrupt state re-applied on the new device");
		k = last_prox(0, &d);
		CHECK(k == 2 && d == 5.0f, "fresh current state after recovery (%d, %.1f)", k, d);
		struct iio_event_data e = { .id = ((uint64_t)IIO_EV_TYPE_THRESH << 56) | ((uint64_t)IIO_EV_DIR_RISING << 48) |
						  ((uint64_t)IIO_PROXIMITY << 32) | 1, .timestamp = 1 };
		CHECK(w >= 0 && write(w, &e, sizeof(e)) == sizeof(e), "near event on the new device");
		usleep(400000);
		k = last_prox(0, &d);
		CHECK(k == 3 && d == 0.0f, "near delivered (%d, %.1f)", k, d);
		getrusage(RUSAGE_SELF, &r1);
		double cpu = (r1.ru_utime.tv_sec - r0.ru_utime.tv_sec) + (r1.ru_utime.tv_usec - r0.ru_utime.tv_usec) / 1e6 +
			     (r1.ru_stime.tv_sec - r0.ru_stime.tv_sec) + (r1.ru_stime.tv_usec - r0.ru_stime.tv_usec) / 1e6;
		CHECK(cpu < 1.0, "no spinning while lost/recovering (cpu %.3f s)", cpu);
		if (w >= 0)
			close(w);
	} else if (!strcmp(c, "latestk")) {
		/* r5 bug hunt S1/S2: STK3338 probes AFTER the HAL opened (boot order: class hal before the adsp module group).
		 * The driver probes with INT_PS enabled; with no proximity client the HAL must still disable it (else PS keeps
		 * sensing and stays wake-armed in every suspend), and an activation made before the probe must not be lost. */
		char sys7[600], p[700];
		float d = -1;
		snprintf(sys7, sizeof(sys7), "%s/iio:device7", getenv("A6L_SYSROOT"));
		CHECK(dev->activate(&dev->v0, A6L_H_LIGHT, 1) == 0, "activate light before the STK3338 exists -> 0 (kept, retried)");
		usleep(300000);
		mkdir(sys7, 0700);
		snprintf(p, sizeof(p), "%s/events", sys7);
		mkdir(p, 0700);
		static const char *files[][2] = { { "in_illuminance_raw", "100\n" }, { "in_illuminance_scale", "0.5\n" },
			{ "in_proximity_raw", "40\n" }, { "events/in_proximity_thresh_rising_value", "600\n" },
			{ "events/in_proximity_thresh_rising_en", "1\n" }, { "events/in_proximity_thresh_falling_en", "1\n" },
			{ "name", "stk3338\n" } };
		for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
			snprintf(p, sizeof(p), "%s/%s", sys7, files[i][0]);
			FILE *f = fopen(p, "w");
			if (f) { fputs(files[i][1], f); fclose(f); }
		}
		int t;
		for (t = 0; t < 40 && !(rd1("iio:device7/events/in_proximity_thresh_rising_en") == '0' &&
					rd1("iio:device7/events/in_proximity_thresh_falling_en") == '0'); t++)
			usleep(100000);
		CHECK(t < 40, "late STK3338: PS interrupt disabled by the HAL without any proximity client (%d ms)", t * 100);
		int k = wait_count(A6L_H_LIGHT, 0, 1, 3000);
		float lux = -1;
		pthread_mutex_lock(&lk);
		for (int i = 0; i < nev; i++) if (ev[i].sensor == A6L_H_LIGHT) lux = ev[i].light;
		pthread_mutex_unlock(&lk);
		CHECK(k == 1 && lux == 50.0f, "light activated before the probe reports once the device exists (%d, %.1f lux)", k, lux);
		CHECK(last_prox(0, &d) == 0, "no proximity event without a proximity client");
		CHECK(dev->flush(dev, A6L_H_PROX) == -EINVAL, "flush of the inactive proximity sensor -> -EINVAL (sensors.h)");
		CHECK(dev->flush(dev, A6L_H_LIGHT) == 0, "flush of the active light sensor -> 0");
		CHECK(!dev->activate(&dev->v0, A6L_H_LIGHT, 0), "deactivate light");
		CHECK(dev->flush(dev, A6L_H_LIGHT) == -EINVAL, "flush after deactivate -> -EINVAL");
	} else if (!strcmp(c, "lostoff")) {
		/* r5 bug hunt S2: a client disables while the IIO device is lost; the disable must stick after re-discovery */
		char sys7[600], gone[600], p[700];
		float d = -1;
		snprintf(sys7, sizeof(sys7), "%s/iio:device7", getenv("A6L_SYSROOT"));
		snprintf(gone, sizeof(gone), "%s/gone7", getenv("A6L_SYSROOT"));
		CHECK(!dev->activate(&dev->v0, A6L_H_LIGHT, 1), "activate light");
		CHECK(wait_count(A6L_H_LIGHT, 0, 1, 2000) == 1, "first light event");
		CHECK(!rename(sys7, gone), "STK3338 unregistered (sysfs node gone)");
		usleep(700000);	/* next 200 ms light read notices the loss */
		CHECK(dev->activate(&dev->v0, A6L_H_LIGHT, 0) == 0, "deactivate light while the device is lost -> 0");
		CHECK(dev->activate(&dev->v0, A6L_H_PROX, 0) == 0, "deactivate (inactive) proximity while lost -> 0");
		for (int i = 0; i < 2; i++) {	/* fresh probe: driver default INT_PS = 1, new lux value */
			snprintf(p, sizeof(p), "%s/events/in_proximity_thresh_%s_en", gone, i ? "falling" : "rising");
			FILE *f = fopen(p, "w");
			if (f) { fputs("1\n", f); fclose(f); }
		}
		snprintf(p, sizeof(p), "%s/in_illuminance_raw", gone);
		FILE *f = fopen(p, "w");
		if (f) { fputs("300\n", f); fclose(f); }
		pthread_mutex_lock(&lk);
		int base = nev;
		pthread_mutex_unlock(&lk);
		CHECK(!rename(gone, sys7), "device re-registered");
		int t;
		for (t = 0; t < 40 && rd1("iio:device7/events/in_proximity_thresh_rising_en") != '0'; t++)
			usleep(100000);
		CHECK(t < 40, "re-probed device: PS interrupt disabled again (no proximity client) (%d ms)", t * 100);
		usleep(1500000);
		CHECK(wait_count(A6L_H_LIGHT, base, 1, 100) == 0, "no light event for the disabled light sensor after re-discovery (%d)",
		      count_of(A6L_H_LIGHT, base));
		CHECK(last_prox(base, &d) == 0, "no proximity event");
	} else if (!strcmp(c, "light")) {
		CHECK(!on(A6L_H_LIGHT, 200, 0), "activate STK light");
		CHECK(!on(A6L_H_ACCEL, 20, 0), "and accel together");
		settle();
		float lux = -1;
		for (int i = 0; i < nev; i++) if (ev[i].sensor == A6L_H_LIGHT) lux = ev[i].light;
		CHECK(lux == 50.0f, "light = raw 100 x scale 0.5 = %.1f lux", lux);
		CHECK(count_of(A6L_H_ACCEL, 0) > 50 * S * 0.95, "accel unaffected by the light polling (%d)", count_of(A6L_H_ACCEL, 0));
		CHECK(count_of(A6L_H_LIGHT, 0) == 1, "light on-change: 1 event for a constant value (%d)", count_of(A6L_H_LIGHT, 0));
	} else {
		printf("unknown case %s\n", c);
		return 2;
	}
	printf("A6L_T_DONE %s fails=%d\n", c, fails);
	_exit(fails ? 1 : 0);
}
