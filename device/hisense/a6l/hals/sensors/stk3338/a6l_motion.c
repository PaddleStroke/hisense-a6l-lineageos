/*
 * sensors.a6l: accelerometer / gyroscope / magnetometer (+ uncalibrated variants) from the ADSP SMGR IIO devices
 * (agent senshal, 26 Sep 2026; logic proven offline against the sens agent's a6l_imu reader, see docs/senshal-20260926.md).
 *  - one IIO kfifo buffer per physical sensor, enabled while any of its handles is active; SMGR always samples at the
 *    native maximum, requested rates are served by decimation; maxReportLatency only raises the kfifo watermark
 *    (no FIFO that survives AP suspend: fifoMaxEventCount = 0);
 *  - units: accel m/s^2, gyro rad/s (scale 1/65536), magnetometer gauss x100 -> uT (auto-detected, property);
 *  - axes: SMGR frame (NED) -> Android with the QTI map "+y+x-z" (Android X = raw y, Y = raw x, Z = -raw z),
 *    persist.vendor.a6l.sensors.map[.accel|.gyro|.mag] overrides;
 *  - timestamps: SMGR DSP ticks (u32, 32768 Hz by default, measured and corrected after 3 s) mapped to CLOCK_BOOTTIME
 *    with a minimum-latency filter; strictly increasing per sensor and never in the future;
 *  - magnetometer hard-iron calibration: a6l_magcal.c; gyroscope bias: stationary detector (uncalibrated variant
 *    reports it).
 * r5 deep review (docs/hardware-review-deep-20260928.md):
 *  - F30: a drain reads the whole backlog before anchoring the clock on its newest scan; rate decimation runs on the
 *    DSP acquisition time, so the monotonicity repair (+1 us) can no longer make valid samples look "too close";
 *  - F31: disable purges the handle's queued samples and turns pending flushes into their FLUSH_COMPLETE; scans older
 *    than the activation are not emitted; queue overflow never drops a flush completion while samples remain;
 *  - F32: a gyro-stable window only becomes bias when the accelerometer shows no tilt (components perpendicular to
 *    gravity) and, for the component along gravity (yaw), the magnetometer shows no heading change; the accel device
 *    runs while a gyro handle is active; accuracy HIGH only after a fully confirmed estimate.
 */
#define LOG_TAG "sensors.a6l"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <log/log.h>

#include "a6l_motion.h"

#ifndef A6L_HOST_TEST
#include <cutils/properties.h>
int a6l_prop(const char *key, char *val, const char *def)
{
	return property_get(key, val, def);
}
const char *a6l_sysroot(void) { return "/sys/bus/iio/devices"; }
const char *a6l_devroot(void) { return "/dev"; }
const char *a6l_datadir(void) { return "/data/vendor/sensors"; }
#else
/* host tests: persist.vendor.a6l.sensors.map.accel -> env A6L_PROP_persist_vendor_a6l_sensors_map_accel */
int a6l_prop(const char *key, char *val, const char *def)
{
	char env[128] = "A6L_PROP_";
	size_t n = strlen(env);
	for (const char *k = key; *k && n < sizeof(env) - 1; k++)
		env[n++] = (*k == '.') ? '_' : *k;
	env[n] = 0;
	const char *v = getenv(env);
	snprintf(val, 92, "%s", v ? v : def);
	return (int)strlen(val);
}
static const char *envor(const char *k, const char *d) { const char *v = getenv(k); return v ? v : d; }
const char *a6l_sysroot(void) { return envor("A6L_SYSROOT", "/sys/bus/iio/devices"); }
const char *a6l_devroot(void) { return envor("A6L_DEVROOT", "/dev"); }
const char *a6l_datadir(void) { return envor("A6L_DATADIR", "/data/vendor/sensors"); }
#endif

int64_t a6l_now_ns(void)
{
	struct timespec t;
	clock_gettime(CLOCK_BOOTTIME, &t);
	return (int64_t)t.tv_sec * 1000000000LL + t.tv_nsec;
}

/* ---------------------------------------------------------------- sensor list */
#define G 9.80665f
static const struct sensor_t g_motion[] = {
	{ .name = "BMI160 Accelerometer (SMGR)", .vendor = "Bosch", .version = 1, .handle = A6L_H_ACCEL,
	  .type = SENSOR_TYPE_ACCELEROMETER, .maxRange = 8 * G, .resolution = 8 * G / 32768, .power = 0.18f,
	  .minDelay = 5000, .maxDelay = 1000000, .stringType = SENSOR_STRING_TYPE_ACCELEROMETER, .requiredPermission = "",
	  .flags = SENSOR_FLAG_CONTINUOUS_MODE },
	{ .name = "BMI160 Gyroscope (SMGR)", .vendor = "Bosch", .version = 1, .handle = A6L_H_GYRO,
	  .type = SENSOR_TYPE_GYROSCOPE, .maxRange = 34.906586f, .resolution = 34.906586f / 32768, .power = 0.9f,
	  .minDelay = 5000, .maxDelay = 1000000, .stringType = SENSOR_STRING_TYPE_GYROSCOPE, .requiredPermission = "",
	  .flags = SENSOR_FLAG_CONTINUOUS_MODE },
	{ .name = "AK09918 Magnetometer (SMGR)", .vendor = "AKM", .version = 1, .handle = A6L_H_MAG,
	  .type = SENSOR_TYPE_MAGNETIC_FIELD, .maxRange = 4912.0f, .resolution = 0.15f, .power = 1.1f,
	  .minDelay = 10000, .maxDelay = 1000000, .stringType = SENSOR_STRING_TYPE_MAGNETIC_FIELD, .requiredPermission = "",
	  .flags = SENSOR_FLAG_CONTINUOUS_MODE },
	{ .name = "BMI160 Accelerometer Uncalibrated (SMGR)", .vendor = "Bosch", .version = 1, .handle = A6L_H_ACCEL_UNCAL,
	  .type = SENSOR_TYPE_ACCELEROMETER_UNCALIBRATED, .maxRange = 8 * G, .resolution = 8 * G / 32768, .power = 0.18f,
	  .minDelay = 5000, .maxDelay = 1000000, .stringType = SENSOR_STRING_TYPE_ACCELEROMETER_UNCALIBRATED,
	  .requiredPermission = "", .flags = SENSOR_FLAG_CONTINUOUS_MODE },
	{ .name = "BMI160 Gyroscope Uncalibrated (SMGR)", .vendor = "Bosch", .version = 1, .handle = A6L_H_GYRO_UNCAL,
	  .type = SENSOR_TYPE_GYROSCOPE_UNCALIBRATED, .maxRange = 34.906586f, .resolution = 34.906586f / 32768,
	  .power = 0.9f, .minDelay = 5000, .maxDelay = 1000000, .stringType = SENSOR_STRING_TYPE_GYROSCOPE_UNCALIBRATED,
	  .requiredPermission = "", .flags = SENSOR_FLAG_CONTINUOUS_MODE },
	{ .name = "AK09918 Magnetometer Uncalibrated (SMGR)", .vendor = "AKM", .version = 1, .handle = A6L_H_MAG_UNCAL,
	  .type = SENSOR_TYPE_MAGNETIC_FIELD_UNCALIBRATED, .maxRange = 4912.0f, .resolution = 0.15f, .power = 1.1f,
	  .minDelay = 10000, .maxDelay = 1000000, .stringType = SENSOR_STRING_TYPE_MAGNETIC_FIELD_UNCALIBRATED,
	  .requiredPermission = "", .flags = SENSOR_FLAG_CONTINUOUS_MODE },
};

