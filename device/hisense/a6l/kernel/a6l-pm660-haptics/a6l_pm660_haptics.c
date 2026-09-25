// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2021, Caleb Connolly <caleb@connolly.tech>
 * Qualcomm QPMI haptics driver for pmi8998 and related PMICs.
 *
 * A6L variant (Hisense A6L, PM660 LRA), derived from the pinned 7.2.3
 * drivers/input/misc/qcom-spmi-haptics.c (sha256 e83ffefb...dcd6):
 *  - FF magnitude is 0..0xffff and is scaled to a DT/param ceiling
 *    (qcom,vmax-mv, default = stock 3200 mV) instead of the upstream
 *    "(strong>>8) as percent" formula which saturates at 3596 mV;
 *  - qcom,brake-pattern accepted as four u32 cells (stock/overlay form);
 *  - qcom,ilim-ma (400/800) honoured;
 *  - PM660 mode (default): the PMI8998-specific auto-resonance writes
 *    (LRA_AUTO_RES 0x4F mode ZXD_EOP, TEST2 0xE3 bit7) are NOT issued;
 *    the LRA is driven open-loop at qcom,wave-play-rate-us (6667 us = 150 Hz);
 *  - mutex_init() for play_lock (missing upstream);
 *  - read-only register dump in debugfs (a6l_haptics/regs) and at probe.
 */

#include <dt-bindings/input/qcom,spmi-haptics.h>

#include <linux/atomic.h>
#include <linux/bits.h>
#include <linux/bitfield.h>
#include <linux/errno.h>
#include <linux/input.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/log2.h>
#include <linux/minmax.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/slab.h>
#include <linux/time.h>
#include <linux/types.h>
#include <linux/debugfs.h>
#include <linux/uaccess.h>
#include <linux/fs.h>
#include <linux/delay.h>
#include <linux/seq_file.h>
#include <linux/moduleparam.h>

static bool pm660_mode = true;
module_param(pm660_mode, bool, 0444);
MODULE_PARM_DESC(pm660_mode, "skip PMI8998 auto-resonance/TEST2 writes (default: true)");
static uint max_mv;
module_param(max_mv, uint, 0644);
MODULE_PARM_DESC(max_mv, "optional lower vmax ceiling in mV (0 = use DT qcom,vmax-mv)");
/* misc agent 24 Sep 2026: stock (qpnp-haptic, DT qcom,haptic@c000) parity knobs */
static unsigned int ilim_ma;
module_param(ilim_ma, uint, 0444);
MODULE_PARM_DESC(ilim_ma, "current limit 400/800 mA (0 = DT qcom,ilim-ma, else stock 800)");
static unsigned int sc_deb_cycles = 8;
module_param(sc_deb_cycles, uint, 0444);
MODULE_PARM_DESC(sc_deb_cycles, "short-circuit debounce 0/8/16/32 cycles (stock 8; old driver wrote 0)");
static unsigned int int_pwm_khz = 505;
module_param(int_pwm_khz, uint, 0444);
MODULE_PARM_DESC(int_pwm_khz, "internal PWM 253/505/739/1076 kHz written to INT_PWM 0x56 + PWM_CAP 0x58 (stock 505, 0 = leave)");
static bool pm660_autores;
module_param(pm660_autores, bool, 0644);
MODULE_PARM_DESC(pm660_autores, "LRA auto-resonance via AUTO_RES_CTRL 0x4B bit7 around PLAY (stock QWD does this)");
#define A6L_STOCK_VMAX_MV 3200


#define HAP_STATUS_1_REG		0x0A
#define HAP_BUSY_BIT			BIT(1)
#define SC_FLAG_BIT			BIT(3)
#define AUTO_RES_ERROR_BIT		BIT(4)

#define HAP_LRA_AUTO_RES_LO_REG		0x0B
#define HAP_LRA_AUTO_RES_HI_REG		0x0C

#define HAP_EN_CTL_REG			0x46
#define HAP_EN_BIT			BIT(7)

#define HAP_EN_CTL2_REG			0x48
#define BRAKE_EN_BIT			BIT(0)

#define HAP_AUTO_RES_CTRL_REG		0x4B
#define AUTO_RES_EN_BIT			BIT(7)
#define AUTO_RES_ERR_RECOVERY_BIT	BIT(3)
#define AUTO_RES_EN_FLAG_BIT		BIT(0)

#define HAP_CFG1_REG			0x4C
#define HAP_ACT_TYPE_MASK		BIT(0)

#define HAP_CFG2_REG			0x4D
#define HAP_LRA_RES_TYPE_MASK		BIT(0)

#define HAP_SEL_REG			0x4E
#define HAP_WF_SOURCE_MASK		GENMASK(5, 4)
#define HAP_WF_SOURCE_SHIFT		4

#define HAP_LRA_AUTO_RES_REG		0x4F
#define LRA_AUTO_RES_MODE_MASK		GENMASK(6, 4)
#define LRA_AUTO_RES_MODE_SHIFT		4
#define LRA_HIGH_Z_MASK			GENMASK(3, 2)
#define LRA_HIGH_Z_SHIFT		2
#define LRA_RES_CAL_MASK		GENMASK(1, 0)
#define HAP_RES_CAL_PERIOD_MIN		4
#define HAP_RES_CAL_PERIOD_MAX		32

#define HAP_VMAX_CFG_REG		0x51
#define HAP_VMAX_OVD_BIT		BIT(6)
#define HAP_VMAX_MASK			GENMASK(5, 1)
#define HAP_VMAX_SHIFT			1

#define HAP_ILIM_CFG_REG		0x52
#define HAP_ILIM_SEL_MASK		BIT(0)
#define HAP_ILIM_400_MA			0
#define HAP_ILIM_800_MA			1

#define HAP_SC_DEB_REG			0x53
#define HAP_SC_DEB_MASK			GENMASK(2, 0)
#define HAP_SC_DEB_CYCLES_MIN		0
#define HAP_DEF_SC_DEB_CYCLES		8
#define HAP_SC_DEB_CYCLES_MAX		32

