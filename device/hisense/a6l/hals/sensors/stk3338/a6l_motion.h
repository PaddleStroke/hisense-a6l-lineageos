/*
 * sensors.a6l motion part (agent senshal, 26 Sep 2026): accelerometer, gyroscope and magnetometer from the ADSP Sensor
 * Manager through the mainline qcom_smgr IIO drivers (IIO names qcom-smgr-accel / -gyro / -mag, channels x/y/z le:s32/32,
 * timestamp le:u32/64 = SMGR DSP ticks, scale 1/65536, NO _raw attributes: data only through the kfifo buffer,
 * sample rate always the sensor's native maximum). See docs/senshal-20260926.md.
 */
#pragma once
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <hardware/sensors.h>

/* handles (1 and 2 = STK3338, see sensors_a6l.c) */
#define A6L_H_PROX 1
#define A6L_H_LIGHT 2
#define A6L_H_ACCEL 3
#define A6L_H_GYRO 4
#define A6L_H_MAG 5
#define A6L_H_ACCEL_UNCAL 6
#define A6L_H_GYRO_UNCAL 7
#define A6L_H_MAG_UNCAL 8
#define A6L_H_FIRST_MOTION A6L_H_ACCEL
#define A6L_H_LAST_MOTION A6L_H_MAG_UNCAL
#define A6L_NMH (A6L_H_LAST_MOTION - A6L_H_FIRST_MOTION + 1)

enum { A6L_ACC, A6L_GYR, A6L_MAG, A6L_NDEV };

/* ---------------- properties / paths (host tests: environment) ---------------- */
int a6l_prop(const char *key, char *val, const char *def);	/* val >= 92 bytes */
const char *a6l_sysroot(void);	/* /sys/bus/iio/devices */
const char *a6l_devroot(void);	/* /dev */
const char *a6l_datadir(void);	/* /data/vendor/sensors */
int64_t a6l_now_ns(void);	/* CLOCK_BOOTTIME */

/* ---------------- event queue ---------------- */
#define A6L_EVQ 2048
struct a6l_evq {
	sensors_event_t ev[A6L_EVQ];
	int head, n;
	long dropped;
};
/* F31 (r5 deep review): overflow drops the oldest SAMPLE; META_DATA_FLUSH_COMPLETE records are control completions and are
 * kept (in order) unless the queue holds nothing else */
void a6l_evq_push(struct a6l_evq *q, const sensors_event_t *e);
int a6l_evq_pop(struct a6l_evq *q, sensors_event_t *out, int max);
/* F31: removes the queued samples (not the flush completions) of one sensor handle; returns how many */
int a6l_evq_purge(struct a6l_evq *q, int handle);

/* ---------------- DSP tick -> CLOCK_BOOTTIME ---------------- */
struct a6l_tsmap {
	double hz;		/* tick rate (property, then measured) */
	int init, measured;
	uint32_t last_tick;
	uint64_t ext;		/* unwrapped last tick */
	uint64_t ext_base;	/* tick origin for ns conversion */
	double offset_ns;	/* boottime = tick_ns + offset (min-latency filter) */
	int64_t last_anchor_ns;
	uint64_t m_ext0;	/* rate measurement start */
	int64_t m_now0;
};
void a6l_tsmap_reset(struct a6l_tsmap *t, double hz);
uint64_t a6l_tsmap_unwrap(struct a6l_tsmap *t, uint32_t tick);
void a6l_tsmap_anchor(struct a6l_tsmap *t, uint64_t ext_last, int64_t now_ns);
int64_t a6l_tsmap_ns(const struct a6l_tsmap *t, uint64_t ext);

/* ---------------- magnetometer hard-iron calibration ---------------- */
#define A6L_MC_PTS 64
struct a6l_magcal {
	float bias[3];
	float radius;
	int accuracy;		/* SENSOR_STATUS_* 0..3 */
	int have_bias;
	float pts[A6L_MC_PTS][3];
	int np, next;
	int bins, fits;
	float rms;
	float dev_ema;		/* relative | |B-bias| - r | / r */
	int64_t last_save_ns;
	int dirty;
	char path[160];
};
void a6l_magcal_init(struct a6l_magcal *mc, const char *path);
int a6l_magcal_load(struct a6l_magcal *mc);
int a6l_magcal_save(struct a6l_magcal *mc);
/* feed one uncalibrated sample (uT); returns 1 when bias/accuracy changed */
int a6l_magcal_add(struct a6l_magcal *mc, const float b[3], int64_t now_ns);
/* pure sphere fit, exported for tests: returns 0 and centre/radius/rms, -1 if singular */
int a6l_sphere_fit(const float (*p)[3], int n, float c[3], float *r, float *rms);