const struct sensor_t *a6l_motion_list(int *n)
{
	*n = (int)(sizeof(g_motion) / sizeof(g_motion[0]));
	return g_motion;
}

int a6l_motion_owns(int handle)
{
	return handle >= A6L_H_FIRST_MOTION && handle <= A6L_H_LAST_MOTION;
}

static const struct sensor_t *info_of(int handle)
{
	for (size_t i = 0; i < sizeof(g_motion) / sizeof(g_motion[0]); i++)
		if (g_motion[i].handle == handle)
			return &g_motion[i];
	return NULL;
}

/* ---------------------------------------------------------------- event queue */
static int is_meta(const sensors_event_t *e)
{
	return e->type == SENSOR_TYPE_META_DATA;
}

void a6l_evq_push(struct a6l_evq *q, const sensors_event_t *e)
{
	if (q->n == A6L_EVQ) {	/* full: drop the oldest sample, keep flush completions (F31) */
		int j = 0;
		while (j < q->n && is_meta(&q->ev[(q->head + j) % A6L_EVQ]))
			j++;
		if (j == q->n && !is_meta(e)) {	/* only completions queued: the new sample is the expendable one */
			q->dropped++;
			return;
		}
		if (j == q->n)
			j = 0;	/* 2048 completions: nothing but the oldest can go */
		for (int k = j; k > 0; k--)	/* completions ahead of the dropped sample move up one slot, order kept */
			q->ev[(q->head + k) % A6L_EVQ] = q->ev[(q->head + k - 1) % A6L_EVQ];
		q->head = (q->head + 1) % A6L_EVQ;
		q->n--;
		q->dropped++;
	}
	q->ev[(q->head + q->n) % A6L_EVQ] = *e;
	q->n++;
}

int a6l_evq_purge(struct a6l_evq *q, int handle)
{
	int w = 0, removed = 0;
	for (int r = 0; r < q->n; r++) {
		const sensors_event_t *e = &q->ev[(q->head + r) % A6L_EVQ];
		if (!is_meta(e) && e->sensor == handle) {
			removed++;
			continue;
		}
		if (w != r)
			q->ev[(q->head + w) % A6L_EVQ] = *e;
		w++;
	}
	q->n = w;
	return removed;
}

int a6l_evq_pop(struct a6l_evq *q, sensors_event_t *out, int max)
{
	int k = 0;
	while (k < max && q->n) {
		out[k++] = q->ev[q->head];
		q->head = (q->head + 1) % A6L_EVQ;
		q->n--;
	}
	return k;
}

/* ---------------------------------------------------------------- tick -> boottime */
#define TS_CREEP 1e-4	/* offset may grow 100 ppm of elapsed time (clock drift) */

void a6l_tsmap_reset(struct a6l_tsmap *t, double hz)
{
	memset(t, 0, sizeof(*t));
	t->hz = hz > 0 ? hz : 32768;
}

uint64_t a6l_tsmap_unwrap(struct a6l_tsmap *t, uint32_t tick)
{
	if (!t->init) {
		t->init = 1;
		t->last_tick = tick;
		t->ext = (uint64_t)1 << 32;	/* headroom for small backwards steps */
		t->ext_base = t->ext;
		t->offset_ns = NAN;
		return t->ext;
	}
	int32_t d = (int32_t)(tick - t->last_tick);	/* modulo 2^32 */
	t->last_tick = tick;
	t->ext += (int64_t)d;
	return t->ext;
}

int64_t a6l_tsmap_ns(const struct a6l_tsmap *t, uint64_t ext)
{
	double tick_ns = ((double)(int64_t)(ext - t->ext_base)) * 1e9 / t->hz;
	return (int64_t)llround(tick_ns + (isnan(t->offset_ns) ? 0 : t->offset_ns));
}

void a6l_tsmap_anchor(struct a6l_tsmap *t, uint64_t ext_last, int64_t now)
{
	double tick_ns = ((double)(int64_t)(ext_last - t->ext_base)) * 1e9 / t->hz;
	double obs = (double)now - tick_ns;
	if (isnan(t->offset_ns) || obs < t->offset_ns) {
		t->offset_ns = obs;
	} else {
		double grown = t->offset_ns + (double)(now - t->last_anchor_ns) * TS_CREEP;
		t->offset_ns = grown < obs ? grown : obs;
	}
	t->last_anchor_ns = now;
	/* tick-rate check: one measurement over >= 3 s of wall time */
	if (!t->m_now0) {
		t->m_now0 = now;
		t->m_ext0 = ext_last;
	} else if (!t->measured && now - t->m_now0 >= 3000000000LL) {
		double meas = (double)(int64_t)(ext_last - t->m_ext0) * 1e9 / (double)(now - t->m_now0);
		t->measured = 1;
		if (meas > 0 && fabs(meas / t->hz - 1.0) > 0.10) {
			ALOGW("SMGR tick rate %.0f Hz measured, %.0f Hz assumed: using the measured rate", meas, t->hz);
			/* re-base so already-issued timestamps stay continuous */
			double cur = (double)now;
			t->hz = meas;
			t->ext_base = ext_last;
			t->offset_ns = cur;
		}
	}
}