#define HAP_RATE_CFG1_REG		0x54
#define HAP_RATE_CFG1_MASK		GENMASK(7, 0)
#define HAP_RATE_CFG2_SHIFT		8

#define HAP_RATE_CFG2_REG		0x55
#define HAP_RATE_CFG2_MASK		GENMASK(3, 0)

#define HAP_INT_PWM_REG			0x56
#define HAP_PWM_CAP_REG			0x58
#define HAP_SC_CLR_REG			0x59
#define SC_CLR_BIT			BIT(0)

#define HAP_BRAKE_REG			0x5C
#define HAP_BRAKE_PAT_MASK		0x3

#define HAP_WF_REPEAT_REG		0x5E
#define WF_REPEAT_MASK			GENMASK(6, 4)
#define WF_REPEAT_SHIFT			4
#define WF_REPEAT_MIN			1
#define WF_REPEAT_MAX			128
#define WF_S_REPEAT_MASK		GENMASK(1, 0)
#define WF_S_REPEAT_MIN			1
#define WF_S_REPEAT_MAX			8

#define HAP_WF_S1_REG			0x60
#define HAP_WF_SIGN_BIT			BIT(7)
#define HAP_WF_OVD_BIT			BIT(6)
#define HAP_WF_SAMP_MAX			GENMASK(5, 1)
#define HAP_WF_SAMPLE_LEN		8

#define HAP_PLAY_REG			0x70
#define HAP_PLAY_BIT			BIT(7)
#define HAP_PAUSE_BIT			BIT(0)

#define HAP_SEC_ACCESS_REG		0xD0
#define HAP_SEC_ACCESS_UNLOCK		0xA5

#define HAP_TEST2_REG			0xE3


#define HAP_VMAX_MIN_MV			116
#define HAP_VMAX_MAX_MV			3596
#define HAP_VMAX_MAX_MV_STRONG		3596

#define HAP_WAVE_PLAY_RATE_MIN_US	0
#define HAP_WAVE_PLAY_RATE_MAX_US	20475
#define HAP_WAVE_PLAY_TIME_MAX_MS	15000

#define AUTO_RES_ERR_POLL_TIME_NS	(20 * NSEC_PER_MSEC)
#define HAPTICS_BACK_EMF_DELAY_US	20000

#define HAP_BRAKE_PAT_LEN		4
#define HAP_WAVE_SAMP_LEN		8
#define NUM_WF_SET			4
#define HAP_WAVE_SAMP_SET_LEN		(HAP_WAVE_SAMP_LEN * NUM_WF_SET)
#define HAP_RATE_CFG_STEP_US		5

#define SC_MAX_COUNT			5
#define SC_COUNT_RST_DELAY_US		1000000

enum hap_play_control {
	HAP_STOP,
	HAP_PAUSE,
	HAP_PLAY,
};

/**
 * struct spmi_haptics - struct for spmi haptics data.
 *
 * @dev: Our device parent.
 * @regmap: Register map for the hardware block.
 * @haptics_input_dev: The input device used to receive events.
 * @work: Work struct to play effects.
 * @base: Base address of the regmap.
 * @active: Atomic value used to track if haptics are currently playing.
 * @play_irq: Fired to load the next wave pattern.
 * @sc_irq: Short circuit irq.
 * @last_sc_time: Time since the short circuit IRQ last fired.
 * @sc_count: Number of times the short circuit IRQ has fired in this interval.
 * @actuator_type: The type of actuator in use.
 * @wave_shape: The shape of the waves to use (sine or square).
 * @play_mode: The play mode to use (direct, buffer, pwm, audio).
 * @magnitude: The strength we should be playing at.
 * @vmax: Max voltage to use when playing.
 * @current_limit: The current limit for this hardware (400mA or 800mA).
 * @play_wave_rate: The wave rate to use for this hardware.
 * @wave_samp: The array of wave samples to write for buffer mode.
 * @brake_pat: The pattern to apply when braking.
 * @play_lock: Lock to be held when updating the hardware state.
 */
struct spmi_haptics {
	struct device *dev;
	struct regmap *regmap;
	struct input_dev *haptics_input_dev;
	struct work_struct work;
	u32 base;

	atomic_t active;

	int play_irq;
	int sc_irq;
	ktime_t last_sc_time;
	u8 sc_count;

	u8 actuator_type;
	u8 wave_shape;
	u8 play_mode;
	int magnitude;
	u32 vmax;
	u32 vmax_cap;
	u32 current_limit;
	struct dentry *dbg;
	u32 play_wave_rate;

	u32 wave_samp[HAP_WAVE_SAMP_SET_LEN];
	u8 brake_pat[HAP_BRAKE_PAT_LEN];

	struct mutex play_lock;
	unsigned int sc_events;
};

static inline bool is_secure_addr(u16 addr)
{
	return (addr & 0xFF) > 0xD0;
}

static int spmi_haptics_read(struct spmi_haptics *haptics,
	u16 addr, u8 *val, int len)
{
	int ret;

	ret = regmap_bulk_read(haptics->regmap, addr, val, len);
	if (ret < 0)
		dev_err(haptics->dev, "Error reading address: 0x%x, ret %d\n", addr, ret);

	return ret;
}

static int spmi_haptics_write(struct spmi_haptics *haptics,
			      u16 addr, u8 *val, int len)
{
	int ret, i;

	if (is_secure_addr(addr)) {
		for (i = 0; i < len; i++) {
			dev_dbg(haptics->dev, "%s: unlocking for addr: 0x%x, val: 0x%x", __func__,
				addr, val[i]);
			ret = regmap_write(haptics->regmap,
				haptics->base + HAP_SEC_ACCESS_REG, HAP_SEC_ACCESS_UNLOCK);
			if (ret < 0) {
				dev_err(haptics->dev, "Error writing unlock code, ret %d\n",
					ret);
				return ret;
			}

			ret = regmap_write(haptics->regmap, addr + i, val[i]);
			if (ret < 0) {
				dev_err(haptics->dev, "Error writing address 0x%x, ret %d\n",
					addr + i, ret);
				return ret;
			}
		}
	} else {
		if (len > 1)
			ret = regmap_bulk_write(haptics->regmap, addr, val, len);
		else
			ret = regmap_write(haptics->regmap, addr, *val);
	}

	if (ret < 0)
		dev_err(haptics->dev, "%s: Error writing address: 0x%x, ret %d\n",
			__func__, addr, ret);

	return ret;
}

