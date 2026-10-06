/*
 * sensors.a6l.so - Hisense A6L sensors: FRONT light + proximity (Sensortek STK3338, kernel stk3338_a6l.ko, IIO name
 * "stk3338") and, since 26 Sep 2026 (agent senshal), accelerometer / gyroscope / magnetometer + uncalibrated variants
 * from the ADSP SMGR IIO devices (a6l_motion.c, a6l_magcal.c). The trout IIO sub-HAL is no longer used.
 * Legacy sensors HAL module (API 1.3) wrapped into the multihal by LineageOS'
 * android.hardware.sensors@2.0-subhal-impl-1.0 (hardware/lineage/interfaces/sensors), found by hw_get_module()
 * through ro.hardware.sensors=a6l.
 *   handle 1: proximity, WAKE-UP, on-change, 0 cm = near / 5 cm = far, from IIO threshold events (near = rising)
 *   handle 2: light, on-change, lux = in_illuminance_raw * in_illuminance_scale, polled every 200 ms while active
 * The REAR TMD3702 is deliberately NOT exposed as proximity: Android's default (wake-up) proximity sensor must be the
 * front one, next to the earpiece.
 *   handles 3..8: accel, gyro, mag, accel uncal, gyro uncal, mag uncal (a6l_motion.h)
 * stk agent, 25 Sep 2026 (docs/stk-20260925.md); motion: senshal agent, 26 Sep 2026 (docs/senshal-20260926.md).
 * r5 review fixes (29 Sep 2026, docs/hardware-review-20260928.md F3/F4): the poll timeout is derived after the motion
 * read/error handling (a pending retry always bounds it); late motion IIO nodes keep the activation and are retried;
 * a failed proximity event-FD open is retried from the poll loop (fresh initial state after recovery); the initial
 * proximity event is kept until a valid reading exists; the STK3338 PS interrupt (IIO event enable) follows the
 * proximity activation so stk3338_a6l.ko keeps PS wake-armed in suspend only while a client needs it.
 * r5 review round6 (29 Sep 2026, docs/hardware-review-round6-20260928.md):
 *  F51: the PS interrupt enable is requested-vs-applied state: a write is applied only when the readback of the (shared)
 *       INT_PS bit matches; a failure keeps the request and is retried from the poll loop with a bounded backoff
 *       (200 ms .. 5 s) independent of the event fd; disable and HAL-open failures are retried the same way, a client
 *       disable retargets a pending enable. After an enable that needed retries the current state is re-reported.
 *  F4 (incomplete before): the kernel's iio_event_poll() returns an EMPTY mask once the device is unregistered (no
 *       POLLERR/POLLHUP), only read() says -ENODEV. While proximity is active the event fd is nonblocking and probed
 *       every PROX_LIVE_MS (read -> ENODEV, or the sysfs node gone); on loss the IIO identity is forgotten, the device
 *       is re-discovered by name (index may change), the interrupt re-applied and the event fd reopened.
 * r5 bug hunt sensors-vib-wifi-bt (29 Sep 2026):
 *  S1: boot order: the multihal (class hal) opens this HAL before the adsp module group probes stk3338_a6l.ko, whose
 *      probe enables INT_PS. Discovery used to run only on an activation, so with no proximity client the interrupt
 *      stayed enabled (PS sensing + wake-armed in every suspend, F3 defeated) until the first call. The poll loop now
 *      also re-discovers while the PS interrupt state is not verified (1 s, 10 s after SYS_FAST_TRIES misses).
 *  S2: activate() without the device: an enable is kept and served once the device appears (like the motion part,
 *      F4); a disable always succeeds and clears the request (it used to return -ENODEV and leave the sensor enabled,
 *      so a lost + re-discovered device resumed reporting / re-armed the interrupt for a disabled sensor).
 *  S3: flush() of an inactive proximity/light sensor returns -EINVAL (sensors.h) instead of a FLUSH_COMPLETE.
 */
#define _GNU_SOURCE
#define LOG_TAG "sensors.a6l"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/iio/events.h>
#include <linux/iio/types.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <hardware/hardware.h>
#include <hardware/sensors.h>
#include <log/log.h>

