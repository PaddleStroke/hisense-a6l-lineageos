// SPDX-License-Identifier: GPL-2.0-only
/*
 * A6L power28 (28 Sep 2026; power29 29 Sep: D+/D- pull-up (OCP/FLOAT) model, QC_AUTH_DONE, late APSD result,
 * hvdcp_rerun / hvdcp_regs): register-level host test of the A6L-patched qcom_smbx.c (HVDCP / Quick Charge).
 * The driver source is #included unchanged; regmap/iio/workqueue are faked (shim.h) and a PM660 SMB2 + adapter model
 * reacts to the writes: APSD (SDP/DCP/QC2/QC3 when HVDCP_EN), CMD_HVDCP_2 FORCE_5V/9V/12V, QC3 +/-200 mV pulses,
 * USBIN_ADAPTER_ALLOW window, ICL-limited input current, USBIN OV status, input suspend. Build: see run-hvdcp-sim.sh.
 *   ./sim-smb2            -> all scenarios, prints A6L_HVDCP_SIM PASS|FAIL n
 *   ./sim-smb2 trace-off  -> register write trace of probe + plug with hvdcp_enable=0 (compared with the r4 driver)
 */
#include "shim.h"

char sim_log[1 << 20]; size_t sim_log_len; unsigned long jiffies = 1000;
struct workqueue_struct *system_wq;
static int trace_on; static FILE *trace_f;

void sim_logf(const char *lvl, const char *fmt, ...)
{
	va_list a; char b[512]; int n;
	va_start(a, fmt); n = vsnprintf(b, sizeof(b), fmt, a); va_end(a);
	if (n > 0 && b[n - 1] != '\n' && n < (int)sizeof(b) - 1) { b[n++] = '\n'; b[n] = 0; }
	sim_log_len += snprintf(sim_log + sim_log_len, sizeof(sim_log) - sim_log_len, "[%6lu] %s %s", jiffies, lvl, b);
	if (getenv("SIM_VERBOSE")) fprintf(stderr, "[%6lu] %s %s", jiffies, lvl, b);
}

/* ---------------- PM660 SMB2 + adapter model ---------------- */
enum { AD_NONE, AD_SDP, AD_DCP, AD_QC2, AD_QC3 };
static struct {
	u8 r[0x800];
	int plugged, ad, vbus, apsd_t0, hv_seen;	/* hv_seen: HVDCP_EN was on when APSD ran */
	int overshoot_uv;	/* added on FORCE_9V */
	int pulse_uv;		/* QC3 step (200 mV normally) */
	int noraise, stuck_hi;	/* adapter ignores raise / ignores FORCE_5V */
	int load_ua, ignore_icl;
	int hot;
	int pullup;		/* power29: USB gadget D+ pull-up on a wall charger -> APSD result OCP (1) or FLOAT (2), no HVDCP */
	int pu_seen;		/* pull-up state latched when APSD ran */
	int res_late_ms;	/* power29: APSD done but result 0 until this time after the APSD start */
	int n_force12, n_force9, n_force5, n_inc, n_dec, n_apsd_rerun;
	int icl_at_first_raise;	/* ICL (uA) when the first raise command came */
	int max_vbus;
} m;
static struct regmap *fake_map = (struct regmap *)0x1;
#define R(off) m.r[(off)]

static int allow_max_uv(void)
{
	switch (R(0x360) & 0xf) {
	case 0: return 5000000; case 2: case 3: case 8: return 9000000; default: return 12000000;
	}
}

static void apsd_start(void) { m.apsd_t0 = jiffies; m.pu_seen = m.pullup; m.hv_seen = !!(R(0x362) & BIT(2)); if (m.ad == AD_QC2 || m.ad == AD_QC3) m.vbus = m.plugged ? 5000000 : 0; }