static int spmi_haptics_write_masked(struct spmi_haptics *haptics,
	u16 addr, u8 mask, u8 val)
{
	int ret;

	if (is_secure_addr(addr)) {
		ret = regmap_write(haptics->regmap,
			haptics->base + HAP_SEC_ACCESS_REG, HAP_SEC_ACCESS_UNLOCK);
		if (ret < 0) {
			dev_err(haptics->dev, "Error writing unlock code - ret %d\n", ret);
			return ret;
		}
	}

	ret = regmap_update_bits(haptics->regmap, addr, mask, val);
	if (ret < 0)
		dev_err(haptics->dev, "Error writing address: 0x%x - ret %d\n", addr, ret);

	return ret;
}

static bool is_haptics_idle(struct spmi_haptics *haptics)
{
	int ret;
	u8 val;

	if (haptics->play_mode == HAP_PLAY_DIRECT ||
			haptics->play_mode == HAP_PLAY_PWM)
		return true;

	ret = spmi_haptics_read(haptics, haptics->base + HAP_STATUS_1_REG, &val, 1);
	if (ret < 0 || (val & HAP_BUSY_BIT))
		return false;

	return true;
}

static int spmi_haptics_module_enable(struct spmi_haptics *haptics, bool enable)
{
	u8 val;

	dev_dbg(haptics->dev, "Setting module enable: %d", enable);

	val = enable ? HAP_EN_BIT : 0;
	return spmi_haptics_write(haptics, haptics->base + HAP_EN_CTL_REG, &val, 1);
}

static int spmi_haptics_write_vmax(struct spmi_haptics *haptics)
{
	u8 val = 0;
	u32 vmax_mv = haptics->vmax;

	vmax_mv = clamp_t(u32, vmax_mv, HAP_VMAX_MIN_MV, HAP_VMAX_MAX_MV);

	dev_dbg(haptics->dev, "Setting vmax to: %d", vmax_mv);

	val = DIV_ROUND_CLOSEST(vmax_mv, HAP_VMAX_MIN_MV);
	val = FIELD_PREP(HAP_VMAX_MASK, val);

	// TODO: pm660 can enable overdrive here

	return spmi_haptics_write_masked(haptics, haptics->base + HAP_VMAX_CFG_REG,
					 HAP_VMAX_MASK | HAP_WF_OVD_BIT, val);
}

static int spmi_haptics_write_current_limit(struct spmi_haptics *haptics)
{
	haptics->current_limit = clamp_t(u32, haptics->current_limit,
					 HAP_ILIM_400_MA, HAP_ILIM_800_MA);

	dev_dbg(haptics->dev, "Setting current_limit to: 0x%x", haptics->current_limit);

	return spmi_haptics_write_masked(haptics, haptics->base + HAP_ILIM_CFG_REG,
			HAP_ILIM_SEL_MASK, haptics->current_limit);
}

static int spmi_haptics_write_play_mode(struct spmi_haptics *haptics)
{
	u8 val = 0;

	if (!is_haptics_idle(haptics))
		return -EBUSY;

	dev_dbg(haptics->dev, "Setting play_mode to: 0x%x", haptics->play_mode);

	val = FIELD_PREP(HAP_WF_SOURCE_MASK, haptics->play_mode);
	return spmi_haptics_write_masked(haptics, haptics->base + HAP_SEL_REG,
					 HAP_WF_SOURCE_MASK, val);

}

static int spmi_haptics_write_play_rate(struct spmi_haptics *haptics, u16 play_rate)
{
	u8 val[2];

	dev_dbg(haptics->dev, "Setting play_rate to: %d", play_rate);

	val[0] = FIELD_PREP(HAP_RATE_CFG1_MASK, play_rate);
	val[1] = FIELD_PREP(HAP_RATE_CFG2_MASK, play_rate >> HAP_RATE_CFG2_SHIFT);
	return spmi_haptics_write(haptics, haptics->base + HAP_RATE_CFG1_REG, val, 2);
}

/*
 * spmi_haptics_set_auto_res() - Auto resonance
 * allows the haptics to automatically adjust the
 * speed of the oscillation in order to maintain
 * the resonant frequency.
 */
static int spmi_haptics_set_auto_res(struct spmi_haptics *haptics, bool enable)
{
	u8 val = 0;

	if (haptics->actuator_type == HAP_TYPE_ERM)
		return spmi_haptics_write(haptics, haptics->base + HAP_LRA_AUTO_RES_REG,
					  &val, 1);

	// LRAs are the only type to support auto res
	if (haptics->actuator_type != HAP_TYPE_LRA)
		return 0;

	/* PM660 auto-res lives in AUTO_RES_CTRL 0x4B bit7 (qpnp-haptic PM660 path); never poke TEST2 */
	if (pm660_mode) {
		if (!pm660_autores && enable)
			return 0;
		return spmi_haptics_write_masked(haptics, haptics->base + HAP_AUTO_RES_CTRL_REG,
						 AUTO_RES_EN_BIT, enable ? AUTO_RES_EN_BIT : 0);
	}

	val = enable ? AUTO_RES_EN_BIT : 0;

	return spmi_haptics_write_masked(haptics, haptics->base + HAP_TEST2_REG,
				       AUTO_RES_EN_BIT, val);
}

static int spmi_haptics_write_brake(struct spmi_haptics *haptics)
{
	int ret, i;
	u8 val;

	dev_dbg(haptics->dev, "Configuring brake pattern");

	ret = spmi_haptics_write_masked(haptics, haptics->base + HAP_EN_CTL2_REG,
					BRAKE_EN_BIT, 1);
	if (ret < 0)
		return ret;

	for (i = HAP_BRAKE_PAT_LEN - 1, val = 0; i >= 0; i--) {
		u8 p = haptics->brake_pat[i] & HAP_BRAKE_PAT_MASK;

		val |= p << (i * 2);
	}

	return spmi_haptics_write(haptics, haptics->base + HAP_BRAKE_REG, &val, 1);
}