#include "a6l_motion.h"

#define H_PROX A6L_H_PROX
#define H_LIGHT A6L_H_LIGHT
#define PROX_FAR_CM 5.0f
#define LIGHT_PERIOD_MS 200
#define IIO_NAME "stk3338"
#define PROX_RETRY_MS 200	/* initial proximity reading not valid yet */
#define EVFD_RETRY_MS 1000	/* proximity event FD open failed */
#define PSINT_RETRY_MIN_MS 200	/* F51: PS interrupt enable/disable not applied: first retry */
#define PSINT_RETRY_MAX_MS 5000
#define PROX_LIVE_MS 2000	/* F4: liveness probe of the proximity event fd (IIO poll gives no error on unregister) */
#define SYS_FAST_TRIES 60	/* S1: discovery every EVFD_RETRY_MS for ~1 min, then every SYS_SLOW_MS */
#define SYS_SLOW_MS 10000

static const struct sensor_t g_list[] = {
	{
		.name = "STK3338 Proximity (front)", .vendor = "Sensortek", .version = 1, .handle = H_PROX,
		.type = SENSOR_TYPE_PROXIMITY, .maxRange = PROX_FAR_CM, .resolution = PROX_FAR_CM, .power = 0.1f,
		.minDelay = 0, .fifoReservedEventCount = 0, .fifoMaxEventCount = 0,
		.stringType = SENSOR_STRING_TYPE_PROXIMITY, .requiredPermission = "", .maxDelay = 0,
		.flags = SENSOR_FLAG_WAKE_UP | SENSOR_FLAG_ON_CHANGE_MODE,
	},
	{
		.name = "STK3338 Light (front)", .vendor = "Sensortek", .version = 1, .handle = H_LIGHT,
		.type = SENSOR_TYPE_LIGHT, .maxRange = 60000.0f, .resolution = 1.0f, .power = 0.1f,
		.minDelay = 0, .fifoReservedEventCount = 0, .fifoMaxEventCount = 0,
		.stringType = SENSOR_STRING_TYPE_LIGHT, .requiredPermission = "", .maxDelay = 0,
		.flags = SENSOR_FLAG_ON_CHANGE_MODE,
	},
};

struct a6l_ctx {
	struct sensors_poll_device_1 dev;	/* must be first */
	pthread_mutex_t lock;
	int wake[2];			/* activate()/flush() interrupt poll() */
	char sys[192];			/* /sys/bus/iio/devices/iio:deviceN */
	int evfd;
	int prox_on, light_on;
	int prox_initial, light_initial;
	int flush_prox, flush_light;
	float last_lux;
	double lux_scale;
	int64_t light_next;
	int64_t evfd_retry;		/* F4: next event-FD open attempt (CLOCK_BOOTTIME ns) */
	int ps_int_applied;		/* F51: INT_PS as last verified: -1 unknown/failed, 0, 1 (requested = prox_on) */
	unsigned ps_int_fail;		/* F51: consecutive failed attempts (backoff, bounded logging) */
	int64_t ps_int_retry;		/* F51: next attempt */
	int64_t prox_live;		/* F4: next event-fd liveness probe */
	int64_t sys_retry;		/* F4: next IIO re-discovery after the device was lost */
	unsigned sys_miss;		/* S1: consecutive failed discoveries (slows the retry down) */
	struct a6l_motion m;	/* accel / gyro / mag (big: event queue) */
};

static int64_t now_ns(void)
{
	struct timespec t;
	clock_gettime(CLOCK_BOOTTIME, &t);
	return (int64_t)t.tv_sec * 1000000000LL + t.tv_nsec;
}