static void model_update(void)
{
	long dt = (long)jiffies - m.apsd_t0;
	u8 apsd = 0, res = 0;
	if (m.plugged && dt >= 300) {
		apsd |= BIT(0);
		res = m.ad == AD_SDP ? BIT(0) : BIT(3);
		if (m.pu_seen && m.ad != AD_SDP)
			res = m.pu_seen == 2 ? BIT(4) : BIT(1);	/* OCP / FLOAT: no BC1.2 DCP, the HVDCP algorithm does not start */
		else if (m.hv_seen) {
			if ((m.ad == AD_QC2 || m.ad == AD_QC3) && dt >= 1500) { apsd |= BIT(1); res |= m.ad == AD_QC3 ? BIT(6) : BIT(5); }
			if ((m.ad == AD_QC2 || m.ad == AD_QC3) && dt >= 1800 && (R(0x362) & BIT(6))) apsd |= BIT(2);	/* QC_AUTH_DONE */
			if (dt >= 2500) apsd |= BIT(6);
		}
		if (dt < m.res_late_ms) res = 0;
	}
	R(0x307) = apsd; R(0x308) = res;
	R(0x60B) = m.plugged && !(R(0x340) & 1) ? (BIT(4) | BIT(0)) : 0;
	R(0x310) = (m.plugged ? BIT(4) : 0) | (m.vbus > 10000000 ? BIT(3) : 0);
	R(0x306) = R(0x606 + 1) = 0;
	R(0x607) = R(0x370);	/* ICL_STATUS follows the setting */
	R(0x07) = 0; R(0x06) = 2;	/* fast charge */
	R(0x07) = m.hot ? BIT(1) : 0;
	if (m.vbus > m.max_vbus) m.max_vbus = m.vbus;
}

static void cmd_hvdcp(unsigned int v)
{
	int qc = m.plugged && (m.ad == AD_QC2 || m.ad == AD_QC3) && m.hv_seen && (R(0x307) & BIT(1));
	int icl = R(0x370) * 25000;
	if (v & (BIT(5) | BIT(4) | BIT(0))) { if (!m.icl_at_first_raise) m.icl_at_first_raise = icl; }
	if (v & BIT(5)) { m.n_force12++; if (qc && allow_max_uv() >= 12000000) m.vbus = 12000000; }
	if (v & BIT(4)) { m.n_force9++; if (qc && !m.noraise) m.vbus = 9000000 + m.overshoot_uv; }
	if (v & BIT(3)) { m.n_force5++; if (qc && !m.stuck_hi) m.vbus = 5000000; }
	if (v & BIT(0)) { m.n_inc++; if (qc && m.ad == AD_QC3 && !m.noraise && m.vbus + m.pulse_uv <= allow_max_uv() + 1000000) m.vbus += m.pulse_uv; }
	if (v & BIT(1)) { m.n_dec++; if (qc && m.ad == AD_QC3 && !m.stuck_hi && m.vbus > 5000000) m.vbus -= 200000; }
}

struct regmap *dev_get_regmap(struct device *d, const char *n) { (void)d; (void)n; return fake_map; }
int regmap_read(struct regmap *map, unsigned int reg, unsigned int *val)
{
	(void)map; assert(reg >= 0x1000 && reg < 0x1800); model_update(); *val = m.r[reg - 0x1000]; return 0;
}
int regmap_write(struct regmap *map, unsigned int reg, unsigned int val)
{
	unsigned int off = reg - 0x1000;
	(void)map; assert(reg >= 0x1000 && reg < 0x1800);
	if (trace_on) fprintf(trace_f, "W 0x%04x 0x%02x\n", reg, val & 0xff);
	if (off == 0x343) { cmd_hvdcp(val); return 0; }
	if (off == 0x341) { if (val & 1) { m.n_apsd_rerun++; apsd_start(); } return 0; }
	m.r[off] = val;
	return 0;
}
int regmap_update_bits(struct regmap *map, unsigned int reg, unsigned int mask, unsigned int val)
{
	unsigned int old; regmap_read(map, reg, &old);
	if ((reg - 0x1000) == 0x343 || (reg - 0x1000) == 0x341) return regmap_write(map, reg, val & mask);
	return regmap_write(map, reg, (old & ~mask) | (val & mask));
}
int regmap_bulk_read(struct regmap *map, unsigned int reg, void *val, size_t n)
{
	size_t i; unsigned int v; for (i = 0; i < n; i++) { regmap_read(map, reg + i, &v); ((u8 *)val)[i] = v; } return 0;
}
static struct iio_channel ch_v = { 0 }, ch_i = { 1 };
struct iio_channel *devm_iio_channel_get(struct device *d, const char *name) { (void)d; return strcmp(name, "usbin_v") ? &ch_i : &ch_v; }
int iio_read_channel_processed(struct iio_channel *c, int *val)
{
	int icl = R(0x370) * 25000;
	model_update();
	if (!m.plugged) { *val = 0; return 0; }
	if (R(0x340) & 1) { *val = c->which == 0 ? m.vbus : 0; return 0; }	/* suspended: VBUS still measured */
	*val = c->which == 0 ? m.vbus : (m.ignore_icl ? m.load_ua : min_t(int, m.load_ua, icl));
	return 0;
}
static struct power_supply the_psy; static struct power_supply_battery_info bi;
struct power_supply *devm_power_supply_register(struct device *d, const struct power_supply_desc *desc, const struct power_supply_config *c)
{ (void)d; the_psy.desc = desc; the_psy.drv = c->drv_data; return &the_psy; }
int power_supply_get_battery_info(struct power_supply *p, struct power_supply_battery_info **b) { (void)p; *b = &bi; return 0; }