static int spmi_haptics_write_buffer_config(struct spmi_haptics *haptics)
{
	u8 buf[HAP_WAVE_SAMP_LEN];
	int i;

	dev_dbg(haptics->dev, "Writing buffer config");

	for (i = 0; i < HAP_WAVE_SAMP_LEN; i++)
		buf[i] = haptics->wave_samp[i];

	return spmi_haptics_write(haptics, haptics->base + HAP_WF_S1_REG, buf,
				  HAP_WAVE_SAMP_LEN);
}

static int spmi_haptics_write_wave_repeat(struct spmi_haptics *haptics)
{
	u8 val, mask;

	/* The number of times to repeat each wave */
	mask = WF_REPEAT_MASK | WF_S_REPEAT_MASK;
	val = FIELD_PREP(WF_REPEAT_MASK, 0) |
	      FIELD_PREP(WF_S_REPEAT_MASK, 0);

	return spmi_haptics_write_masked(haptics, haptics->base + HAP_WF_REPEAT_REG,
					 mask, val);
}

static int spmi_haptics_write_play_control(struct spmi_haptics *haptics,
						enum hap_play_control ctrl)
{
	u8 val;

	switch (ctrl) {
	case HAP_STOP:
		val = 0;
		break;
	case HAP_PAUSE:
		val = HAP_PAUSE_BIT;
		break;
	case HAP_PLAY:
		val = HAP_PLAY_BIT;
		break;
	default:
		return 0;
	}

	dev_dbg(haptics->dev, "haptics play ctrl: %d\n", ctrl);
	return spmi_haptics_write(haptics, haptics->base + HAP_PLAY_REG, &val, 1);
}

/*
 * This IRQ is fired to tell us to load the next wave sample set.
 * As we only currently support a single sample set, it's unused.
 */
static irqreturn_t spmi_haptics_play_irq_handler(int irq, void *data)
{
	struct spmi_haptics *haptics = data;

	dev_dbg(haptics->dev, "play_irq triggered");

	return IRQ_HANDLED;
}

/**
 * spmi_haptics_sc_irq_handler() - short circuit irq handler
 * Fires every ~50ms whilst the haptics are active.
 * If the SC_FLAG_BIT is set then that means there isn't a short circuit
 * and we just need to clear the IRQ to indicate that the device should
 * keep vibrating.
 *
 * Otherwise, it means a short circuit situation has occurred.
 *
 * @irq: irq number
 * @data: haptics data
 * Returns: IRQ_HANDLED
 */
static irqreturn_t spmi_haptics_sc_irq_handler(int irq, void *data)
{
	struct spmi_haptics *haptics = data;
	int ret;
	u8 val;
	s64 sc_delta_time_us;
	ktime_t temp;

	ret = spmi_haptics_read(haptics, haptics->base + HAP_STATUS_1_REG, &val, 1);
	if (ret < 0)
		return IRQ_HANDLED;

	if (!(val & SC_FLAG_BIT)) {
		haptics->sc_count = 0;
		return IRQ_HANDLED;
	}

	temp = ktime_get();
	sc_delta_time_us = ktime_us_delta(temp, haptics->last_sc_time);
	haptics->last_sc_time = temp;

	if (sc_delta_time_us > SC_COUNT_RST_DELAY_US)
		haptics->sc_count = 0;
	else
		haptics->sc_count++;

	// Clear the interrupt flag
	val = SC_CLR_BIT;
	ret = spmi_haptics_write(haptics, haptics->base + HAP_SC_CLR_REG, &val, 1);
	if (ret < 0)
		return IRQ_HANDLED;

	haptics->sc_events++;
	dev_warn_ratelimited(haptics->dev, "A6L_HAP short-circuit flag (status 0x%02x, count %u, total %u)\n",
			     val, haptics->sc_count, haptics->sc_events);
	if (haptics->sc_count > SC_MAX_COUNT) {
		dev_err(haptics->dev, "Short circuit persists, disabling haptics\n");
		ret = spmi_haptics_module_enable(haptics, false);
		if (ret < 0)
			dev_err(haptics->dev, "Error disabling module, rc=%d\n", ret);
	}

	return IRQ_HANDLED;
}


static const u8 a6l_dump_regs[] = {
	0x04, 0x05, 0x08, 0x0A, 0x0B, 0x0C, 0x46, 0x48, 0x4B, 0x4C, 0x4D, 0x4E,
	0x4F, 0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x58, 0x5C, 0x5E, 0x60, 0x61, 0x70,
};

static void a6l_haptics_dump(struct spmi_haptics *haptics, const char *tag)
{
	char buf[256];
	int i, n = 0;
	unsigned int v;

	for (i = 0; i < ARRAY_SIZE(a6l_dump_regs); i++) {
		if (regmap_read(haptics->regmap, haptics->base + a6l_dump_regs[i], &v))
			v = 0xfff;
		n += scnprintf(buf + n, sizeof(buf) - n, " %02x=%02x", a6l_dump_regs[i], v);
	}
	dev_info(haptics->dev, "A6L_HAP %s vmax_cap=%u ilim=%s pm660=%d:%s\n", tag,
		 haptics->vmax_cap, haptics->current_limit ? "800" : "400", pm660_mode, buf);
}

