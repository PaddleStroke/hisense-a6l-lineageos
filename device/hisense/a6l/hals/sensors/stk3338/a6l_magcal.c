/*
 * sensors.a6l: magnetometer hard-iron calibration (agent senshal, 26 Sep 2026).
 * Android's framework does not calibrate the magnetometer; SensorService fusion (rotation vector, orientation, compass
 * apps) consumes TYPE_MAGNETIC_FIELD, which must already be hard-iron corrected. Stock did this in the SSC ("mag cal").
 * Method: keep up to 64 well-separated raw samples (>= 6 uT apart, ring replacement), least-squares sphere fit
 * |p - c|^2 = r^2 (linear form), accept when 15 <= r <= 120 uT, rms small and the points cover enough directions
 * around the centre (26 direction bins). Accuracy: 0 no bias, 1 persisted bias not yet confirmed / environment changed,
 * 2 fit with >= 10 bins, 3 fit with >= 16 bins and rms < 4 % r. Persisted in /data/vendor/sensors/mag_bias.
 */
#define LOG_TAG "sensors.a6l"
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <log/log.h>

#include "a6l_motion.h"

#define MC_MIN_SEP 6.0f		/* uT between stored points */
#define MC_MIN_PTS 12
#define MC_RMIN 15.0f
#define MC_RMAX 120.0f
#define MC_SAVE_EVERY_NS (30LL * 1000000000LL)

void a6l_magcal_init(struct a6l_magcal *mc, const char *path)
{
	memset(mc, 0, sizeof(*mc));
	snprintf(mc->path, sizeof(mc->path), "%s", path ? path : "");
	mc->accuracy = SENSOR_STATUS_UNRELIABLE;
}

int a6l_magcal_load(struct a6l_magcal *mc)
{
	char b[160];
	float x, y, z, r;
	int acc, ver;
	int fd = open(mc->path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -errno;
	ssize_t n = read(fd, b, sizeof(b) - 1);
	close(fd);
	if (n <= 0)
		return -EINVAL;
	b[n] = 0;
	if (sscanf(b, "A6L_MAGCAL %d %f %f %f %f %d", &ver, &x, &y, &z, &r, &acc) != 6 || ver != 1 ||
	    !isfinite(x) || !isfinite(y) || !isfinite(z) || fabsf(x) > 2000 || fabsf(y) > 2000 || fabsf(z) > 2000 ||
	    r < MC_RMIN || r > MC_RMAX) {
		ALOGW("magcal: ignoring bad %s", mc->path);
		return -EINVAL;
	}
	mc->bias[0] = x;
	mc->bias[1] = y;
	mc->bias[2] = z;
	mc->radius = r;
	mc->have_bias = 1;
	mc->accuracy = SENSOR_STATUS_ACCURACY_LOW;	/* until a fit in this session confirms it */
	ALOGI("magcal: loaded bias (%.1f %.1f %.1f) r=%.1f uT", x, y, z, r);
	return 0;
}

int a6l_magcal_save(struct a6l_magcal *mc)
{
	char tmp[176], b[160];
	if (!mc->have_bias || !mc->path[0])
		return -EINVAL;
	snprintf(tmp, sizeof(tmp), "%s.tmp", mc->path);
	int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0660);
	if (fd < 0)
		return -errno;
	int n = snprintf(b, sizeof(b), "A6L_MAGCAL 1 %.3f %.3f %.3f %.3f %d\n", mc->bias[0], mc->bias[1], mc->bias[2],
			 mc->radius, mc->accuracy);
	int ok = write(fd, b, n) == n;
	if (fsync(fd))
		ok = 0;
	close(fd);
	if (!ok || rename(tmp, mc->path)) {
		unlink(tmp);
		return -EIO;
	}
	mc->dirty = 0;
	return 0;
}

/* solve 4x4 A x = b, partial pivoting; -1 if (near) singular */
static int solve4(double a[4][5], double x[4])
{
	for (int c = 0; c < 4; c++) {
		int p = c;
		for (int r = c + 1; r < 4; r++)
			if (fabs(a[r][c]) > fabs(a[p][c]))
				p = r;
		if (fabs(a[p][c]) < 1e-9)
			return -1;
		if (p != c)
			for (int k = 0; k < 5; k++) {
				double t = a[c][k];
				a[c][k] = a[p][k];
				a[p][k] = t;
			}
		for (int r = 0; r < 4; r++) {
			if (r == c)
				continue;
			double f = a[r][c] / a[c][c];
			for (int k = c; k < 5; k++)
				a[r][k] -= f * a[c][k];
		}
	}
	for (int c = 0; c < 4; c++)
		x[c] = a[c][4] / a[c][c];
	return 0;
}