static struct delayed_work *works[8]; static int nworks;
static void (*actions[8])(void *); static void *action_data[8]; static int nactions;
int devm_delayed_work_autocancel(struct device *d, struct delayed_work *w, void (*fn)(struct work_struct *))
{ (void)d; w->fn = fn; w->pending = 0; works[nworks++] = w; return 0; }
int devm_add_action_or_reset(struct device *d, void (*fn)(void *), void *data)
{ (void)d; actions[nactions] = fn; action_data[nactions++] = data; return 0; }
void msleep(unsigned int ms) { jiffies += ms; }

static void run_for(unsigned long ms)
{
	unsigned long end = jiffies + ms;
	for (;;) {
		struct delayed_work *best = NULL; int i;
		for (i = 0; i < nworks; i++)
			if (works[i]->pending && works[i]->due <= end && (!best || works[i]->due < best->due)) best = works[i];
		if (!best) break;
		if (best->due > jiffies) jiffies = best->due;
		best->pending = 0; best->fn(&best->work);
	}
	if (jiffies < end) jiffies = end;
}

/* ---------------- the driver ---------------- */
#include QCOM_SMBX_C

#ifndef A6L_OLD_DRIVER
static struct smb_chip *chip(void) { return a6l_hv_chip; }
static const char *st(void) { return a6l_hv_state_names[chip()->hv_state]; }
#endif
static struct platform_device pdev = { .name = "pm660-charger" };
static int fails, checks;
#define EXPECT(name, cond) do { checks++; if (cond) printf("ok   %s\n", name); else { fails++; printf("FAIL %s  (line %d)\n", name, __LINE__); } } while (0)

static void reset_world(int ad, int hv_enable, unsigned int icl, unsigned int maxuv)
{
	int i;
	memset(&m, 0, sizeof(m)); nworks = 0; nactions = 0;
	m.pulse_uv = 200000; m.load_ua = 3000000; jiffies = 1000;
	R(0x360) = 3;	/* bootloader leaves 5V_OR_9V (unknown on the phone; 12 V must never be requested anyway) */
	R(0x362) = BIT(3);	/* AUTO_SRC_DETECT */
	bi.voltage_max_design_uv = 4400000; bi.constant_charge_current_max_ua = 2000000;
	(void)i;
#ifndef A6L_OLD_DRIVER
	hvdcp_enable = hv_enable; hvdcp_icl_ua = icl; hvdcp_max_uv = maxuv; a6l_hv_chip = NULL;
#else
	(void)hv_enable; (void)icl; (void)maxuv;
#endif
	m.ad = ad; m.plugged = ad != AD_NONE; m.vbus = m.plugged ? 5000000 : 0;
	apsd_start(); m.apsd_t0 -= 20000;	/* cable in since boot, APSD done by the bootloader (HVDCP_EN off) */
}
static void do_probe(void) { int rc = smb_probe(&pdev); assert(rc == 0); }
static void plug(int ad) { m.ad = ad; m.plugged = 1; m.vbus = 5000000; apsd_start(); smb_handle_usb_plugin(0, pdev.drvdata); }
static void unplug(void) { m.plugged = 0; m.vbus = 0; smb_handle_usb_plugin(0, pdev.drvdata); }
static int icl_reg(void) { return R(0x370) * 25000; }