static int a6l_regs_show(struct seq_file *s, void *unused)
{
	struct spmi_haptics *haptics = s->private;
	unsigned int v;
	int i;

	for (i = 0; i < 0x80; i++) {
		if (regmap_read(haptics->regmap, haptics->base + i, &v))
			continue;
		seq_printf(s, "%s%02x=%02x", (i % 16) ? " " : (i ? "\n" : ""), i, v);
	}
	seq_printf(s, "\nvmax=%u vmax_cap=%u magnitude=%d active=%d\n", haptics->vmax,
		   haptics->vmax_cap, haptics->magnitude, atomic_read(&haptics->active));
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(a6l_regs);


/*
 * debugfs a6l_haptics/test (write-only, root): one synchronous, bounded pulse with register snapshots DURING play.
 *   echo "pulse <ms> <mv> [rate_us] [autores 0|1] [erm 0|1]" > /sys/kernel/debug/a6l_haptics/test
 * ms 10..1000, mv 116..3200 (ERM: ms <= 100, mv <= 2000), rate_us 3000..10000 (LRA drive period: 333..100 Hz;
 * 0 = DT value). Snapshots "A6L_HAP during t=<ms> ..." go to the kernel log (regs 0A status, 0B/0C auto-res
 * period readback, 46 enable, 4B auto-res ctrl, 4C type, 51 vmax, 52 ilim, 54/55 rate, 70 play).
 */
static const u8 a6l_during_regs[] = { 0x0A, 0x0B, 0x0C, 0x46, 0x48, 0x4B, 0x4C, 0x51, 0x52, 0x53, 0x54, 0x55, 0x70 };

static void a6l_snap(struct spmi_haptics *h, const char *tag, int t_ms)
{
	char buf[160];
	unsigned int v;
	int i, n = 0;

	for (i = 0; i < ARRAY_SIZE(a6l_during_regs); i++) {
		if (regmap_read(h->regmap, h->base + a6l_during_regs[i], &v))
			v = 0xfff;
		n += scnprintf(buf + n, sizeof(buf) - n, " %02x=%02x", a6l_during_regs[i], v);
	}
	dev_info(h->dev, "A6L_HAP %s t=%d sc=%u:%s\n", tag, t_ms, h->sc_events, buf);
}

static ssize_t a6l_test_write(struct file *f, const char __user *ubuf, size_t len, loff_t *ppos)
{
	struct spmi_haptics *h = file_inode(f)->i_private;
	char cmd[96];
	unsigned int ms = 0, mv = 0, rate = 0, ares = 0, erm = 0;
	unsigned int save[6];
	static const u8 save_regs[6] = { 0x4B, 0x4C, 0x51, 0x54, 0x55, 0x52 };
	int n, i, t, ret = 0;
	u8 v8;

	if (len >= sizeof(cmd))
		return -EINVAL;
	if (copy_from_user(cmd, ubuf, len))
		return -EFAULT;
	cmd[len] = 0;
	n = sscanf(cmd, "pulse %u %u %u %u %u", &ms, &mv, &rate, &ares, &erm);
	if (n < 2 || ms < 10 || ms > 1000 || mv < HAP_VMAX_MIN_MV || mv > A6L_STOCK_VMAX_MV ||
	    (rate && (rate < 3000 || rate > 10000)) || (erm && (ms > 100 || mv > 2000)))
		return -EINVAL;
	if (atomic_read(&h->active))
		return -EBUSY;

	mutex_lock(&h->play_lock);
	for (i = 0; i < 6; i++)
		if (regmap_read(h->regmap, h->base + save_regs[i], &save[i]))
			save[i] = 0x100;
	dev_info(h->dev, "A6L_HAP test pulse ms=%u mv=%u rate_us=%u autores=%u erm=%u ilim=%s\n", ms, mv,
		 rate ? rate : h->play_wave_rate, ares, erm, h->current_limit ? "800" : "400");
	if (erm)
		spmi_haptics_write_masked(h, h->base + HAP_CFG1_REG, HAP_ACT_TYPE_MASK, HAP_TYPE_ERM);
	if (rate)
		spmi_haptics_write_play_rate(h, rate / HAP_RATE_CFG_STEP_US);
	v8 = FIELD_PREP(HAP_VMAX_MASK, DIV_ROUND_CLOSEST(mv, HAP_VMAX_MIN_MV));
	spmi_haptics_write_masked(h, h->base + HAP_VMAX_CFG_REG, HAP_VMAX_MASK | HAP_WF_OVD_BIT, v8);
	spmi_haptics_write_masked(h, h->base + HAP_AUTO_RES_CTRL_REG, AUTO_RES_EN_BIT, 0);
	a6l_snap(h, "before", 0);
	ret = spmi_haptics_module_enable(h, true);
	if (!ret)
		ret = spmi_haptics_write_play_control(h, HAP_PLAY);
	for (t = 0; !ret && t < (int)ms; ) {
		int step = t < 50 ? 5 : 50;

		if (t == 20 && ares && !erm)
			spmi_haptics_write_masked(h, h->base + HAP_AUTO_RES_CTRL_REG, AUTO_RES_EN_BIT,
						  AUTO_RES_EN_BIT);
		if (t == 5 || t == 25 || t == 50 || (t % 100 == 0 && t) || t + step >= (int)ms)
			a6l_snap(h, "during", t);
		msleep(step);
		t += step;
	}
	spmi_haptics_write_play_control(h, HAP_STOP);
	spmi_haptics_module_enable(h, false);
	a6l_snap(h, "after", ms);
	for (i = 0; i < 6; i++)
		if (save[i] < 0x100)
			regmap_write(h->regmap, h->base + save_regs[i], save[i]);
	mutex_unlock(&h->play_lock);
	return ret ? ret : len;
}

static const struct file_operations a6l_test_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.write = a6l_test_write,
};

/**
 * spmi_haptics_init() - Initialise haptics hardware for use
 * @haptics: haptics device
 * Returns: 0 on success, < 0 on error
 */