/* ---------------------------------------------------------------- sysfs helpers */
static int rd(const char *path, char *buf, size_t n)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -errno;
	ssize_t r = read(fd, buf, n - 1);
	int e = errno;
	close(fd);
	if (r < 0)
		return -e;
	buf[r] = 0;
	while (r > 0 && (buf[r - 1] == '\n' || buf[r - 1] == ' '))
		buf[--r] = 0;
	return 0;
}

static int wr(const char *path, const char *v)
{
	int fd = open(path, O_WRONLY | O_TRUNC | O_CLOEXEC);
	if (fd < 0)
		return -errno;
	ssize_t r = write(fd, v, strlen(v));
	int e = errno;
	close(fd);
	return r < 0 ? -e : 0;
}

static int rdf(const struct a6l_iio *d, const char *rel, char *buf, size_t n)
{
	char p[320];
	snprintf(p, sizeof(p), "%s/%s", d->sys, rel);
	return rd(p, buf, n);
}

static int wrf(const struct a6l_iio *d, const char *rel, const char *v)
{
	char p[320];
	snprintf(p, sizeof(p), "%s/%s", d->sys, rel);
	return wr(p, v);
}

static int find_iio(struct a6l_iio *d)
{
	char p[320], nm[64];
	if (d->sys[0]) {	/* still the same device? (module reloads renumber) */
		snprintf(p, sizeof(p), "%s/name", d->sys);
		if (!rd(p, nm, sizeof(nm)) && !strcmp(nm, d->iio_name))
			return 0;
		d->sys[0] = 0;
	}
	DIR *dir = opendir(a6l_sysroot());
	struct dirent *e;
	if (!dir)
		return -ENODEV;
	while ((e = readdir(dir))) {
		if (strncmp(e->d_name, "iio:device", 10))
			continue;
		snprintf(p, sizeof(p), "%s/%s/name", a6l_sysroot(), e->d_name);
		if (!rd(p, nm, sizeof(nm)) && !strcmp(nm, d->iio_name)) {
			snprintf(d->sys, sizeof(d->sys), "%s/%s", a6l_sysroot(), e->d_name);
			break;
		}
	}
	closedir(dir);
	return d->sys[0] ? 0 : -ENODEV;
}

/* "le:s32/32>>0" */
static int parse_type(const char *t, struct a6l_chan *c)
{
	char e, sg;
	if (sscanf(t, "%ce:%c%d/%d>>%d", &e, &sg, &c->bits, &c->bytes, &c->shift) != 5 || (e != 'l' && e != 'b') ||
	    (sg != 's' && sg != 'u') || c->bits <= 0 || c->bits > 64 || (c->bytes != 8 && c->bytes != 16 &&
	    c->bytes != 32 && c->bytes != 64) || c->shift < 0 || c->shift >= 64)
		return -1;
	c->be = e == 'b';
	c->sgn = sg == 's';
	c->bytes /= 8;
	return 0;
}

static int parse_map(const char *m, int src[3], int sgn[3])
{
	int used = 0, s[3], g[3];
	if (strlen(m) != 6)
		return -1;
	for (int i = 0; i < 3; i++) {
		char sg = m[2 * i], ax = m[2 * i + 1];
		if ((sg != '+' && sg != '-') || ax < 'x' || ax > 'z')
			return -1;
		g[i] = sg == '-' ? -1 : 1;
		s[i] = ax - 'x';
		if (used & (1 << s[i]))
			return -1;
		used |= 1 << s[i];
	}
	memcpy(src, s, sizeof(s));
	memcpy(sgn, g, sizeof(g));
	return 0;
}

/* ---------------------------------------------------------------- init */
#define DEFAULT_MAP "+y+x-z"

void a6l_motion_init(struct a6l_motion *m)
{
	static const char *const names[A6L_NDEV] = { "qcom-smgr-accel", "qcom-smgr-gyro", "qcom-smgr-mag" };
	static const char *const types[A6L_NDEV] = { "accel", "anglvel", "magn" };
	static const char *const tags[A6L_NDEV] = { "accel", "gyro", "mag" };
	char v[92], key[96], all[92];
	memset(m, 0, sizeof(*m));
	a6l_prop("persist.vendor.a6l.sensors.map", all, DEFAULT_MAP);
	a6l_prop("persist.vendor.a6l.sensors.tick_hz", v, "32768");
	double hz = strtod(v, NULL);
	for (int i = 0; i < A6L_NDEV; i++) {
		struct a6l_iio *d = &m->d[i];
		d->iio_name = names[i];
		d->type = types[i];
		d->tag = tags[i];
		d->fd = -1;
		d->native_ns = i == A6L_MAG ? 10e6 : 5e6;
		a6l_tsmap_reset(&d->tm, hz);
		snprintf(key, sizeof(key), "persist.vendor.a6l.sensors.map.%s", tags[i]);
		a6l_prop(key, v, all);
		if (parse_map(v, d->map_src, d->map_sgn)) {
			ALOGE("bad axis map '%s' for %s, using %s", v, tags[i], DEFAULT_MAP);
			parse_map(DEFAULT_MAP, d->map_src, d->map_sgn);
		}
		d->mul = 1.0;
	}
	a6l_prop("persist.vendor.a6l.sensors.mag_unit", m->mag_unit, "auto");
	if (!strcmp(m->mag_unit, "gauss"))
		m->d[A6L_MAG].mul = 100.0;
	else if (!strcmp(m->mag_unit, "ut"))
		m->d[A6L_MAG].mul = 1.0;
	else
		m->d[A6L_MAG].mul = 0;	/* auto: decided on the first sample */
	for (int i = 0; i < A6L_NMH; i++) {
		struct a6l_mh *h = &m->h[i];
		h->handle = A6L_H_FIRST_MOTION + i;
		switch (h->handle) {
		case A6L_H_ACCEL: case A6L_H_ACCEL_UNCAL: h->dev = A6L_ACC; break;
		case A6L_H_GYRO: case A6L_H_GYRO_UNCAL: h->dev = A6L_GYR; break;
		default: h->dev = A6L_MAG; break;
		}
		h->uncal = h->handle >= A6L_H_ACCEL_UNCAL;
		h->period = (int64_t)info_of(h->handle)->minDelay * 1000 * 4;	/* until batch() */
	}
	a6l_prop("persist.vendor.a6l.sensors.magcal", v, "1");
	m->magcal_on = strcmp(v, "0") != 0;
	snprintf(key, sizeof(key), "%s/mag_bias", a6l_datadir());
	a6l_magcal_init(&m->mc, key);
	if (m->magcal_on)
		a6l_magcal_load(&m->mc);
	ALOGI("motion: map accel %s gyro %s mag %s, mag unit %s, magcal %s (bias %s)", all, all, all, m->mag_unit,
	      m->magcal_on ? "on" : "off", m->mc.have_bias ? "loaded" : "none");
}