#ifndef A6L_OLD_DRIVER
static void set_param(const struct kernel_param_ops *ops, const struct kernel_param *kp, const char *v) { assert(ops->set(v, kp) == 0); }
static void run_all(void)
{
	char buf[4096];

	/* 1 default off: nothing HVDCP is touched, 9 V never happens */
	reset_world(AD_QC2, 0, 1000000, 9000000); do_probe(); run_for(10000);
	EXPECT("off: state off", !strcmp(st(), "off"));
	EXPECT("off: HVDCP_EN clear", !(R(0x362) & BIT(2)));
	EXPECT("off: no HVDCP command", m.n_force9 + m.n_force5 + m.n_force12 + m.n_inc == 0);
	EXPECT("off: adapter allow untouched", R(0x360) == 3);
	EXPECT("off: 5 V", m.max_vbus == 5000000);
	EXPECT("off: DCP ICL = battery ccc 2.0 A", icl_reg() == 2000000);

	/* 2 SDP with hvdcp on */
	reset_world(AD_SDP, 1, 1000000, 9000000); do_probe(); run_for(10000);
	EXPECT("sdp: idle", !strcmp(st(), "idle"));
	EXPECT("sdp: ICL 500 mA", icl_reg() == 500000);
	EXPECT("sdp: no APSD rerun, no HVDCP cmd", m.n_apsd_rerun == 0 && m.n_force9 + m.n_inc == 0);
	EXPECT("sdp: HVDCP_EN + AUTH on, autonomous off", (R(0x362) & (BIT(2) | BIT(6) | BIT(5))) == (BIT(2) | BIT(6)));
	EXPECT("sdp: adapter allow 5-9 V", (R(0x360) & 0xf) == 8);
	EXPECT("sdp: QC2 max 9 V, QC3 max 20 pulses", R(0x35B) == (0x40 | 20));

	/* 3 plain DCP */
	reset_world(AD_DCP, 1, 1000000, 9000000); do_probe(); run_for(10000);
	EXPECT("dcp: state dcp-5v", !strcmp(st(), "dcp-5v"));
	EXPECT("dcp: ICL back to 2.0 A", icl_reg() == 2000000);
	EXPECT("dcp: 5 V only", m.max_vbus == 5000000 && m.n_force9 == 0);

	/* 4 QC2 */
	reset_world(AD_QC2, 1, 1000000, 9000000); do_probe(); run_for(10000);
	EXPECT("qc2: active", !strcmp(st(), "active") && chip()->hv_qc == 2);
	EXPECT("qc2: 9 V", m.vbus == 9000000);
	EXPECT("qc2: FORCE_9V once, never 12 V", m.n_force9 == 1 && m.n_force12 == 0);
	EXPECT("qc2: ICL <= 1 A at the voltage request", m.icl_at_first_raise <= 1000000);
	EXPECT("qc2: ICL = hvdcp_icl 1.0 A", icl_reg() == 1000000);
	EXPECT("qc2: cable in at probe -> exactly one APSD rerun", m.n_apsd_rerun == 1);
	EXPECT("qc2: dmesg A6L_HVDCP ACTIVE QC2", strstr(sim_log, "A6L_HVDCP ACTIVE QC2 usbin 9000000 uV, ICL 1000000 uA") != NULL);
	a6l_hv_status_get(buf, NULL);
	EXPECT("qc2: status string", strstr(buf, "state=active qc=2") && strstr(buf, "usbin_uV=9000000"));
	{ union power_supply_propval v = { .intval = 2400000 }; the_psy.desc->set_property(&the_psy, POWER_SUPPLY_PROP_CURRENT_MAX, &v); }
	run_for(3000);
	EXPECT("qc2: userspace 2.4 A clamped to hvdcp_icl", icl_reg() == 1000000);
	{ union power_supply_propval v = { .intval = 700000 }; the_psy.desc->set_property(&the_psy, POWER_SUPPLY_PROP_CURRENT_MAX, &v); }
	run_for(3000);
	EXPECT("qc2: userspace can lower the ICL (guard warm)", icl_reg() == 700000 && !strcmp(st(), "active"));
	set_param(&a6l_hv_icl_ops, &__param_hvdcp_icl_ua, "3000000"); run_for(2000);
	EXPECT("qc2: hvdcp_icl_ua 3 A clamped to 2 A", hvdcp_icl_ua == 2000000 && icl_reg() == 2000000);
	/* runtime disable */
	set_param(&a6l_hv_enable_ops, &__param_hvdcp_enable, "0"); run_for(3000);
	EXPECT("qc2: hvdcp_enable=0 -> 5 V", m.vbus == 5000000 && m.n_force5 >= 1);
	EXPECT("qc2: hvdcp_enable=0 -> state off, HVDCP_EN clear", !strcmp(st(), "off") && !(R(0x362) & BIT(2)));
	EXPECT("qc2: hvdcp_enable=0 -> ICL back to DCP 2.0 A", icl_reg() == 2000000);
	/* runtime re-enable with the QC adapter still in: APSD rerun -> active again */
	set_param(&a6l_hv_enable_ops, &__param_hvdcp_enable, "1"); run_for(10000);
	EXPECT("qc2: re-enable on DCP reruns APSD -> active", m.n_apsd_rerun == 1 && !strcmp(st(), "active") && m.vbus == 9000000);
	/* unplug / replug */
	unplug(); run_for(3000);
	EXPECT("qc2: unplug -> idle", !strcmp(st(), "idle"));
	plug(AD_QC2); run_for(10000);
	EXPECT("qc2: replug -> active again", !strcmp(st(), "active") && m.vbus == 9000000);
	/* teardown (unbind / rmmod) */
	actions[0](action_data[0]);
	EXPECT("qc2: unbind -> FORCE_5V, 5 V", m.vbus == 5000000);

	/* 5 QC3 9 V */
	reset_world(AD_QC3, 1, 1000000, 9000000); do_probe(); run_for(12000);
	EXPECT("qc3: active at 9.0 V after 20 pulses", !strcmp(st(), "active") && m.vbus == 9000000 && m.n_inc == 20);
	EXPECT("qc3: ICL <= 1 A at the first pulse", m.icl_at_first_raise <= 1000000);
	unplug(); run_for(2000); plug(AD_QC3); run_for(12000);
	EXPECT("qc3: replug -> active 9.0 V", !strcmp(st(), "active") && m.vbus == 9000000);
	set_param(&a6l_hv_enable_ops, &__param_hvdcp_enable, "0"); run_for(5000);
	EXPECT("qc3: disable -> decrement pulses + FORCE_5V -> 5 V", m.vbus == 5000000 && m.n_dec == 20);

	/* 6 QC3 7 V target */
	reset_world(AD_QC3, 1, 1000000, 7000000); do_probe(); run_for(12000);
	EXPECT("qc3 7V: 10 pulses, active 7.0 V", !strcmp(st(), "active") && m.vbus == 7000000 && m.n_inc == 10);
	EXPECT("qc3 7V: QC3 pulse max 10", (R(0x35B) & 0x3f) == 10);
	/* 7 QC2 with a 7 V target stays 5 V */
	reset_world(AD_QC2, 1, 1000000, 7000000); do_probe(); run_for(12000);
	EXPECT("qc2 7V target: stays 5 V (dcp-5v)", !strcmp(st(), "dcp-5v") && m.max_vbus == 5000000);

	/* 8 overshoot (10.2 V) */
	reset_world(AD_QC2, 1, 1000000, 9000000); m.overshoot_uv = 1200000; do_probe(); run_for(12000);
	EXPECT("overshoot 10.2 V: failed", !strcmp(st(), "failed"));
	EXPECT("overshoot: back to 5 V, ICL 500 mA", m.vbus == 5000000 && icl_reg() == 500000);
	run_for(30000);
	EXPECT("overshoot: no retry while plugged", m.n_force9 == 1 && !strcmp(st(), "failed"));
	/* 9 small overshoot 9.7 V (in USBIN OV range of our window) */
	reset_world(AD_QC2, 1, 1000000, 9000000); m.overshoot_uv = 700000; do_probe(); run_for(12000);
	EXPECT("9.7 V: failed + 5 V", !strcmp(st(), "failed") && m.vbus == 5000000);
	/* 10 adapter does not raise */
	reset_world(AD_QC2, 1, 1000000, 9000000); m.noraise = 1; do_probe(); run_for(12000);
	EXPECT("no raise: failed after verify timeout", !strcmp(st(), "failed") && strstr(chip()->hv_reason, "did not reach"));
	/* 11 sag to 8.0 V while active */
	reset_world(AD_QC2, 1, 1000000, 9000000); do_probe(); run_for(10000); m.vbus = 8000000; run_for(1500);
	EXPECT("sag 8.0 V: still active after 1 sample", !strcmp(st(), "active"));
	run_for(3000);
	EXPECT("sag 8.0 V: abort after 3 samples", !strcmp(st(), "failed") && strstr(chip()->hv_reason, "undervoltage"));
	/* 12 overcurrent (ICL not respected) */
	reset_world(AD_QC2, 1, 1000000, 9000000); do_probe(); run_for(10000); m.ignore_icl = 1; m.load_ua = 1500000; run_for(4000);
	EXPECT("overcurrent 1.5 A at ICL 1 A: abort", !strcmp(st(), "failed") && strstr(chip()->hv_reason, "overcurrent"));
	/* 13 battery too hot */
	reset_world(AD_QC2, 1, 1000000, 9000000); do_probe(); run_for(10000); m.hot = 1; run_for(2000);
	EXPECT("battery too hot: abort", !strcmp(st(), "failed") && m.vbus == 5000000);
	/* 14 adapter ignores FORCE_5V -> input suspended */
	reset_world(AD_QC2, 1, 1000000, 9000000); do_probe(); run_for(10000); m.stuck_hi = 1; m.vbus = 9800000; run_for(5000);
	EXPECT("stuck at 9.8 V: USB input suspended", (R(0x340) & 1) && !strcmp(st(), "failed"));
	/* 15 QC3 big steps (400 mV) -> overshoot during pulses */
	reset_world(AD_QC3, 1, 1000000, 9000000); m.pulse_uv = 400000; do_probe(); run_for(12000);
	EXPECT("qc3 400 mV steps: abort, 5 V", !strcmp(st(), "failed") && m.vbus == 5000000 && m.max_vbus <= 9800000);
	/* 16 enable at runtime on SDP: no APSD rerun */
	reset_world(AD_SDP, 0, 1000000, 9000000); do_probe(); run_for(5000);
	set_param(&a6l_hv_enable_ops, &__param_hvdcp_enable, "1"); run_for(5000);
	EXPECT("runtime enable on SDP: no APSD rerun, idle, 500 mA", m.n_apsd_rerun == 0 && !strcmp(st(), "idle") && icl_reg() == 500000);
	/* then move to a QC charger */
	unplug(); run_for(2000); plug(AD_QC2); run_for(10000);
	EXPECT("SDP -> QC2 wall charger: active 9 V", !strcmp(st(), "active") && m.vbus == 9000000);
	/* chg-off order in run-power.sh: input suspend while at 9 V, then hvdcp_enable=0 */
	{ union power_supply_propval v = { .intval = 0 }; the_psy.desc->set_property(&the_psy, POWER_SUPPLY_PROP_STATUS, &v); }
	run_for(5000);
	EXPECT("suspended at 9 V: no false abort", !strcmp(st(), "active"));
	set_param(&a6l_hv_enable_ops, &__param_hvdcp_enable, "0"); run_for(3000);
	EXPECT("suspended + hvdcp_enable=0 -> 5 V", m.vbus == 5000000 && !strcmp(st(), "off"));
	set_param(&a6l_hv_enable_ops, &__param_hvdcp_enable, "1"); run_for(1000);
	{ union power_supply_propval v = { .intval = 1 }; the_psy.desc->set_property(&the_psy, POWER_SUPPLY_PROP_STATUS, &v); }
	run_for(10000);
	EXPECT("re-enable + resume -> active 9 V again", !strcmp(st(), "active") && m.vbus == 9000000);
	/* 17 shutdown */
	smb_shutdown(&pdev);
	EXPECT("shutdown -> 5 V", m.vbus == 5000000);
	/* 18 12 V never */
	EXPECT("12 V never requested in any scenario", m.n_force12 == 0);

	/* ---- power29 (29 Sep): attended QC1 reproduction + fixes ---- */
	/* 19 QC1 field case: armed on the laptop (SDP), unplug, QC2 charger with the USB D+ pull-up -> APSD OCP ("DCP" in
	 * the psy usb_type). power28 stayed "idle" silently; now: logged, dcp-5v, then pull-up removed + hvdcp_rerun -> 9 V */
	reset_world(AD_SDP, 0, 1000000, 9000000); do_probe(); run_for(5000);
	set_param(&a6l_hv_enable_ops, &__param_hvdcp_enable, "1"); run_for(3000);
	unplug(); run_for(5000); m.pullup = 1; plug(AD_QC2); run_for(12000);
	EXPECT("p29 OCP: not silently idle -> dcp-5v", !strcmp(st(), "dcp-5v"));
	EXPECT("p29 OCP: APSD logged", strstr(sim_log, "A6L_HVDCP APSD status 0x01 result 0x02 (OCP)") != NULL);
	EXPECT("p29 OCP: pull-up warning", strstr(sim_log, "not a BC1.2 DCP") != NULL);
	EXPECT("p29 OCP: 5 V, DCP ICL 2.0 A, no APSD rerun", m.max_vbus == 5000000 && icl_reg() == 2000000 && m.n_apsd_rerun == 0);
	a6l_hv_status_get(buf, NULL);
	EXPECT("p29 status shows apsd=0x01/0x02", strstr(buf, "apsd=0x01/0x02 reruns=0") != NULL);
	a6l_hv_regs_get(buf, NULL);
	EXPECT("p29 hvdcp_regs dump", strstr(buf, "1307=01 ") && strstr(buf, "1308=02 ") && strstr(buf, "1362=4c ") && strstr(buf, "1360=08 "));
	m.pullup = 0; set_param(&a6l_hv_rerun_ops, &__param_hvdcp_rerun, "1"); run_for(12000);
	EXPECT("p29 pull-up removed + hvdcp_rerun -> QC2 active 9 V", !strcmp(st(), "active") && m.vbus == 9000000 && m.n_apsd_rerun == 1);
	EXPECT("p29 rerun: ICL <= 1 A at the voltage request", m.icl_at_first_raise <= 1000000);
	EXPECT("p29 rerun logged", strstr(sim_log, "A6L_HVDCP APSD rerun #1 requested (was 0x01/0x02 OCP)") != NULL);
	set_param(&a6l_hv_rerun_ops, &__param_hvdcp_rerun, "1"); run_for(3000);
	EXPECT("p29 rerun refused while active (stays 9 V)", !strcmp(st(), "active") && m.n_apsd_rerun == 1 && strstr(sim_log, "rerun refused: state active"));
	set_param(&a6l_hv_enable_ops, &__param_hvdcp_enable, "0"); run_for(3000);
	EXPECT("p29 disable -> 5 V", m.vbus == 5000000 && !strcmp(st(), "off"));
	/* 20 FLOAT variant */
	reset_world(AD_SDP, 1, 1000000, 9000000); do_probe(); run_for(3000);
	unplug(); run_for(3000); m.pullup = 2; plug(AD_QC3); run_for(12000);
	EXPECT("p29 FLOAT: dcp-5v + warning", !strcmp(st(), "dcp-5v") && strstr(sim_log, "= FLOAT, not a BC1.2 DCP"));
	m.pullup = 0; set_param(&a6l_hv_rerun_ops, &__param_hvdcp_rerun, "1"); run_for(14000);
	EXPECT("p29 FLOAT -> rerun -> QC3 active 9.0 V", !strcmp(st(), "active") && m.vbus == 9000000 && m.n_inc == 20);
	/* 21 rerun never on a USB host */
	reset_world(AD_SDP, 1, 1000000, 9000000); do_probe(); run_for(5000);
	set_param(&a6l_hv_rerun_ops, &__param_hvdcp_rerun, "1"); run_for(3000);
	EXPECT("p29 rerun refused on SDP (ADB safe)", m.n_apsd_rerun == 0 && strstr(sim_log, "rerun refused: SDP") && icl_reg() == 500000);
	/* 22 rerun limit per plug-in, reset on unplug */
	reset_world(AD_SDP, 1, 1000000, 9000000); do_probe(); run_for(3000);
	unplug(); run_for(3000); m.pullup = 1; plug(AD_DCP); run_for(12000);
	{ int k; for (k = 0; k < 5; k++) { set_param(&a6l_hv_rerun_ops, &__param_hvdcp_rerun, "1"); run_for(12000); } }
	EXPECT("p29 at most 3 reruns per plug-in", m.n_apsd_rerun == 3 && strstr(sim_log, "rerun refused: 3 reruns"));
	unplug(); run_for(3000); plug(AD_DCP); run_for(12000);
	set_param(&a6l_hv_rerun_ops, &__param_hvdcp_rerun, "1"); run_for(12000);
	EXPECT("p29 rerun counter reset by unplug", m.n_apsd_rerun == 4 && !strcmp(st(), "dcp-5v"));
	/* 23 APSD done with an empty result when the charger type is read (result 0 until 2 s): the power28 work gave up
	 * after one look; power29 keeps polling in the plug-in window */
	reset_world(AD_SDP, 1, 1000000, 9000000); do_probe(); run_for(3000);
	unplug(); run_for(3000); m.res_late_ms = 2000; plug(AD_QC2); run_for(12000);
	EXPECT("p29 late APSD result -> QC2 active 9 V", !strcmp(st(), "active") && m.vbus == 9000000);
	EXPECT("p29 12 V never (power29 scenarios)", m.n_force12 == 0);

	/* ---- r5 bug hunt power (29 Sep 2026): input suspend set by the abort path, fast replug ---- */
	/* 24 adapter stuck high -> the driver suspends USBIN; after the unplug the next (good) charger must charge again */
	reset_world(AD_QC2, 1, 1000000, 9000000); do_probe(); run_for(10000); m.stuck_hi = 1; m.vbus = 9800000; run_for(5000);
	EXPECT("p30 stuck high: USBIN suspended by the driver", (R(0x340) & 1) && !strcmp(st(), "failed"));
	unplug(); run_for(3000); m.stuck_hi = 0; plug(AD_DCP); run_for(12000);
	EXPECT("p30 driver-owned suspend released by the unplug", !(R(0x340) & 1));
	EXPECT("p30 next charger: dcp-5v, DCP ICL 2.0 A", !strcmp(st(), "dcp-5v") && icl_reg() == 2000000);
	/* 25 userspace (guard) had suspended first: the driver must not take that suspend over and release it */
	reset_world(AD_QC2, 1, 1000000, 9000000); do_probe(); run_for(10000);
	{ union power_supply_propval v = { .intval = 0 }; the_psy.desc->set_property(&the_psy, POWER_SUPPLY_PROP_STATUS, &v); }
	m.stuck_hi = 1; m.vbus = 9800000; run_for(5000);
	EXPECT("p30 guard suspend + stuck high: failed", !strcmp(st(), "failed") && (R(0x340) & 1));
	unplug(); run_for(3000); m.stuck_hi = 0; plug(AD_DCP); run_for(12000);
	EXPECT("p30 guard-owned suspend kept across the unplug", (R(0x340) & 1));
	/* 26 driver suspend, then hvdcp_enable=0 (state off), then unplug: still released */
	reset_world(AD_QC2, 1, 1000000, 9000000); do_probe(); run_for(10000); m.stuck_hi = 1; m.vbus = 9800000; run_for(5000);
	set_param(&a6l_hv_enable_ops, &__param_hvdcp_enable, "0"); run_for(3000);
	unplug(); run_for(3000); m.stuck_hi = 0; plug(AD_DCP); run_for(12000);
	EXPECT("p30 driver suspend released on unplug after hvdcp_enable=0", !(R(0x340) & 1) && !strcmp(st(), "off"));
	/* 27 fast replug (0.1 s, before the status work and the failed-state poll): the unplug must still end the failed state (no retry = until unplug) */
	reset_world(AD_QC2, 1, 1000000, 9000000); m.overshoot_uv = 1200000; do_probe(); run_for(12000);
	EXPECT("p30 overshoot: failed", !strcmp(st(), "failed"));
	m.overshoot_uv = 0; unplug(); run_for(100); plug(AD_QC2); run_for(12000);
	EXPECT("p30 fast replug (0.1 s): fresh detection, active 9 V", !strcmp(st(), "active") && m.vbus == 9000000);
	EXPECT("p30 12 V never", m.n_force12 == 0);
}
#endif

int main(int argc, char **argv)
{
	if (argc > 1 && !strcmp(argv[1], "trace-off")) {
		trace_f = stdout;
		reset_world(AD_QC2, 0, 1000000, 9000000); trace_on = 1; do_probe(); run_for(10000);
		unplug(); run_for(3000); plug(AD_DCP); run_for(10000);
		return 0;
	}
#ifndef A6L_OLD_DRIVER
	run_all();
	if (getenv("SIM_LOG")) fputs(sim_log, stdout);
	printf("A6L_HVDCP_SIM %s (%d checks)\n", fails ? "FAIL" : "PASS", checks);
	if (fails) printf("A6L_HVDCP_SIM FAIL %d\n", fails);
	return !!fails;
#else
	return 0;
#endif
}