static int spmi_haptics_init(struct spmi_haptics *haptics)
{
	int ret;
	u8 val, mask;
	u16 play_rate;

	ret = spmi_haptics_write_masked(haptics, haptics->base + HAP_CFG1_REG,
		HAP_ACT_TYPE_MASK, haptics->actuator_type);
	if (ret < 0)
		return ret;

	/*
	 * Configure auto resonance
	 * see spmi_haptics_lra_auto_res_config downstream
	 * This is greatly simplified.
	 */
	val = FIELD_PREP(LRA_RES_CAL_MASK, ilog2(32 / HAP_RES_CAL_PERIOD_MIN)) |
	      FIELD_PREP(LRA_AUTO_RES_MODE_MASK, HAP_AUTO_RES_ZXD_EOP) |
	      FIELD_PREP(LRA_HIGH_Z_MASK, 1);

	mask = LRA_AUTO_RES_MODE_MASK | LRA_HIGH_Z_MASK | LRA_RES_CAL_MASK;

	if (!pm660_mode)
		ret = spmi_haptics_write_masked(haptics, haptics->base + HAP_LRA_AUTO_RES_REG,
				mask, val);

	/* Configure the PLAY MODE register */
	ret = spmi_haptics_write_play_mode(haptics);
	if (ret < 0)
		return ret;

	ret = spmi_haptics_write_vmax(haptics);
	if (ret < 0)
		return ret;

	/* Configure the ILIM register */
	ret = spmi_haptics_write_current_limit(haptics);
	if (ret < 0)
		return ret;

	// Configure the debounce for short-circuit detection.
	/* qpnp-haptic encoding: 0 = off, 8 -> 1, 16 -> 2, 32 -> 3 (the old code wrote 32 & 0x7 = 0 = no debounce) */
	val = !sc_deb_cycles ? 0 : sc_deb_cycles <= 8 ? 1 : sc_deb_cycles <= 16 ? 2 : 3;
	ret = spmi_haptics_write_masked(haptics, haptics->base + HAP_SC_DEB_REG,
			HAP_SC_DEB_MASK, val);
	if (ret < 0)
		return ret;

	// write the wave shape
	ret = spmi_haptics_write_masked(haptics, haptics->base + HAP_CFG2_REG,
			HAP_LRA_RES_TYPE_MASK, haptics->wave_shape);
	if (ret < 0)
		return ret;

	if (int_pwm_khz) {
		val = int_pwm_khz <= 253 ? 0 : int_pwm_khz <= 505 ? 1 : int_pwm_khz <= 739 ? 2 : 3;
		ret = spmi_haptics_write_masked(haptics, haptics->base + HAP_INT_PWM_REG, 0x3, val);
		if (!ret)
			ret = spmi_haptics_write_masked(haptics, haptics->base + HAP_PWM_CAP_REG, 0x3, val);
		if (ret < 0)
			return ret;
	}

	play_rate = haptics->play_wave_rate / HAP_RATE_CFG_STEP_US;

	/*
	 * Configure RATE_CFG1 and RATE_CFG2 registers.
	 * Note: For ERM these registers act as play rate and
	 * for LRA these represent resonance period
	 */
	ret = spmi_haptics_write_play_rate(haptics, play_rate);
	if (ret < 0)
		return ret;

	ret = spmi_haptics_write_brake(haptics);
	if (ret < 0)
		return ret;

	if (haptics->play_mode == HAP_PLAY_BUFFER) {
		ret = spmi_haptics_write_wave_repeat(haptics);
		if (ret < 0)
			return ret;

		ret = spmi_haptics_write_buffer_config(haptics);
		if (ret < 0)
			return ret;
	}

	dev_dbg(haptics->dev, "%s: Requesting play IRQ, irq: %d", __func__,
		haptics->play_irq);
	ret = devm_request_threaded_irq(haptics->dev, haptics->play_irq,
		NULL, spmi_haptics_play_irq_handler, IRQF_ONESHOT,
		"haptics_play_irq", haptics);

	if (ret < 0) {
		dev_err(haptics->dev, "Unable to request play IRQ ret=%d\n", ret);
		return ret;
	}

	/* use play_irq only for buffer mode */
	if (haptics->play_mode != HAP_PLAY_BUFFER)
		disable_irq(haptics->play_irq);

	dev_dbg(haptics->dev, "%s: Requesting play IRQ, irq: %d", __func__,
		haptics->play_irq);
	ret = devm_request_threaded_irq(haptics->dev, haptics->sc_irq,
		NULL, spmi_haptics_sc_irq_handler, IRQF_ONESHOT,
		"haptics_sc_irq", haptics);

	if (ret < 0) {
		dev_err(haptics->dev, "Unable to request sc IRQ ret=%d\n", ret);
		return ret;
	}

	return 0;
}

/**
 * spmi_haptics_enable - handler to start/stop vibration
 * @haptics: pointer to haptics struct
 * Returns: 0 on success, < 0 on failure
 */
static int spmi_haptics_enable(struct spmi_haptics *haptics)
{
	int ret;

	mutex_lock(&haptics->play_lock);
	if (haptics->sc_count > SC_MAX_COUNT) {
		dev_err(haptics->dev, "Can't play while in short circuit");
		ret = -1;
		goto out;
	}
	ret = spmi_haptics_set_auto_res(haptics, false);
	if (ret < 0) {
		dev_err(haptics->dev, "Error disabling auto_res, ret=%d\n", ret);
		goto out;
	}

	ret = spmi_haptics_module_enable(haptics, true);
	if (ret < 0) {
		dev_err(haptics->dev, "Error enabling module, ret=%d\n", ret);
		goto out;
	}

	ret = spmi_haptics_write_play_control(haptics, HAP_PLAY);
	if (ret < 0) {
		dev_err(haptics->dev, "Error enabling play, ret=%d\n", ret);
		goto out;
	}

	if (pm660_mode && pm660_autores)
		usleep_range(20000, 21000);	/* back-EMF settle before QWD auto-res (stock) */
	ret = spmi_haptics_set_auto_res(haptics, true);
	if (ret < 0) {
		dev_err(haptics->dev, "Error enabling auto_res, ret=%d\n", ret);
		goto out;
	}

out:
	mutex_unlock(&haptics->play_lock);
	return ret;
}

/**
 * spmi_haptics_enable - handler to start/stop vibration
 * @haptics: pointer to haptics struct
 * Returns: 0 on success, < 0 on failure
 */
static int spmi_haptics_disable(struct spmi_haptics *haptics)
{
	int ret;

	mutex_lock(&haptics->play_lock);

	ret = spmi_haptics_write_play_control(haptics, HAP_STOP);
	if (ret < 0) {
		dev_err(haptics->dev, "Error disabling play, ret=%d\n", ret);
		goto out;
	}

	ret = spmi_haptics_module_enable(haptics, false);
	if (ret < 0) {
		dev_err(haptics->dev, "Error disabling module, ret=%d\n", ret);
		goto out;
	}

out:
	mutex_unlock(&haptics->play_lock);
	return ret;
}