static int read_sys(const struct a6l_ctx *c, const char *attr, char *buf, size_t n)
{
	char p[320];
	snprintf(p, sizeof(p), "%s/%s", c->sys, attr);
	int fd = open(p, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -errno;
	ssize_t r = read(fd, buf, n - 1);
	close(fd);
	if (r < 0)
		return -errno;
	buf[r] = 0;
	return 0;
}

static long read_long(const struct a6l_ctx *c, const char *attr, long def)
{
	char b[32];
	return read_sys(c, attr, b, sizeof(b)) ? def : strtol(b, NULL, 10);
}

static int find_iio(struct a6l_ctx *c)
{
	DIR *d = opendir(a6l_sysroot());
	struct dirent *e;
	int ok = -ENODEV;
	if (!d)
		return -errno;
	while ((e = readdir(d))) {
		char b[32];
		if (strncmp(e->d_name, "iio:device", 10))
			continue;
		snprintf(c->sys, sizeof(c->sys), "%s/%s", a6l_sysroot(), e->d_name);
		if (!read_sys(c, "name", b, sizeof(b)) && !strncmp(b, IIO_NAME, strlen(IIO_NAME))) {
			ok = 0;
			break;
		}
	}
	closedir(d);
	if (ok)
		c->sys[0] = 0;
	return ok;
}

static int write_sys(const struct a6l_ctx *c, const char *attr, const char *v)
{
	char p[320];
	snprintf(p, sizeof(p), "%s/%s", c->sys, attr);
	int fd = open(p, O_WRONLY | O_TRUNC | O_CLOEXEC);
	if (fd < 0)
		return -errno;
	ssize_t r = write(fd, v, strlen(v));
	int e = errno;
	close(fd);
	return r < 0 ? -e : 0;
}

/* F3: the PS interrupt (both IIO threshold events share STK3310 INT_PS) follows the proximity activation; the driver
 * keeps PS sensing and wake-armed in suspend only while it is enabled.
 * F51: returns 0 only when the readback of INT_PS equals the request (both attributes are the same register bit, so
 * one accepted write is enough - partial success is judged by the readback, not by the second write). On failure the
 * state is "unknown" and the next attempt is scheduled (backoff); the poll loop retries while it differs from prox_on. */
static int set_ps_int(struct a6l_ctx *c, int on)
{
	const char *v = on ? "1" : "0";
	int e1 = write_sys(c, "events/in_proximity_thresh_rising_en", v);
	int e2 = write_sys(c, "events/in_proximity_thresh_falling_en", v);
	long rb = read_long(c, "events/in_proximity_thresh_rising_en", -1);
	if (rb < 0)
		rb = read_long(c, "events/in_proximity_thresh_falling_en", -1);
	if (rb == on) {
		if (c->ps_int_fail)
			ALOGI("proximity interrupt %s applied after %u failed attempt(s)", on ? "enable" : "disable", c->ps_int_fail);
		c->ps_int_applied = on;
		c->ps_int_fail = 0;
		return 0;
	}
	int e = e1 ? e1 : e2 ? e2 : -EIO;
	unsigned sh = c->ps_int_fail < 5 ? c->ps_int_fail : 5;
	int64_t ms = (int64_t)PSINT_RETRY_MIN_MS << sh;
	if (ms > PSINT_RETRY_MAX_MS)
		ms = PSINT_RETRY_MAX_MS;
	if (!(c->ps_int_fail & 31))
		ALOGW("proximity interrupt %s not applied (%s, readback %ld): retry in %lld ms", on ? "enable" : "disable",
		      strerror(-e), rb, (long long)ms);
	c->ps_int_fail++;
	c->ps_int_applied = -1;
	c->ps_int_retry = now_ns() + ms * 1000000;
	return e;
}

/* F4: the IIO device is gone (unregistered / re-enumerated): forget its identity, re-discover by name later */
static void lose_iio(struct a6l_ctx *c, const char *why)
{
	ALOGE("STK3338 IIO device %s lost (%s): re-discovering", c->sys, why);
	if (c->evfd >= 0)
		close(c->evfd);
	c->evfd = -1;
	c->sys[0] = 0;
	c->ps_int_applied = -1;
	c->ps_int_fail = 0;
	c->ps_int_retry = 0;
	c->sys_retry = now_ns() + (int64_t)EVFD_RETRY_MS * 1000000;
	c->sys_miss = 0;
	c->evfd_retry = c->sys_retry;
}

/* S1/S2: the device is needed while a sensor is active, and also while the PS interrupt state is unverified (the
 * driver probes with INT_PS enabled; it must be disabled even when no proximity client ever comes) */
static int want_sys(const struct a6l_ctx *c)
{
	return !c->sys[0] && (c->prox_on || c->light_on || c->ps_int_applied != c->prox_on);
}

/* returns 0 when the event fd is (already) open, else -errno; the poll loop retries (F4) */
static int open_events(struct a6l_ctx *c)
{
	char dev[256];
	if (c->evfd >= 0)
		return 0;
	if (!c->sys[0])
		return -ENODEV;
	snprintf(dev, sizeof(dev), "%s/%s", a6l_devroot(), strrchr(c->sys, '/') + 1);
#ifdef A6L_HOST_TEST
	/* host tests: a FIFO stands in for the IIO chardev and is its own event fd */
	struct stat st;
	if (!stat(dev, &st) && S_ISFIFO(st.st_mode)) {
		int ff = open(dev, O_RDWR | O_NONBLOCK | O_CLOEXEC);
		if (ff < 0)
			return -errno;
		c->evfd = ff;
		return 0;
	}
#endif
	int fd = open(dev, O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		int e = -errno;
		ALOGE("open %s: %s", dev, strerror(-e));
		return e;
	}
	int efd = -1, e = 0;
	if (ioctl(fd, IIO_GET_EVENT_FD_IOCTL, &efd) < 0 || efd < 0) {
		e = errno ? -errno : -EIO;
		ALOGE("IIO_GET_EVENT_FD_IOCTL: %s", strerror(-e));
	} else {
		/* F4: nonblocking so the liveness probe's read() returns EAGAIN (alive, empty) or ENODEV (unregistered) */
		int fl = fcntl(efd, F_GETFL);
		if (fl >= 0)
			(void)fcntl(efd, F_SETFL, fl | O_NONBLOCK);
		c->evfd = efd;
	}
	close(fd);	/* the event fd stays valid on its own */
	return e;
}

static void kick(struct a6l_ctx *c)
{
	char x = 1;
	(void)!write(c->wake[1], &x, 1);
}

static int a6l_activate(struct sensors_poll_device_t *d, int handle, int enabled)
{
	struct a6l_ctx *c = (struct a6l_ctx *)d;
	pthread_mutex_lock(&c->lock);
	if (a6l_motion_owns(handle)) {
		int r = a6l_motion_activate(&c->m, handle, enabled);
		pthread_mutex_unlock(&c->lock);
		kick(c);
		return r;
	}
	if (handle != H_PROX && handle != H_LIGHT) {
		pthread_mutex_unlock(&c->lock);
		return -EINVAL;
	}
	if (!c->sys[0])
		find_iio(c);
	if (!c->sys[0]) {
		/* S2: no device (not probed yet, or lost): an enable is kept and served by the poll loop's discovery; a
		 * disable always succeeds (the interrupt state is re-verified when the device appears) */
		if (handle == H_PROX) {
			c->prox_on = !!enabled;
			c->prox_initial = !!enabled;
		} else {
			c->light_on = !!enabled;
			c->light_initial = !!enabled;
			c->light_next = 0;
		}
		c->sys_retry = 0;
		c->sys_miss = 0;
		if (enabled)
			ALOGW("activate %d: no IIO device named %s yet, retrying in the background", handle, IIO_NAME);
		pthread_mutex_unlock(&c->lock);
		kick(c);
		return 0;
	}
	if (handle == H_PROX) {
		c->prox_on = !!enabled;
		c->prox_initial = !!enabled;
		c->prox_live = now_ns() + (int64_t)PROX_LIVE_MS * 1000000;
		c->ps_int_fail = 0;	/* a new request (also retargets a pending retry): fresh backoff */
		if (set_ps_int(c, !!enabled))
			ALOGW("proximity interrupt %s pending: retried from the poll loop", enabled ? "enable" : "disable");
		if (enabled && open_events(c)) {
			ALOGW("proximity event fd not available yet, retrying from the poll loop");
			c->evfd_retry = now_ns() + (int64_t)EVFD_RETRY_MS * 1000000;
		}
	} else if (handle == H_LIGHT) {
		char b[32];
		c->light_on = !!enabled;
		c->light_initial = !!enabled;
		c->light_next = 0;
		c->lux_scale = read_sys(c, "in_illuminance_scale", b, sizeof(b)) ? 1.0 : strtod(b, NULL);
		if (c->lux_scale <= 0)
			c->lux_scale = 1.0;
	}
	pthread_mutex_unlock(&c->lock);
	kick(c);
	return 0;
}

static int a6l_set_delay(struct sensors_poll_device_t *d, int handle, int64_t ns)
{
	struct a6l_ctx *c = (struct a6l_ctx *)d;
	if (a6l_motion_owns(handle)) {
		pthread_mutex_lock(&c->lock);
		int r = a6l_motion_batch(&c->m, handle, ns, 0);
		pthread_mutex_unlock(&c->lock);
		kick(c);
		return r;
	}
	return (handle == H_PROX || handle == H_LIGHT) ? 0 : -EINVAL;
}

static int a6l_batch(struct sensors_poll_device_1 *d, int handle, int flags, int64_t period, int64_t latency)
{
	struct a6l_ctx *c = (struct a6l_ctx *)d;
	(void)flags;
	if (a6l_motion_owns(handle)) {
		pthread_mutex_lock(&c->lock);
		int r = a6l_motion_batch(&c->m, handle, period, latency);
		pthread_mutex_unlock(&c->lock);
		kick(c);
		return r;
	}
	return a6l_set_delay(&d->v0, handle, period);
}

static int a6l_flush(struct sensors_poll_device_1 *d, int handle)
{
	struct a6l_ctx *c = (struct a6l_ctx *)d;
	if (a6l_motion_owns(handle)) {
		pthread_mutex_lock(&c->lock);
		int r = a6l_motion_flush(&c->m, handle);
		pthread_mutex_unlock(&c->lock);
		if (!r)
			kick(c);
		return r;
	}
	int r = 0;
	pthread_mutex_lock(&c->lock);
	if (handle == H_PROX && c->prox_on)
		c->flush_prox++;
	else if (handle == H_LIGHT && c->light_on)
		c->flush_light++;
	else
		r = -EINVAL;	/* S3: unknown or inactive sensor (sensors.h flush contract) */
	pthread_mutex_unlock(&c->lock);
	if (!r)
		kick(c);
	return r;
}

static void ev_prox(sensors_event_t *e, int near)
{
	memset(e, 0, sizeof(*e));
	e->version = sizeof(*e);
	e->sensor = H_PROX;
	e->type = SENSOR_TYPE_PROXIMITY;
	e->timestamp = now_ns();
	e->distance = near ? 0.0f : PROX_FAR_CM;
}

static void ev_meta(sensors_event_t *e, int handle)
{
	memset(e, 0, sizeof(*e));
	e->version = META_DATA_VERSION;
	e->type = SENSOR_TYPE_META_DATA;
	e->meta_data.what = META_DATA_FLUSH_COMPLETE;
	e->meta_data.sensor = handle;
}

static int a6l_poll(struct sensors_poll_device_t *d, sensors_event_t *out, int count)
{
	struct a6l_ctx *c = (struct a6l_ctx *)d;
	for (;;) {
		int n = 0;
		pthread_mutex_lock(&c->lock);
		while (n < count && c->flush_prox) { ev_meta(&out[n++], H_PROX); c->flush_prox--; }
		while (n < count && c->flush_light) { ev_meta(&out[n++], H_LIGHT); c->flush_light--; }
		int64_t tnow = now_ns();
		/* F4: device lost: re-discover it by name (the IIO index may have changed) */
		/* S1: also while the PS interrupt is unverified (late probe after the HAL opened, no client) */
		if (want_sys(c) && tnow >= c->sys_retry) {
			if (find_iio(c)) {
				c->sys_miss++;
				c->sys_retry = tnow + (int64_t)(c->sys_miss < SYS_FAST_TRIES ? EVFD_RETRY_MS : SYS_SLOW_MS) * 1000000;
			} else {
				char b[32];
				c->sys_miss = 0;
				ALOGI("STK3338 IIO device found: %s", c->sys);
				c->lux_scale = read_sys(c, "in_illuminance_scale", b, sizeof(b)) ? 1.0 : strtod(b, NULL);
				if (c->lux_scale <= 0)
					c->lux_scale = 1.0;
				c->prox_initial = c->prox_on;
				c->light_initial = c->light_on;
			}
		}
		/* F51: requested (prox_on) vs applied PS interrupt state; retried independently of the event fd */
		if (c->sys[0] && c->ps_int_applied != c->prox_on && tnow >= c->ps_int_retry) {
			int was_unknown = c->ps_int_applied < 0;
			if (!set_ps_int(c, c->prox_on) && c->prox_on && was_unknown)
				c->prox_initial = 1;	/* near/far edges may have been missed: report the current state */
		}
		/* F4: event fd failed at activate (or was lost): retry here; after recovery report the current state again */
		if (c->prox_on && c->evfd < 0 && c->sys[0] && tnow >= c->evfd_retry) {
			if (!open_events(c)) {
				ALOGI("proximity event fd recovered");
				c->prox_initial = 1;
				c->prox_live = tnow + (int64_t)PROX_LIVE_MS * 1000000;
			} else {
				c->evfd_retry = tnow + (int64_t)EVFD_RETRY_MS * 1000000;
			}
		}
		/* F4: the initial event stays pending until a valid reading exists */
		if (n < count && c->prox_on && c->prox_initial) {
			long raw = read_long(c, "in_proximity_raw", -1);
			long thd = read_long(c, "events/in_proximity_thresh_rising_value", 120);
			if (raw >= 0) {
				c->prox_initial = 0;
				ev_prox(&out[n++], raw >= thd);
			}
		}
		if (n < count && c->light_on && tnow >= c->light_next) {
			long raw = read_long(c, "in_illuminance_raw", -1);
			c->light_next = tnow + (int64_t)LIGHT_PERIOD_MS * 1000000;
			char nb[32];
			if (raw < 0 && c->sys[0] && read_sys(c, "name", nb, sizeof(nb)) == -ENOENT)
				lose_iio(c, "sysfs node gone");	/* F4 */
			if (raw >= 0) {
				float lux = (float)(raw * c->lux_scale);
				if (c->light_initial || lux != c->last_lux) {
					sensors_event_t *e = &out[n++];
					memset(e, 0, sizeof(*e));
					e->version = sizeof(*e);
					e->sensor = H_LIGHT;
					e->type = SENSOR_TYPE_LIGHT;
					e->timestamp = tnow;
					e->light = lux;
					c->last_lux = lux;
					c->light_initial = 0;
				}
			}
		}
		/* motion: (re)enable IIO buffers as needed, drain them non-blocking, hand out queued events */
		a6l_motion_reconcile(&c->m);
		a6l_motion_service(&c->m);
		n += a6l_motion_pop(&c->m, out + n, count - n);
		/* F4: the retry deadline is derived AFTER service(): a read error there closes a device and schedules its
		 * retry, which must bound this poll even when no other FD or deadline is left */
		int retry_ms = a6l_motion_retry_ms(&c->m);
		struct pollfd p[2 + A6L_NDEV];
		p[0].fd = c->wake[0];
		p[0].events = POLLIN;
		p[0].revents = 0;
		int np = 1, evslot = -1;
		if (c->prox_on && c->evfd >= 0) {
			evslot = np;
			p[np].fd = c->evfd;
			p[np].events = POLLIN;
			p[np].revents = 0;
			np++;
		}
		np += a6l_motion_fds(&c->m, p + np, A6L_NDEV);
		int timeout = -1;
		if (c->light_on) {
			int64_t w = (c->light_next - tnow) / 1000000;
			timeout = w < 0 ? 0 : (int)w + 1;
		}
		if (retry_ms >= 0 && (timeout < 0 || retry_ms < timeout))
			timeout = retry_ms;
		if (c->prox_on && c->prox_initial && (timeout < 0 || PROX_RETRY_MS < timeout))
			timeout = PROX_RETRY_MS;
		int64_t dl[4];
		int ndl = 0;
		if (c->prox_on && c->evfd < 0)
			dl[ndl++] = c->evfd_retry;
		if (c->prox_on && c->evfd >= 0)
			dl[ndl++] = c->prox_live;		/* F4: liveness probe */
		if (c->sys[0] && c->ps_int_applied != c->prox_on)
			dl[ndl++] = c->ps_int_retry;		/* F51 */
		if (want_sys(c))
			dl[ndl++] = c->sys_retry;		/* F4, S1 */
		for (int i = 0; i < ndl; i++) {
			int64_t w = (dl[i] - now_ns()) / 1000000;
			int wt = w < 0 ? 0 : (int)w + 1;
			if (timeout < 0 || wt < timeout)
				timeout = wt;
		}
		if (c->m.q.n)
			timeout = 0;	/* more queued events than fitted in out[] */
		pthread_mutex_unlock(&c->lock);
		if (n)
			return n;
		int pr = poll(p, np, timeout);
		if (pr < 0 && errno != EINTR)
			usleep(100000);
#ifdef A6L_HOST_TEST
		/* regular files stand in for the IIO chardevs: always readable, avoid spinning at EOF */
		if (pr > 0 && !(p[0].revents & POLLIN) && (evslot < 0 || !(p[evslot].revents & POLLIN)))
			usleep(2000);
#endif
		if (p[0].revents & POLLIN) {
			char b[16];
			(void)!read(c->wake[0], b, sizeof(b));
		}
		/* F4: liveness probe: an unregistered IIO device never flags the event fd; a nonblocking read tells ENODEV
		 * (EAGAIN = alive and empty), a vanished sysfs node tells the same for any cause */
		int probe = 0;
		if (evslot > 0 && !(p[evslot].revents & POLLIN) && now_ns() >= c->prox_live) {
			char nb[32];
			pthread_mutex_lock(&c->lock);
			c->prox_live = now_ns() + (int64_t)PROX_LIVE_MS * 1000000;
			if (c->evfd >= 0 && c->sys[0] && read_sys(c, "name", nb, sizeof(nb)) == -ENOENT)
				lose_iio(c, "sysfs node gone");
			else
				probe = c->evfd >= 0;
			pthread_mutex_unlock(&c->lock);
			if (!probe)
				continue;
		}
		if (evslot > 0 && (p[evslot].revents & (POLLERR | POLLHUP | POLLNVAL)) && !(p[evslot].revents & POLLIN)) {
			pthread_mutex_lock(&c->lock);	/* F4: event fd lost (driver unbound): reopen from the loop */
			ALOGE("proximity event fd error 0x%x, reopening", p[evslot].revents);
			close(c->evfd);
			c->evfd = -1;
			c->evfd_retry = now_ns() + (int64_t)EVFD_RETRY_MS * 1000000;
			pthread_mutex_unlock(&c->lock);
		} else if (evslot > 0 && ((p[evslot].revents & POLLIN) || probe)) {
			struct iio_event_data ev;
			ssize_t rr = read(c->evfd, &ev, sizeof(ev));
			if (rr < 0 && errno == ENODEV) {
				pthread_mutex_lock(&c->lock);	/* F4: unregistered: rediscover (index may change), not just reopen */
				lose_iio(c, "event read ENODEV");
				pthread_mutex_unlock(&c->lock);
			} else if (rr < 0 && errno != EAGAIN && errno != EINTR) {
				pthread_mutex_lock(&c->lock);
				ALOGE("proximity event read: %s, reopening", strerror(errno));
				close(c->evfd);
				c->evfd = -1;
				c->evfd_retry = now_ns() + (int64_t)EVFD_RETRY_MS * 1000000;
				pthread_mutex_unlock(&c->lock);
			} else if (rr == sizeof(ev) &&
			    IIO_EVENT_CODE_EXTRACT_CHAN_TYPE(ev.id) == IIO_PROXIMITY) {
				int dir = IIO_EVENT_CODE_EXTRACT_DIR(ev.id);
				pthread_mutex_lock(&c->lock);
				int on = c->prox_on;
				pthread_mutex_unlock(&c->lock);
				if (on && (dir == IIO_EV_DIR_RISING || dir == IIO_EV_DIR_FALLING)) {
					ev_prox(&out[0], dir == IIO_EV_DIR_RISING);
					return 1;
				}
			}
		}
	}
}

static int a6l_close(struct hw_device_t *d)
{
	struct a6l_ctx *c = (struct a6l_ctx *)d;
	a6l_motion_close(&c->m);
	if (c->evfd >= 0)
		close(c->evfd);
	close(c->wake[0]);
	close(c->wake[1]);
	free(c);
	return 0;
}

static int a6l_open(const struct hw_module_t *m, const char *id, struct hw_device_t **dev)
{
	if (strcmp(id, SENSORS_HARDWARE_POLL))
		return -EINVAL;
	struct a6l_ctx *c = calloc(1, sizeof(*c));
	if (!c)
		return -ENOMEM;
	if (pipe2(c->wake, O_CLOEXEC | O_NONBLOCK)) {
		free(c);
		return -errno;
	}
	pthread_mutex_init(&c->lock, NULL);
	c->evfd = -1;
	c->lux_scale = 1.0;
	c->ps_int_applied = -1;	/* F51: unknown until verified */
	c->dev.common.tag = HARDWARE_DEVICE_TAG;
	c->dev.common.version = SENSORS_DEVICE_API_VERSION_1_3;
	c->dev.common.module = (struct hw_module_t *)m;
	c->dev.common.close = a6l_close;
	c->dev.activate = a6l_activate;
	c->dev.setDelay = a6l_set_delay;
	c->dev.poll = a6l_poll;
	c->dev.batch = a6l_batch;
	c->dev.flush = a6l_flush;
	find_iio(c);
	if (c->sys[0] && set_ps_int(c, 0))	/* F3: no proximity client yet: let the driver fully standby in suspend */
		ALOGW("proximity interrupt disable at open pending: retried from the poll loop");
	a6l_motion_init(&c->m);
	ALOGI("A6L sensors: front STK3338 %s", c->sys[0] ? c->sys : "not found yet");
	*dev = &c->dev.common;
	return 0;
}

/* STK3338 + motion sensors; persist.vendor.a6l.sensors.motion=0 hides the motion part (fallback) */
static struct sensor_t g_all[2 + A6L_NMH];
static int g_all_n;
static pthread_once_t g_all_once = PTHREAD_ONCE_INIT;

static void build_list(void)
{
	char v[92];
	int nm = 0;
	const struct sensor_t *ml = a6l_motion_list(&nm);
	g_all_n = 0;
	for (size_t i = 0; i < sizeof(g_list) / sizeof(g_list[0]); i++)
		g_all[g_all_n++] = g_list[i];
	a6l_prop("persist.vendor.a6l.sensors.motion", v, "1");
	if (strcmp(v, "0"))
		for (int i = 0; i < nm && g_all_n < (int)(sizeof(g_all) / sizeof(g_all[0])); i++)
			g_all[g_all_n++] = ml[i];
	ALOGI("A6L sensors: %d sensors listed (motion %s)", g_all_n, strcmp(v, "0") ? "on" : "OFF by property");
}

static int a6l_get_list(struct sensors_module_t *m, const struct sensor_t **list)
{
	(void)m;
	pthread_once(&g_all_once, build_list);
	*list = g_all;
	return g_all_n;
}

static struct hw_module_methods_t a6l_methods = { .open = a6l_open };

struct sensors_module_t HAL_MODULE_INFO_SYM = {
	.common = {
		.tag = HARDWARE_MODULE_TAG,
		.module_api_version = SENSORS_MODULE_API_VERSION_0_1,
		.hal_api_version = HARDWARE_HAL_API_VERSION,
		.id = SENSORS_HARDWARE_MODULE_ID,
		.name = "Hisense A6L sensors (STK3338 light/proximity, SMGR accel/gyro/mag)",
		.author = "A6L port",
		.methods = &a6l_methods,
	},
	.get_sensors_list = a6l_get_list,
};