static struct a6l_mh *mh_of(struct a6l_motion *m, int handle)
{
	return a6l_motion_owns(handle) ? &m->h[handle - A6L_H_FIRST_MOTION] : NULL;
}

static void push_flush_complete(struct a6l_motion *m, int handle)
{
	sensors_event_t e;
	memset(&e, 0, sizeof(e));
	e.version = META_DATA_VERSION;
	e.type = SENSOR_TYPE_META_DATA;
	e.meta_data.what = META_DATA_FLUSH_COMPLETE;
	e.meta_data.sensor = handle;
	a6l_evq_push(&m->q, &e);
}

/* does handle h keep device dv running? (F32: the accelerometer is the gyro calibration's tilt reference) */
static int consumes(const struct a6l_mh *h, int dv)
{
	return h->enabled && (h->dev == dv || (dv == A6L_ACC && h->dev == A6L_GYR));
}

/* ---------------------------------------------------------------- control (binder threads, caller holds the lock) */
int a6l_motion_activate(struct a6l_motion *m, int handle, int en)
{
	struct a6l_mh *h = mh_of(m, handle);
	if (!h)
		return -EINVAL;
	/* F4 (r5 review): a late IIO node (HAL before ADSP/SMGR, module reload) no longer fails the activation: the request
	 * is remembered and the poll thread's reconcile retries the device every 2 s until it appears */
	if (en && find_iio(&m->d[h->dev]))
		ALOGW("activate %d: no IIO device named %s yet, retrying in the background", handle, m->d[h->dev].iio_name);
	if (en && !h->enabled)
		h->act_ns = a6l_now_ns();	/* F31: scans acquired before this activation belong to no client */
	h->enabled = !!en;
	h->emitted = 0;
	if (!en) {
		/* F31: no sample of this handle is delivered after its disable; every flush() that returned 0 still gets its
		 * FLUSH_COMPLETE (after the purge, so it follows everything that was delivered for the handle) */
		int k = a6l_evq_purge(&m->q, handle);
		if (k)
			ALOGD("deactivate %d: %d queued samples discarded", handle, k);
		while (h->flush_pending > 0) {
			push_flush_complete(m, handle);
			h->flush_pending--;
		}
	}
	return 0;
}

int a6l_motion_batch(struct a6l_motion *m, int handle, int64_t period, int64_t latency)
{
	struct a6l_mh *h = mh_of(m, handle);
	if (!h)
		return -EINVAL;
	const struct sensor_t *s = info_of(handle);
	int64_t mn = (int64_t)s->minDelay * 1000, mx = (int64_t)s->maxDelay * 1000;
	if (period < mn)
		period = mn;
	if (period > mx)
		period = mx;
	h->period = period;
	h->latency = latency > 0 ? latency : 0;
	return 0;
}

int a6l_motion_flush(struct a6l_motion *m, int handle)
{
	struct a6l_mh *h = mh_of(m, handle);
	if (!h || !h->enabled)
		return -EINVAL;	/* inactive continuous sensor */
	h->flush_pending++;
	return 0;
}

/* ---------------------------------------------------------------- buffer control (poll thread) */
static void dev_off(struct a6l_iio *d)
{
	if (d->fd >= 0)
		close(d->fd);
	d->fd = -1;
	if (d->on && d->sys[0])
		wrf(d, "buffer/enable", "0");
	d->on = 0;
}

static int wanted_watermark(const struct a6l_motion *m, int dev)
{
	int64_t lat = -1;
	for (int i = 0; i < A6L_NMH; i++) {
		const struct a6l_mh *h = &m->h[i];
		if (consumes(h, dev) && (lat < 0 || h->latency < lat))
			lat = h->latency;
	}
	if (lat <= 0)
		return 1;
	int64_t n = lat / 2 / (int64_t)m->d[dev].native_ns;	/* half the latency budget */
	return n < 1 ? 1 : (n > 64 ? 64 : (int)n);
}