/*
 * Threaded function to update the haptics state.
 */
static void spmi_haptics_work(struct work_struct *work)
{
	struct spmi_haptics *haptics = container_of(work, struct spmi_haptics, work);

	int ret;
	bool enable;

	enable = atomic_read(&haptics->active);
	dev_dbg(haptics->dev, "%s: state: %d\n", __func__, enable);

	if (enable)
		ret = spmi_haptics_enable(haptics);
	else
		ret = spmi_haptics_disable(haptics);
	if (ret < 0)
		dev_err(haptics->dev, "Error setting haptics, ret=%d", ret);
}

/**
 * spmi_haptics_close - callback for input device close
 * @dev: input device pointer
 *
 * Turns off the vibrator.
 */
static void spmi_haptics_close(struct input_dev *dev)
{
	struct spmi_haptics *haptics = input_get_drvdata(dev);

	cancel_work_sync(&haptics->work);
	if (atomic_read(&haptics->active)) {
		atomic_set(&haptics->active, 0);
		schedule_work(&haptics->work);
	}
}

/**
 * spmi_haptics_play_effect - play haptics effects
 * @dev: input device pointer
 * @data: data of effect
 * @effect: effect to play
 */
static int spmi_haptics_play_effect(struct input_dev *dev, void *data,
					struct ff_effect *effect)
{
	struct spmi_haptics *haptics = input_get_drvdata(dev);

	dev_dbg(haptics->dev, "%s: Rumbling with strong: %d and weak: %d", __func__,
		effect->u.rumble.strong_magnitude, effect->u.rumble.weak_magnitude);

	if (effect->type == FF_CONSTANT) {
		/* ff-memless combined constant: signed level (sign depends on direction) */
		int lvl = abs((int)effect->u.constant.level);

		haptics->magnitude = min(0xffff, lvl * 2);
	} else {
		/* FF_RUMBLE 0..0xffff, strong wins; weak counts at half strength */
		haptics->magnitude = effect->u.rumble.strong_magnitude;
		if (!haptics->magnitude)
			haptics->magnitude = effect->u.rumble.weak_magnitude >> 1;
	}

	if (!haptics->magnitude) {
		atomic_set(&haptics->active, 0);
		goto end;
	}

	atomic_set(&haptics->active, 1);

	{
		u32 cap = haptics->vmax_cap;

		if (max_mv && max_mv < cap)
			cap = max_mv;
		haptics->vmax = (u32)(((u64)cap * haptics->magnitude) / 0xffff);
		if (haptics->vmax < HAP_VMAX_MIN_MV)
			haptics->vmax = HAP_VMAX_MIN_MV;
		if (haptics->vmax > cap)
			haptics->vmax = cap;
	}

	dev_dbg(haptics->dev, "%s: magnitude: %d, vmax: %d", __func__,
		haptics->magnitude, haptics->vmax);

	spmi_haptics_write_vmax(haptics);

end:
	schedule_work(&haptics->work);

	return 0;
}