/* ---------------- motion engine ---------------- */
struct a6l_chan { int index, bytes, bits, shift, sgn, be, off, axis; };
/* F30: one drain reads the whole available backlog (up to A6L_RBATCH scans, > the 256-scan kfifo) before the clock anchor
 * is chosen from its newest scan */
#define A6L_RBATCH 512
/* F32: recent accel / mag samples (CLOCK_BOOTTIME, Android frame) = independent motion evidence for the gyro bias */
#define A6L_RING 512
struct a6l_vs { int64_t ts; float v[3]; };
struct a6l_ring { struct a6l_vs s[A6L_RING]; int next, n; };
void a6l_ring_add(struct a6l_ring *r, int64_t ts, const double v[3]);
struct a6l_iio {
	const char *iio_name, *type, *tag;
	char sys[192];
	int fd, on, want, broken;
	int epoch;			/* F30: +1 per buffer enable (tick domain restarts) */
	int64_t retry_ns;
	struct a6l_chan ch[4];
	int nch, rec, has_ts, watermark;
	double scale, mul;		/* mul: extra unit factor (mag gauss->uT = 100) */
	int map_src[3], map_sgn[3];
	double native_ns;		/* estimated native sample period */
	int64_t last_ts;
	uint64_t last_ext;
	struct a6l_tsmap tm;
	long samples;
	uint8_t part[64];
	int npart;
};
struct a6l_mh {
	int handle, dev, enabled, uncal, flush_pending;
	int64_t period, latency, last_emit;
	int emitted;
	uint64_t last_emit_ext;		/* F30: decimation in acquisition (tick) time, not repaired timestamps */
	int emit_epoch;
	int64_t act_ns;			/* F31: activation time; older scans are not this activation's samples */
};
struct a6l_motion {
	struct a6l_iio d[A6L_NDEV];
	struct a6l_mh h[A6L_NMH];
	struct a6l_evq q;
	struct a6l_magcal mc;
	int magcal_on;
	char mag_unit[16];
	/* gyro bias (stationary detector) */
	double gb[3];
	int gb_valid;
	double gw_sum[3], gw_sum2[3];
	long gw_n;
	int64_t gw_t0;
	/* F32: a gyro-stable window is only a CANDIDATE; accel (tilt) and mag (yaw) evidence decide what is bias */
	int gb_full;			/* every component confirmed by independent evidence at least once */
	int gc_pending;
	int64_t gc_t0, gc_t1;
	double gc_mean[3];
	long gc_stat[4];		/* full, partial (no yaw evidence), rejected (motion), rejected (no accel) */
	struct a6l_ring acc_ring, mag_ring;
	uint8_t rbuf[A6L_RBATCH * 64];	/* F30 drain batch */
};
const struct sensor_t *a6l_motion_list(int *n);
void a6l_motion_init(struct a6l_motion *m);
int a6l_motion_owns(int handle);
int a6l_motion_activate(struct a6l_motion *m, int handle, int en);
int a6l_motion_batch(struct a6l_motion *m, int handle, int64_t period, int64_t latency);
int a6l_motion_flush(struct a6l_motion *m, int handle);
/* poll thread, under the caller's lock */
int a6l_motion_reconcile(struct a6l_motion *m);	/* returns ms until a retry is due, -1 = none */
/* F4: ms until the next (re)open attempt of a wanted device that is off, 0 = now, -1 = none; call it AFTER
 * a6l_motion_service(), whose read-error handling closes devices and schedules their retry */
int a6l_motion_retry_ms(const struct a6l_motion *m);
int a6l_motion_fds(struct a6l_motion *m, struct pollfd *p, int max);
void a6l_motion_service(struct a6l_motion *m);
int a6l_motion_pop(struct a6l_motion *m, sensors_event_t *out, int max);
void a6l_motion_close(struct a6l_motion *m);
/* F32, exported for tests: feed one gyro sample (rad/s, Android frame) to the stationary detector; evaluate a pending
 * candidate window against the accel/mag rings: 0 = nothing pending / still waiting for data, 1 = full update,
 * 2 = partial update (components perpendicular to gravity only), -1 = rejected (motion seen), -2 = no accel evidence */
void a6l_gyro_bias_feed(struct a6l_motion *m, const double w[3], int64_t ts);
int a6l_gyro_cal_eval(struct a6l_motion *m, int64_t now);