static int dev_on(struct a6l_iio *d, int watermark)
{
	static const char ax[3] = { 'x', 'y', 'z' };
	char p[160], b[64];
	int e;
	if ((e = find_iio(d)))
		return e;
	wrf(d, "buffer/enable", "0");
	d->nch = 0;
	d->has_ts = 0;
	for (int a = 0; a < 4; a++) {
		struct a6l_chan *c = &d->ch[d->nch];
		char rel[96];
		if (a < 3)
			snprintf(rel, sizeof(rel), "scan_elements/in_%s_%c", d->type, ax[a]);
		else
			snprintf(rel, sizeof(rel), "scan_elements/in_timestamp");
		snprintf(p, sizeof(p), "%s_en", rel);
		if ((e = wrf(d, p, "1"))) {
			if (a < 3) {
				ALOGE("%s: cannot enable %s (%s): check ueventd/a6l-modules chown", d->iio_name, p, strerror(-e));
				return e;
			}
			continue;
		}
		snprintf(p, sizeof(p), "%s_index", rel);
		if (rdf(d, p, b, sizeof(b)))
			return -EIO;
		c->index = atoi(b);
		snprintf(p, sizeof(p), "%s_type", rel);
		if (rdf(d, p, b, sizeof(b)) || parse_type(b, c)) {
			ALOGE("%s: bad scan type '%s'", d->iio_name, b);
			return -EINVAL;
		}
		c->axis = a;
		if (a == 3)
			d->has_ts = 1;
		d->nch++;
	}
	/* IIO layout: scan-index order, each element naturally aligned, record padded to the largest element */
	for (int i = 0; i < d->nch; i++)
		for (int k = i + 1; k < d->nch; k++)
			if (d->ch[k].index < d->ch[i].index) {
				struct a6l_chan t = d->ch[i];
				d->ch[i] = d->ch[k];
				d->ch[k] = t;
			}
	int off = 0, maxb = 1;
	for (int i = 0; i < d->nch; i++) {
		int bb = d->ch[i].bytes;
		off = (off + bb - 1) / bb * bb;
		d->ch[i].off = off;
		off += bb;
		if (bb > maxb)
			maxb = bb;
	}
	d->rec = (off + maxb - 1) / maxb * maxb;
	snprintf(p, sizeof(p), "in_%s_scale", d->type);
	d->scale = rdf(d, p, b, sizeof(b)) ? 1.0 / 65536 : strtod(b, NULL);
	if (!(d->scale > 0))
		d->scale = 1.0 / 65536;
	wrf(d, "buffer/length", "256");
	snprintf(b, sizeof(b), "%d", watermark);
	wrf(d, "buffer/watermark", b);
	snprintf(p, sizeof(p), "%s/%s", a6l_devroot(), strrchr(d->sys, '/') + 1);
	d->fd = open(p, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	if (d->fd < 0) {
		e = -errno;
		ALOGE("%s: open %s: %s", d->iio_name, p, strerror(-e));
		return e;
	}
	d->npart = 0;
#ifdef A6L_HOST_TEST
	lseek(d->fd, 0, SEEK_END);	/* a regular file stands in for the chardev; enabling resets the kfifo */
#endif
	if ((e = wrf(d, "buffer/enable", "1"))) {
		ALOGE("%s: buffer/enable: %s", d->iio_name, strerror(-e));
		close(d->fd);
		d->fd = -1;
		return e;
	}
	d->on = 1;
	d->epoch++;
	d->watermark = watermark;
	d->samples = 0;
	a6l_tsmap_reset(&d->tm, d->tm.hz);	/* keeps a measured tick rate; long gaps must not be unwrapped */
	snprintf(p, sizeof(p), "in_%s_sampling_frequency", d->type);
	if (rdf(d, p, b, sizeof(b)))
		strcpy(b, "?");
	ALOGI("%s on: %s rec=%dB scale=%.9g ts=%d wm=%d rate=%s Hz", d->iio_name, d->sys, d->rec, d->scale, d->has_ts,
	      watermark, b);
	return 0;
}

int a6l_motion_reconcile(struct a6l_motion *m)
{
	int64_t now = a6l_now_ns(), due = -1;
	for (int dv = 0; dv < A6L_NDEV; dv++) {
		struct a6l_iio *d = &m->d[dv];
		int want = 0;
		for (int i = 0; i < A6L_NMH; i++)
			want |= consumes(&m->h[i], dv);
		if (!want) {
			if (d->on || d->fd >= 0)
				dev_off(d);
			d->retry_ns = 0;
			continue;
		}
		int wm = wanted_watermark(m, dv);
		if (d->on && wm < d->watermark)	/* a client needs lower latency now */
			dev_off(d);
		if (d->on)
			continue;
		if (d->retry_ns && now < d->retry_ns) {
			int64_t w = d->retry_ns - now;
			if (due < 0 || w < due)
				due = w;
			continue;
		}
		if (dev_on(d, wm)) {
			dev_off(d);
			d->retry_ns = now + 2000000000LL;	/* ADSP/SMGR late or permissions: retry */
			if (due < 0 || 2000000000LL < due)
				due = 2000000000LL;
		} else {
			d->retry_ns = 0;
		}
	}
	return due < 0 ? -1 : (int)(due / 1000000 + 1);
}

int a6l_motion_retry_ms(const struct a6l_motion *m)
{
	int64_t now = a6l_now_ns(), due = -1;
	for (int dv = 0; dv < A6L_NDEV; dv++) {
		const struct a6l_iio *d = &m->d[dv];
		int want = 0;
		for (int i = 0; i < A6L_NMH; i++)
			want |= consumes(&m->h[i], dv);
		if (!want || d->on)
			continue;
		int64_t w = d->retry_ns > now ? d->retry_ns - now : 0;
		if (due < 0 || w < due)
			due = w;
	}
	return due < 0 ? -1 : (due ? (int)(due / 1000000 + 1) : 0);
}

int a6l_motion_fds(struct a6l_motion *m, struct pollfd *p, int max)
{
	int n = 0;
	for (int dv = 0; dv < A6L_NDEV && n < max; dv++)
		if (m->d[dv].on && m->d[dv].fd >= 0) {
			p[n].fd = m->d[dv].fd;
			p[n].events = POLLIN;
			p[n].revents = 0;
			n++;
		}
	return n;
}

/* ---------------------------------------------------------------- data path */
static int64_t getval(const uint8_t *r, const struct a6l_chan *c)
{
	uint64_t v = 0;
	for (int i = 0; i < c->bytes; i++)
		v |= (uint64_t)r[c->off + (c->be ? c->bytes - 1 - i : i)] << (8 * i);
	v >>= c->shift;
	if (c->bits < 64) {
		v &= (1ULL << c->bits) - 1;
		if (c->sgn && (v >> (c->bits - 1)))
			v |= ~((1ULL << c->bits) - 1);
	}
	return (int64_t)v;
}

void a6l_ring_add(struct a6l_ring *r, int64_t ts, const double v[3])
{
	struct a6l_vs *e = &r->s[r->next];
	e->ts = ts;
	for (int i = 0; i < 3; i++)
		e->v[i] = (float)v[i];
	r->next = (r->next + 1) % A6L_RING;
	if (r->n < A6L_RING)
		r->n++;
}

static int64_t ring_last(const struct a6l_ring *r)
{
	return r->n ? r->s[(r->next + A6L_RING - 1) % A6L_RING].ts : 0;
}

/* least-squares line per axis over the samples in [t0, t1]: mean, slope (unit/s), residual variance */
struct a6l_fit { int n; double mean[3], slope[3], rvar[3], stt; };
static int ring_fit(const struct a6l_ring *r, int64_t t0, int64_t t1, struct a6l_fit *f)
{
	int64_t first = 0, last = 0;
	double tm = 0;
	memset(f, 0, sizeof(*f));
	for (int k = 0; k < r->n; k++) {
		const struct a6l_vs *e = &r->s[k];
		if (e->ts < t0 || e->ts > t1)
			continue;
		if (!f->n || e->ts < first) first = e->ts;
		if (!f->n || e->ts > last) last = e->ts;
		tm += (double)(e->ts - t0) * 1e-9;
		for (int i = 0; i < 3; i++)
			f->mean[i] += e->v[i];
		f->n++;
	}
	/* the window must be covered (>= 20 samples, both ends within 150 ms) */
	if (f->n < 20 || first - t0 > 150000000LL || t1 - last > 150000000LL)
		return -1;
	tm /= f->n;
	for (int i = 0; i < 3; i++)
		f->mean[i] /= f->n;
	double sty[3] = { 0 }, syy[3] = { 0 };
	for (int k = 0; k < r->n; k++) {
		const struct a6l_vs *e = &r->s[k];
		if (e->ts < t0 || e->ts > t1)
			continue;
		double t = (double)(e->ts - t0) * 1e-9 - tm;
		f->stt += t * t;
		for (int i = 0; i < 3; i++) {
			double y = e->v[i] - f->mean[i];
			sty[i] += t * y;
			syy[i] += y * y;
		}
	}
	if (f->stt <= 0)
		return -1;
	for (int i = 0; i < 3; i++) {
		f->slope[i] = sty[i] / f->stt;
		double rv = (syy[i] - f->slope[i] * sty[i]) / (f->n > 2 ? f->n - 2 : 1);
		f->rvar[i] = rv > 0 ? rv : 0;
	}
	return 0;
}

void a6l_gyro_bias_feed(struct a6l_motion *m, const double w[3], int64_t ts)
{
	/* stationary gyro window >= 1 s: every axis sd < 0.01 rad/s and |mean| < 0.05 rad/s. F32: gyro stability alone
	 * cannot tell a bias from a slow constant rotation, so the window is only a candidate (a6l_gyro_cal_eval) */
	double mag = sqrt(w[0] * w[0] + w[1] * w[1] + w[2] * w[2]);
	if (mag > 0.2 || !m->gw_n) {	/* moving (or first sample): restart the window */
		memset(m->gw_sum, 0, sizeof(m->gw_sum));
		memset(m->gw_sum2, 0, sizeof(m->gw_sum2));
		m->gw_n = 0;
		m->gw_t0 = ts;
		if (mag > 0.2)
			return;
	}
	for (int i = 0; i < 3; i++) {
		m->gw_sum[i] += w[i];
		m->gw_sum2[i] += w[i] * w[i];
	}
	m->gw_n++;
	if (ts - m->gw_t0 < 1000000000LL || m->gw_n < 20)
		return;
	int still = 1;
	double mean[3];
	for (int i = 0; i < 3; i++) {
		mean[i] = m->gw_sum[i] / m->gw_n;
		double var = m->gw_sum2[i] / m->gw_n - mean[i] * mean[i];
		if (var > 0.0001 || fabs(mean[i]) > 0.05)
			still = 0;
	}
	if (still) {
		memcpy(m->gc_mean, mean, sizeof(mean));
		m->gc_t0 = m->gw_t0;
		m->gc_t1 = ts;
		m->gc_pending = 1;
	}
	memset(m->gw_sum, 0, sizeof(m->gw_sum));
	memset(m->gw_sum2, 0, sizeof(m->gw_sum2));
	m->gw_n = 0;
	m->gw_t0 = ts;
}

#define GC_TILT_TOL 0.003	/* rad/s allowed on top of 4 sigma of the evidence */
int a6l_gyro_cal_eval(struct a6l_motion *m, int64_t now)
{
	if (!m->gc_pending)
		return 0;
	int64_t t0 = m->gc_t0, t1 = m->gc_t1;
	/* wait (<= 1 s) until the accel / mag scans of the window have been drained too */
	if (now - t1 < 1000000000LL && ((m->d[A6L_ACC].on && ring_last(&m->acc_ring) < t1) ||
					 (m->d[A6L_MAG].on && ring_last(&m->mag_ring) < t1)))
		return 0;
	m->gc_pending = 0;
	struct a6l_fit fa, fm;
	if (ring_fit(&m->acc_ring, t0, t1, &fa)) {
		m->gc_stat[3]++;
		return -2;	/* no independent evidence: keep the old bias */
	}
	double g = sqrt(fa.mean[0] * fa.mean[0] + fa.mean[1] * fa.mean[1] + fa.mean[2] * fa.mean[2]);
	if (fabs(g - G) > 1.0 || fa.rvar[0] + fa.rvar[1] + fa.rvar[2] > 3 * 0.15 * 0.15) {
		m->gc_stat[2]++;
		return -1;	/* accelerating or vibrating */
	}
	double u[3], sp = 0;
	for (int i = 0; i < 3; i++)
		u[i] = fa.mean[i] / g;
	for (int i = 0; i < 3; i++)
		sp += fa.slope[i] * u[i];
	/* gravity turns in the device frame at |w_perp| * g: tilt rate from the accel slope perpendicular to gravity */
	double tilt2 = 0;
	for (int i = 0; i < 3; i++) {
		double c = fa.slope[i] - sp * u[i];
		tilt2 += c * c;
	}
	double tilt = sqrt(tilt2) / g, tilt_sd = sqrt((fa.rvar[0] + fa.rvar[1] + fa.rvar[2]) / fa.stt) / g;
	if (tilt > GC_TILT_TOL + 4 * tilt_sd) {
		m->gc_stat[2]++;
		return -1;	/* the device is turning: the gyro mean is not (only) bias */
	}
	/* yaw (rotation about gravity) is invisible to the accelerometer: the magnetometer's horizontal field turns at
	 * the yaw rate. Without that evidence the along-gravity component keeps its previous value. */
	int yaw_ok = 0;
	if (!ring_fit(&m->mag_ring, t0, t1, &fm)) {
		double b[3], c[3];
		for (int i = 0; i < 3; i++)
			b[i] = fm.mean[i] - (m->magcal_on && m->mc.have_bias ? m->mc.bias[i] : 0);
		c[0] = u[1] * b[2] - u[2] * b[1];	/* u x B: direction B moves in under yaw, |.| = horizontal field */
		c[1] = u[2] * b[0] - u[0] * b[2];
		c[2] = u[0] * b[1] - u[1] * b[0];
		double hz = sqrt(c[0] * c[0] + c[1] * c[1] + c[2] * c[2]);
		if (hz > 5.0) {
			double proj = 0, pvar = 0;
			for (int i = 0; i < 3; i++) {
				proj += fm.slope[i] * c[i] / hz;
				pvar += fm.rvar[i] * (c[i] / hz) * (c[i] / hz);
			}
			double yaw = fabs(proj) / hz, yaw_sd = sqrt(pvar / fm.stt) / hz;
			yaw_ok = yaw_sd < 0.008 && yaw < GC_TILT_TOL + 4 * yaw_sd;
		}
	}
	double par = 0, oldpar = 0, tgt[3];
	for (int i = 0; i < 3; i++) {
		par += m->gc_mean[i] * u[i];
		oldpar += (m->gb_valid ? m->gb[i] : 0) * u[i];
	}
	for (int i = 0; i < 3; i++)
		tgt[i] = m->gc_mean[i] - par * u[i] + (yaw_ok ? par : oldpar) * u[i];
	for (int i = 0; i < 3; i++)
		m->gb[i] = m->gb_valid ? 0.7 * m->gb[i] + 0.3 * tgt[i] : tgt[i];
	m->gb_valid = 1;
	if (yaw_ok)
		m->gb_full = 1;
	m->gc_stat[yaw_ok ? 0 : 1]++;
	return yaw_ok ? 1 : 2;
}

static void emit(struct a6l_motion *m, struct a6l_mh *h, const double v[3], int64_t ts)
{
	sensors_event_t e;
	memset(&e, 0, sizeof(e));
	e.version = sizeof(e);
	e.sensor = h->handle;
	e.type = info_of(h->handle)->type;
	e.timestamp = ts;
	switch (h->handle) {
	case A6L_H_ACCEL:
		e.acceleration.x = (float)v[0];
		e.acceleration.y = (float)v[1];
		e.acceleration.z = (float)v[2];
		e.acceleration.status = SENSOR_STATUS_ACCURACY_HIGH;
		break;
	case A6L_H_GYRO:
		for (int i = 0; i < 3; i++)
			e.gyro.v[i] = (float)(v[i] - (m->gb_valid ? m->gb[i] : 0));
		e.gyro.status = m->gb_full ? SENSOR_STATUS_ACCURACY_HIGH : SENSOR_STATUS_ACCURACY_MEDIUM;
		break;
	case A6L_H_MAG:
		for (int i = 0; i < 3; i++)
			e.magnetic.v[i] = (float)v[i] - (m->magcal_on && m->mc.have_bias ? m->mc.bias[i] : 0);
		e.magnetic.status = m->magcal_on ? m->mc.accuracy : SENSOR_STATUS_UNRELIABLE;
		break;
	case A6L_H_ACCEL_UNCAL:
		e.uncalibrated_accelerometer.x_uncalib = (float)v[0];
		e.uncalibrated_accelerometer.y_uncalib = (float)v[1];
		e.uncalibrated_accelerometer.z_uncalib = (float)v[2];
		break;
	case A6L_H_GYRO_UNCAL:
		e.uncalibrated_gyro.x_uncalib = (float)v[0];
		e.uncalibrated_gyro.y_uncalib = (float)v[1];
		e.uncalibrated_gyro.z_uncalib = (float)v[2];
		if (m->gb_valid) {
			e.uncalibrated_gyro.x_bias = (float)m->gb[0];
			e.uncalibrated_gyro.y_bias = (float)m->gb[1];
			e.uncalibrated_gyro.z_bias = (float)m->gb[2];
		}
		break;
	case A6L_H_MAG_UNCAL:
		e.uncalibrated_magnetic.x_uncalib = (float)v[0];
		e.uncalibrated_magnetic.y_uncalib = (float)v[1];
		e.uncalibrated_magnetic.z_uncalib = (float)v[2];
		if (m->magcal_on && m->mc.have_bias) {
			e.uncalibrated_magnetic.x_bias = m->mc.bias[0];
			e.uncalibrated_magnetic.y_bias = m->mc.bias[1];
			e.uncalibrated_magnetic.z_bias = m->mc.bias[2];
		}
		break;
	}
	a6l_evq_push(&m->q, &e);
}

static void process(struct a6l_motion *m, int dv, const uint8_t *buf, int nrec, int64_t now)
{
	struct a6l_iio *d = &m->d[dv];
	uint64_t ext[A6L_RBATCH];
	int64_t ts[A6L_RBATCH];
	if (nrec > A6L_RBATCH)
		nrec = A6L_RBATCH;
	/* timestamps: the anchor is the newest scan of the whole drained batch (F30) */
	if (d->has_ts) {
		const struct a6l_chan *tc = NULL;
		for (int i = 0; i < d->nch; i++)
			if (d->ch[i].axis == 3)
				tc = &d->ch[i];
		for (int k = 0; k < nrec; k++)
			ext[k] = a6l_tsmap_unwrap(&d->tm, (uint32_t)getval(buf + k * d->rec, tc));
		a6l_tsmap_anchor(&d->tm, ext[nrec - 1], now);
		for (int k = 0; k < nrec; k++)
			ts[k] = a6l_tsmap_ns(&d->tm, ext[k]);
	} else {
		for (int k = 0; k < nrec; k++) {
			ext[k] = 0;
			ts[k] = now - (int64_t)((nrec - 1 - k) * d->native_ns);
		}
	}
	for (int k = 0; k < nrec; k++) {
		const uint8_t *r = buf + k * d->rec;
		double raw[3] = { 0 }, v[3];
		/* native period estimate from the tick spacing */
		if (d->has_ts && d->samples) {
			double dt = ((double)(int64_t)(ext[k] - d->last_ext)) * 1e9 / d->tm.hz;
			if (dt > 5e5 && dt < 1e9)
				d->native_ns = 0.95 * d->native_ns + 0.05 * dt;
		}
		if (d->has_ts)
			d->last_ext = ext[k];
		if (ts[k] > now)
			ts[k] = now;
		if (ts[k] <= d->last_ts)
			ts[k] = d->last_ts + 1000;	/* strictly increasing (repair only: decimation uses ticks) */
		d->last_ts = ts[k];
		d->samples++;
		for (int i = 0; i < d->nch; i++)
			if (d->ch[i].axis < 3)
				raw[d->ch[i].axis] = (double)getval(r, &d->ch[i]) * d->scale;
		for (int i = 0; i < 3; i++)
			v[i] = d->map_sgn[i] * raw[d->map_src[i]];
		if (dv == A6L_MAG) {
			if (d->mul == 0) {
				double n = sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
				d->mul = n < 3.0 ? 100.0 : 1.0;
				ALOGI("mag unit auto: |B|=%.3f -> %s", n, d->mul == 100.0 ? "gauss (x100 -> uT)" : "uT");
			}
			for (int i = 0; i < 3; i++)
				v[i] *= d->mul;
			if (m->magcal_on) {
				float f[3] = { (float)v[0], (float)v[1], (float)v[2] };
				a6l_magcal_add(&m->mc, f, ts[k]);
			}
			a6l_ring_add(&m->mag_ring, ts[k], v);
		} else if (dv == A6L_GYR) {
			a6l_gyro_bias_feed(m, v, ts[k]);
		} else {
			a6l_ring_add(&m->acc_ring, ts[k], v);
		}
		for (int i = 0; i < A6L_NMH; i++) {
			struct a6l_mh *h = &m->h[i];
			if (h->dev != dv || !h->enabled || ts[k] < h->act_ns)
				continue;
			/* F30: rate decimation on the acquisition time (DSP ticks) of the same buffer epoch */
			if (h->emitted && h->emit_epoch == d->epoch) {
				int64_t dt = d->has_ts ? (int64_t)((double)(int64_t)(ext[k] - h->last_emit_ext) * 1e9 / d->tm.hz)
						       : ts[k] - h->last_emit;
				if (dt >= 0 && dt < h->period - (int64_t)(d->native_ns / 2))
					continue;	/* decimation (dt < 0: tick discontinuity, restart the phase) */
			}
			h->emitted = 1;
			h->emit_epoch = d->epoch;
			h->last_emit = ts[k];
			h->last_emit_ext = ext[k];
			emit(m, h, v, ts[k]);
		}
	}
}

/* processes the whole records of buf[0..*tot), keeps the partial tail at the start of buf */
static void batch_done(struct a6l_motion *m, int dv, uint8_t *buf, size_t *tot)
{
	struct a6l_iio *d = &m->d[dv];
	int n = (int)(*tot / d->rec);
	size_t rest = *tot - (size_t)n * d->rec;
	if (n > 0)
		process(m, dv, buf, n, a6l_now_ns());
	memmove(buf, buf + (size_t)n * d->rec, rest);
	*tot = rest;
}

static void drain(struct a6l_motion *m, int dv)
{
	struct a6l_iio *d = &m->d[dv];
	uint8_t *buf = m->rbuf;
	const size_t cap = (size_t)A6L_RBATCH * (d->rec > 0 ? d->rec : 1);
	int err = 0;
	if (!d->on || d->fd < 0 || d->rec <= 0 || d->rec > 64)
		return;
	/* F30: read EVERYTHING available first (IIO reads return whole scans; the carry only matters for a partial
	 * read in host tests), then map the batch to boottime with its newest scan as the anchor. A 64-scan read is
	 * not an acquisition-time boundary. */
	size_t tot = (size_t)d->npart;
	memcpy(buf, d->part, d->npart);
	d->npart = 0;
	for (int loops = 0; loops < 64; loops++) {
		size_t req = cap - tot;
		ssize_t r = read(d->fd, buf + tot, req);
		if (r <= 0) {
			if (r < 0 && errno != EAGAIN && errno != EINTR)
				err = errno;
			break;
		}
		tot += (size_t)r;
		if ((size_t)r < req)
			break;	/* backlog drained */
		batch_done(m, dv, buf, &tot);	/* more than A6L_RBATCH scans (> kfifo size): one anchor per batch */
	}
	batch_done(m, dv, buf, &tot);
	if (err) {
		ALOGE("%s: read: %s", d->iio_name, strerror(err));
		dev_off(d);
		d->retry_ns = a6l_now_ns() + 1000000000LL;
		return;
	}
	memcpy(d->part, buf, tot);
	d->npart = (int)tot;
}

void a6l_motion_service(struct a6l_motion *m)
{
	for (int dv = 0; dv < A6L_NDEV; dv++)
		drain(m, dv);
	a6l_gyro_cal_eval(m, a6l_now_ns());	/* F32: accel/mag of the candidate window are drained now */
	/* flush: everything buffered has been drained above, then FLUSH_COMPLETE */
	for (int i = 0; i < A6L_NMH; i++) {
		struct a6l_mh *h = &m->h[i];
		while (h->flush_pending > 0) {
			push_flush_complete(m, h->handle);
			h->flush_pending--;
		}
	}
}

int a6l_motion_pop(struct a6l_motion *m, sensors_event_t *out, int max)
{
	return a6l_evq_pop(&m->q, out, max);
}

void a6l_motion_close(struct a6l_motion *m)
{
	for (int dv = 0; dv < A6L_NDEV; dv++)
		dev_off(&m->d[dv]);
	if (m->mc.dirty)
		a6l_magcal_save(&m->mc);
}