static int spmi_haptics_probe(struct platform_device *pdev)
{
	struct spmi_haptics *haptics;
	struct device_node *node;
	struct input_dev *input_dev;
	int ret;
	u32 val;
	int i;

	haptics = devm_kzalloc(&pdev->dev, sizeof(*haptics), GFP_KERNEL);
	if (!haptics)
		return -ENOMEM;

	haptics->regmap = dev_get_regmap(pdev->dev.parent, NULL);
	if (!haptics->regmap)
		return -ENODEV;

	node = pdev->dev.of_node;

	haptics->dev = &pdev->dev;

	ret = of_property_read_u32(node, "reg", &haptics->base);
	if (ret < 0) {
		dev_err(haptics->dev, "Couldn't find reg in node = %s ret = %d\n",
			node->full_name, ret);
		return ret;
	}

	haptics->play_irq = platform_get_irq_byname(pdev, "play");
	if (haptics->play_irq < 0) {
		dev_err(&pdev->dev, "Unable to get play irq\n");
		ret = haptics->play_irq;
		goto register_fail;
	}

	haptics->sc_irq = platform_get_irq_byname(pdev, "short");
	if (haptics->sc_irq < 0) {
		dev_err(&pdev->dev, "Unable to get sc irq\n");
		ret = haptics->sc_irq;
		goto register_fail;
	}

	haptics->actuator_type = HAP_TYPE_LRA;
	ret = of_property_read_u32(node, "qcom,actuator-type", &val);
	if (!ret) {
		if (val != HAP_TYPE_ERM && val != HAP_TYPE_LRA) {
			dev_err(&pdev->dev, "qcom,actuator-type (%d) isn't supported\n", val);
			ret = -EINVAL;
			goto register_fail;
		}
		haptics->actuator_type = val;
	}

	haptics->play_mode = HAP_PLAY_BUFFER;
	ret = of_property_read_u32(node, "qcom,play-mode", &val);
	if (!ret) {
		if (val != HAP_PLAY_BUFFER && val != HAP_PLAY_DIRECT) {
			dev_err(&pdev->dev, "qcom,play-mode (%d) isn't supported\n", val);
			ret = -EINVAL;
			goto register_fail;
		}
		haptics->play_mode = val;
	}

	ret = of_property_read_u32(node, "qcom,wave-play-rate-us", &val);
	if (!ret) {
		haptics->play_wave_rate = val;
	} else if (ret != -EINVAL) {
		dev_err(haptics->dev, "Unable to read play rate ret=%d\n", ret);
		goto register_fail;
	}

	haptics->play_wave_rate =
		clamp_t(u32, haptics->play_wave_rate,
			HAP_WAVE_PLAY_RATE_MIN_US, HAP_WAVE_PLAY_RATE_MAX_US);

	haptics->wave_shape = HAP_WAVE_SINE;
	ret = of_property_read_u32(node, "qcom,wave-shape", &val);
	if (!ret) {
		if (val != HAP_WAVE_SINE && val != HAP_WAVE_SQUARE) {
			dev_err(&pdev->dev, "qcom,wave-shape is invalid: %d\n", val);
			ret = -EINVAL;
			goto register_fail;
		}
		haptics->wave_shape = val;
	}

	haptics->brake_pat[0] = 0x3;
	haptics->brake_pat[1] = 0x3;
	haptics->brake_pat[2] = 0x2;
	haptics->brake_pat[3] = 0x1;

	if (of_property_count_elems_of_size(node, "qcom,brake-pattern", sizeof(u32)) == 4) {
		u32 bp[4];

		ret = of_property_read_u32_array(node, "qcom,brake-pattern", bp, 4);
		for (i = 0; !ret && i < 4; i++) {
			if (bp[i] > 3) {
				ret = -EINVAL;
				break;
			}
			haptics->brake_pat[i] = bp[i];
		}
	} else {
		ret = of_property_read_u8_array(node, "qcom,brake-pattern", haptics->brake_pat, 4);
	}
	if (ret < 0 && ret != -EINVAL) {
		dev_err(&pdev->dev, "qcom,brake-pattern is invalid, ret = %d\n", ret);
		goto register_fail;
	}

	haptics->current_limit = HAP_ILIM_800_MA;	/* stock qcom,ilim-ma = 800 */
	if (!of_property_read_u32(node, "qcom,ilim-ma", &val))
		haptics->current_limit = val >= 800 ? HAP_ILIM_800_MA : HAP_ILIM_400_MA;
	if (ilim_ma)
		haptics->current_limit = ilim_ma >= 800 ? HAP_ILIM_800_MA : HAP_ILIM_400_MA;

	haptics->vmax_cap = A6L_STOCK_VMAX_MV;
	if (!of_property_read_u32(node, "qcom,vmax-mv", &val))
		haptics->vmax_cap = clamp_t(u32, val, HAP_VMAX_MIN_MV, A6L_STOCK_VMAX_MV);
	haptics->vmax = haptics->vmax_cap;
	mutex_init(&haptics->play_lock);

	for (i = 0; i < HAP_WAVE_SAMP_LEN; i++)
		haptics->wave_samp[i] = HAP_WF_SAMP_MAX;

	ret = spmi_haptics_init(haptics);
	if (ret < 0) {
		dev_err(&pdev->dev, "Error initialising haptics, ret=%d\n",
			ret);
		goto register_fail;
	}

	platform_set_drvdata(pdev, haptics);
	a6l_haptics_dump(haptics, "probe");
	haptics->dbg = debugfs_create_dir("a6l_haptics", NULL);
	debugfs_create_file("regs", 0400, haptics->dbg, haptics, &a6l_regs_fops);
	debugfs_create_file("test", 0200, haptics->dbg, haptics, &a6l_test_fops);

	input_dev = devm_input_allocate_device(&pdev->dev);
	if (!input_dev)
		return -ENOMEM;

	INIT_WORK(&haptics->work, spmi_haptics_work);
	haptics->haptics_input_dev = input_dev;

	input_dev->name = "spmi_haptics";
	input_dev->id.version = 1;
	input_dev->close = spmi_haptics_close;
	input_set_drvdata(input_dev, haptics);
	// Figure out how to make this FF_PERIODIC
	input_set_capability(haptics->haptics_input_dev, EV_FF, FF_RUMBLE);
	/* FF_CONSTANT for the QTI VibratorOL AIDL HAL (needs FF_CONSTANT or FF_PERIODIC) */
	input_set_capability(haptics->haptics_input_dev, EV_FF, FF_CONSTANT);

	ret = input_ff_create_memless(input_dev, NULL,
					spmi_haptics_play_effect);
	if (ret) {
		dev_err(&pdev->dev,
			"couldn't register vibrator as FF device\n");
		goto register_fail;
	}

	ret = input_register_device(input_dev);
	if (ret) {
		dev_err(&pdev->dev, "couldn't register input device\n");
		goto register_fail;
	}

	return 0;

register_fail:
	cancel_work_sync(&haptics->work);
	mutex_destroy(&haptics->play_lock);

	return ret;
}

static int __maybe_unused spmi_haptics_suspend(struct device *dev)
{
	struct spmi_haptics *haptics = dev_get_drvdata(dev);

	cancel_work_sync(&haptics->work);
	spmi_haptics_disable(haptics);

	return 0;
}

static SIMPLE_DEV_PM_OPS(spmi_haptics_pm_ops, spmi_haptics_suspend, NULL);

static void spmi_haptics_remove(struct platform_device *pdev)
{
	struct spmi_haptics *haptics = dev_get_drvdata(&pdev->dev);

	debugfs_remove_recursive(haptics->dbg);

	cancel_work_sync(&haptics->work);
	mutex_destroy(&haptics->play_lock);
	input_unregister_device(haptics->haptics_input_dev);
}

static void spmi_haptics_shutdown(struct platform_device *pdev)
{
	struct spmi_haptics *haptics = dev_get_drvdata(&pdev->dev);

	cancel_work_sync(&haptics->work);

	spmi_haptics_disable(haptics);
}

static const struct of_device_id spmi_haptics_match_table[] = {
	{ .compatible = "qcom,spmi-haptics" },
	{ .compatible = "hisense,a6l-pm660-haptics" },
	{ }
};
MODULE_DEVICE_TABLE(of, spmi_haptics_match_table);

static struct platform_driver spmi_haptics_driver = {
	.probe		= spmi_haptics_probe,
	.remove		= spmi_haptics_remove,
	.shutdown	= spmi_haptics_shutdown,
	.driver		= {
		.name	= "a6l-pm660-haptics",
		.pm	= &spmi_haptics_pm_ops,
		.of_match_table = spmi_haptics_match_table,
	},
};
module_platform_driver(spmi_haptics_driver);

MODULE_DESCRIPTION("A6L PM660 LRA haptics (qcom-spmi-haptics derivative)");
MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Caleb Connolly <caleb@connolly.tech>");