int a6l_sphere_fit(const float (*p)[3], int n, float c[3], float *r, float *rms)
{
	/* |p|^2 = 2 c.p + d, d = r^2 - |c|^2 ; normal equations of rows [2x 2y 2z 1] */
	double a[4][5] = { { 0 } }, x[4];
	if (n < 4)
		return -1;
	for (int i = 0; i < n; i++) {
		double row[4] = { 2 * p[i][0], 2 * p[i][1], 2 * p[i][2], 1 };
		double y = (double)p[i][0] * p[i][0] + (double)p[i][1] * p[i][1] + (double)p[i][2] * p[i][2];
		for (int j = 0; j < 4; j++) {
			for (int k = 0; k < 4; k++)
				a[j][k] += row[j] * row[k];
			a[j][4] += row[j] * y;
		}
	}
	if (solve4(a, x))
		return -1;
	double r2 = x[3] + x[0] * x[0] + x[1] * x[1] + x[2] * x[2];
	if (!(r2 > 0))
		return -1;
	c[0] = (float)x[0];
	c[1] = (float)x[1];
	c[2] = (float)x[2];
	*r = (float)sqrt(r2);
	double s = 0;
	for (int i = 0; i < n; i++) {
		double dx = p[i][0] - x[0], dy = p[i][1] - x[1], dz = p[i][2] - x[2];
		double e = sqrt(dx * dx + dy * dy + dz * dz) - *r;
		s += e * e;
	}
	*rms = (float)sqrt(s / n);
	return 0;
}

/* nearest of the 26 cube directions (-1/0/+1 per axis, not all 0) */
static int dir_bin(float x, float y, float z)
{
	float m = sqrtf(x * x + y * y + z * z);
	if (m <= 0)
		return 0;
	int q[3];
	float v[3] = { x / m, y / m, z / m };
	for (int i = 0; i < 3; i++)
		q[i] = v[i] > 0.3827f ? 2 : (v[i] < -0.3827f ? 0 : 1);	/* 22.5 deg bands */
	return q[0] * 9 + q[1] * 3 + q[2];
}

int a6l_magcal_add(struct a6l_magcal *mc, const float b[3], int64_t now_ns)
{
	int changed = 0;
	if (!isfinite(b[0]) || !isfinite(b[1]) || !isfinite(b[2]))
		return 0;
	/* environment check: a large persistent deviation from the fitted sphere lowers the accuracy */
	if (mc->have_bias && mc->radius > 0) {
		float dx = b[0] - mc->bias[0], dy = b[1] - mc->bias[1], dz = b[2] - mc->bias[2];
		float dev = fabsf(sqrtf(dx * dx + dy * dy + dz * dz) - mc->radius) / mc->radius;
		mc->dev_ema = 0.98f * mc->dev_ema + 0.02f * dev;
		if (mc->dev_ema > 0.25f && mc->accuracy > SENSOR_STATUS_ACCURACY_LOW) {
			mc->accuracy = SENSOR_STATUS_ACCURACY_LOW;
			changed = 1;
		}
	}
	for (int i = 0; i < mc->np; i++) {
		float dx = b[0] - mc->pts[i][0], dy = b[1] - mc->pts[i][1], dz = b[2] - mc->pts[i][2];
		if (dx * dx + dy * dy + dz * dz < MC_MIN_SEP * MC_MIN_SEP)
			goto maybe_save;	/* not new information */
	}
	memcpy(mc->pts[mc->next], b, sizeof(float) * 3);
	mc->next = (mc->next + 1) % A6L_MC_PTS;
	if (mc->np < A6L_MC_PTS)
		mc->np++;
	if (mc->np >= MC_MIN_PTS) {
		float c[3], r, rms;
		if (!a6l_sphere_fit((const float (*)[3])mc->pts, mc->np, c, &r, &rms) && r >= MC_RMIN && r <= MC_RMAX &&
		    rms < 0.08f * r) {
			unsigned seen[27] = { 0 };
			int bins = 0;
			for (int i = 0; i < mc->np; i++) {
				int k = dir_bin(mc->pts[i][0] - c[0], mc->pts[i][1] - c[1], mc->pts[i][2] - c[2]);
				if (!seen[k]++)
					bins++;
			}
			mc->bins = bins;
			mc->rms = rms;
			mc->fits++;
			if (bins >= 10) {
				int acc = (bins >= 16 && rms < 0.04f * r) ? SENSOR_STATUS_ACCURACY_HIGH
									 : SENSOR_STATUS_ACCURACY_MEDIUM;
				float mv = fabsf(c[0] - mc->bias[0]) + fabsf(c[1] - mc->bias[1]) + fabsf(c[2] - mc->bias[2]);
				if (!mc->have_bias || mv > 0.2f || acc != mc->accuracy) {
					if (!mc->have_bias || mv > 1.0f || acc > mc->accuracy)
						mc->dirty = 1;
					memcpy(mc->bias, c, sizeof(c));
					mc->radius = r;
					mc->have_bias = 1;
					mc->accuracy = acc;
					mc->dev_ema = 0;
					changed = 1;
				}
			}
		}
	}
maybe_save:
	if (mc->dirty && (mc->last_save_ns == 0 || now_ns - mc->last_save_ns >= MC_SAVE_EVERY_NS)) {
		int e = a6l_magcal_save(mc);
		mc->last_save_ns = now_ns;
		if (e)
			ALOGW("magcal: save %s failed (%d)", mc->path, e);
		else
			ALOGI("magcal: saved bias (%.1f %.1f %.1f) r=%.1f acc=%d bins=%d", mc->bias[0], mc->bias[1],
			      mc->bias[2], mc->radius, mc->accuracy, mc->bins);
	}
	return changed;
}
