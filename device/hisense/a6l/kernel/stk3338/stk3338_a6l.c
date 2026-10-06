/*
 * stk3338_a6l.c - Hisense A6L FRONT ambient light / proximity sensor (Sensortek STK3338).
 *
 * Derived from mainline drivers/iio/light/stk3310.c (Linux 7.2.3, same register map: STATE 0x00, PSCTRL 0x01,
 * ALSCTRL 0x02, LEDCTRL 0x03, INT 0x04, WAIT 0x05, THD_PS 0x06/0x08, FLAG 0x10, PS 0x11, ALS 0x13, PDT_ID 0x3E,
 * SW_RESET 0x80), with the power-on/probe sequence of the stock Hisense stk3x3x driver v3.11.0 (disassembled from
 * the stock kernel, docs/stk-20260925.md):
 *   - regulator "vdd" (pm660l L3, stock asks 2.80-2.95 V) and "vio" (pm660 L13, 1.75-1.95 V) enabled, 3 ms settle.
 *     Mainline stk3310 has NO regulator handling at all.
 *   - chip search: the stock driver ignores the DT address and tries I2C 0x67 first, then 0x47 (table in
 *     stk_ps_driver+0x308), reading PID/RID at 0x3E; PID must be non-zero.
 *   - software reset (write 0x05, read it back, write 0x80 = 0, sleep 13-15 ms), then stock register presets
 *     (DT stk,*-reg: STATE 0x08, PSCTRL 0x71, ALSCTRL 0x21, LEDCTRL 0x40, INT 0x01, WAIT 0x20, 0x4E = 0x20,
 *     0xFA = 0x01, PS thresholds high 0x78 / low 0x37).
 * a6l stk agent, 25 Sep 2026.
 * F3 (hardware review 28 Sep 2026, fix 29 Sep): proximity is Android's WAKE-UP sensor, so system suspend no longer puts
 * the chip in standby while proximity is enabled (PS state bit set and PS interrupt enabled through the IIO event
 * enable that sensors.a6l writes on activate/deactivate): the device is wakeup-capable (devm_device_init_wakeup), PS
 * keeps sensing (its vdd/vio supplies are never released in suspend), only ALS goes to standby, and the PS interrupt
 * is armed with enable_irq_wake(). An interrupt that arrives while suspended is only marked pending (I2C may still be
 * suspended); resume disarms the wake IRQ, restores ALS and reports the pending near/far state (FLAG_NF) as an IIO
 * event, holding a 500 ms wakeup event so userspace can read it. Proximity disabled: full standby as before.
 */
// SPDX-License-Identifier: GPL-2.0-only
/*
 * Sensortek STK3310/STK3311 Ambient Light and Proximity Sensor
 *
 * Copyright (c) 2015, Intel Corporation.
 *
 * IIO driver for STK3310/STK3311. 7-bit I2C address: 0x48.
 */

#include <linux/array_size.h>
#include <linux/bits.h>
#include <linux/dev_printk.h>
#include <linux/err.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pm.h>
#include <linux/property.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/delay.h>
#include <linux/sprintf.h>
#include <linux/sysfs.h>
#include <linux/types.h>

#include <linux/iio/events.h>
#include <linux/iio/iio.h>
#include <linux/iio/sysfs.h>
#include <linux/iio/types.h>

#include <asm/byteorder.h>

#define STK3310_REG_STATE			0x00
#define STK3310_REG_PSCTRL			0x01
#define STK3310_REG_ALSCTRL			0x02
#define STK3310_REG_INT				0x04
#define STK3310_REG_THDH_PS			0x06
#define STK3310_REG_THDL_PS			0x08
#define STK3310_REG_FLAG			0x10
#define STK3310_REG_PS_DATA_MSB			0x11
#define STK3310_REG_PS_DATA_LSB			0x12
#define STK3310_REG_ALS_DATA_MSB		0x13
#define STK3310_REG_ALS_DATA_LSB		0x14
#define STK3310_REG_ID				0x3E
#define STK3310_MAX_REG				0x80

#define STK3310_STATE_EN_PS			BIT(0)
#define STK3310_STATE_EN_ALS			BIT(1)
#define STK3310_STATE_STANDBY			0x00

#define STK3013_CHIP_ID_VAL			0x31
#define STK3310_CHIP_ID_VAL			0x13
#define STK3311_CHIP_ID_VAL			0x1D
#define STK3311A_CHIP_ID_VAL			0x15
#define STK3311S34_CHIP_ID_VAL			0x1E
#define STK3311X_CHIP_ID_VAL			0x12
#define STK3335_CHIP_ID_VAL			0x51
/* STK3338 PID: not in mainline; stock only requires PID != 0. Logged at probe. */
#define STK3310_PSINT_EN			0x01
#define STK3310_PS_MAX_VAL			0xFFFF

#define STK3310_DRIVER_NAME			"stk3338"

#define STK3310_SCALE_AVAILABLE			"6.4 1.6 0.4 0.1"

#define STK3310_IT_AVAILABLE \
	"0.000185 0.000370 0.000741 0.001480 0.002960 0.005920 0.011840 " \
	"0.023680 0.047360 0.094720 0.189440 0.378880 0.757760 1.515520 " \
	"3.031040 6.062080"

#define STK3310_REGFIELD(name)						    \
	do {								    \
		data->reg_##name =					    \
			devm_regmap_field_alloc(&client->dev, regmap,	    \
				stk3310_reg_field_##name);		    \
		if (IS_ERR(data->reg_##name)) {				    \
			dev_err(&client->dev, "reg field alloc failed.\n"); \
			return PTR_ERR(data->reg_##name);		    \
		}							    \
	} while (0)

static const struct reg_field stk3310_reg_field_state =
				REG_FIELD(STK3310_REG_STATE, 0, 2);
static const struct reg_field stk3310_reg_field_als_gain =
				REG_FIELD(STK3310_REG_ALSCTRL, 4, 5);
static const struct reg_field stk3310_reg_field_ps_gain =
				REG_FIELD(STK3310_REG_PSCTRL, 4, 5);
static const struct reg_field stk3310_reg_field_als_it =
				REG_FIELD(STK3310_REG_ALSCTRL, 0, 3);
static const struct reg_field stk3310_reg_field_ps_it =
				REG_FIELD(STK3310_REG_PSCTRL, 0, 3);
static const struct reg_field stk3310_reg_field_int_ps =
				REG_FIELD(STK3310_REG_INT, 0, 2);
static const struct reg_field stk3310_reg_field_flag_psint =
				REG_FIELD(STK3310_REG_FLAG, 4, 4);
static const struct reg_field stk3310_reg_field_flag_nf =
				REG_FIELD(STK3310_REG_FLAG, 0, 0);

static unsigned short addr_fallback = 1;
module_param(addr_fallback, ushort, 0444);
MODULE_PARM_DESC(addr_fallback, "1 = like stock, try 0x67 then 0x47 if the DT address NAKs");
static int ps_thd_high = 0x78, ps_thd_low = 0x37;
module_param(ps_thd_high, int, 0444);
module_param(ps_thd_low, int, 0444);
static int reg_psctrl = 0x71, reg_alsctrl = 0x21, reg_ledctrl = 0x40, reg_wait = 0x20, reg_state = 0x08;
module_param(reg_psctrl, int, 0444);
module_param(reg_alsctrl, int, 0444);
module_param(reg_ledctrl, int, 0444);
module_param(reg_wait, int, 0444);
module_param(reg_state, int, 0444);

static const u8 stk3310_chip_ids[] = {
	STK3013_CHIP_ID_VAL,
	STK3310_CHIP_ID_VAL,
	STK3311A_CHIP_ID_VAL,
	STK3311S34_CHIP_ID_VAL,
	STK3311X_CHIP_ID_VAL,
	STK3311_CHIP_ID_VAL,
	STK3335_CHIP_ID_VAL,
};

/* Estimate maximum proximity values with regard to measurement scale. */
static const int stk3310_ps_max[4] = {
	STK3310_PS_MAX_VAL / 640,
	STK3310_PS_MAX_VAL / 160,
	STK3310_PS_MAX_VAL /  40,
	STK3310_PS_MAX_VAL /  10
};

static const int stk3310_scale_table[][2] = {
	{6, 400000}, {1, 600000}, {0, 400000}, {0, 100000}
};

/* Integration time in seconds, microseconds */
static const int stk3310_it_table[][2] = {
	{0, 185},	{0, 370},	{0, 741},	{0, 1480},
	{0, 2960},	{0, 5920},	{0, 11840},	{0, 23680},
	{0, 47360},	{0, 94720},	{0, 189440},	{0, 378880},
	{0, 757760},	{1, 515520},	{3, 31040},	{6, 62080},
};

struct stk3310_data {
	struct i2c_client *client;
	struct mutex lock;
	u64 timestamp;
	struct regmap *regmap;
	struct regmap_field *reg_state;
	struct regmap_field *reg_als_gain;
	struct regmap_field *reg_ps_gain;
	struct regmap_field *reg_als_it;
	struct regmap_field *reg_ps_it;
	struct regmap_field *reg_int_ps;
	struct regmap_field *reg_flag_psint;
	struct regmap_field *reg_flag_nf;
	u32 ps_thdl;
	u32 ps_thdh;
	u32 ps_near_level;
	bool als_enabled;
	bool ps_enabled;
	bool ps_int_enabled;
	bool suspended;		/* F3: between suspend and resume; the IRQ thread must not touch I2C */
	bool wake_armed;	/* F3: enable_irq_wake() done in suspend */
	bool irq_pending;	/* F3: PS interrupt seen while suspended, reported by resume */
};

static const struct iio_event_spec stk3310_events[] = {
	/* Proximity event */
	{
		.type = IIO_EV_TYPE_THRESH,
		.dir = IIO_EV_DIR_RISING,
		.mask_separate = BIT(IIO_EV_INFO_VALUE) |
				 BIT(IIO_EV_INFO_ENABLE),
	},
	/* Out-of-proximity event */
	{
		.type = IIO_EV_TYPE_THRESH,
		.dir = IIO_EV_DIR_FALLING,
		.mask_separate = BIT(IIO_EV_INFO_VALUE) |
				 BIT(IIO_EV_INFO_ENABLE),
	},
};

static ssize_t stk3310_read_near_level(struct iio_dev *indio_dev,
				       uintptr_t priv,
				       const struct iio_chan_spec *chan,
				       char *buf)
{
	struct stk3310_data *data = iio_priv(indio_dev);

	return sprintf(buf, "%u\n", data->ps_near_level);
}

static const struct iio_chan_spec_ext_info stk3310_ext_info[] = {
	{
		.name = "nearlevel",
		.shared = IIO_SEPARATE,
		.read = stk3310_read_near_level,
	},
	{ }
};

static const struct iio_chan_spec stk3310_channels[] = {
	{
		.type = IIO_LIGHT,
		.info_mask_separate =
			BIT(IIO_CHAN_INFO_RAW) |
			BIT(IIO_CHAN_INFO_SCALE) |
			BIT(IIO_CHAN_INFO_INT_TIME),
	},
	{
		.type = IIO_PROXIMITY,
		.info_mask_separate =
			BIT(IIO_CHAN_INFO_RAW) |
			BIT(IIO_CHAN_INFO_SCALE) |
			BIT(IIO_CHAN_INFO_INT_TIME),
		.event_spec = stk3310_events,
		.num_event_specs = ARRAY_SIZE(stk3310_events),
		.ext_info = stk3310_ext_info,
	}
};

static IIO_CONST_ATTR(in_illuminance_scale_available, STK3310_SCALE_AVAILABLE);

static IIO_CONST_ATTR(in_proximity_scale_available, STK3310_SCALE_AVAILABLE);

static IIO_CONST_ATTR(in_illuminance_integration_time_available,
		      STK3310_IT_AVAILABLE);

static IIO_CONST_ATTR(in_proximity_integration_time_available,
		      STK3310_IT_AVAILABLE);

static struct attribute *stk3310_attributes[] = {
	&iio_const_attr_in_illuminance_scale_available.dev_attr.attr,
	&iio_const_attr_in_proximity_scale_available.dev_attr.attr,
	&iio_const_attr_in_illuminance_integration_time_available.dev_attr.attr,
	&iio_const_attr_in_proximity_integration_time_available.dev_attr.attr,
	NULL,
};

static const struct attribute_group stk3310_attribute_group = {
	.attrs = stk3310_attributes
};

static int stk3310_check_chip_id(const u8 chip_id)
{
	for (int i = 0; i < ARRAY_SIZE(stk3310_chip_ids); i++) {
		if (chip_id == stk3310_chip_ids[i])
			return 0;
	}

	return -ENODEV;
}

static int stk3310_get_index(const int table[][2], int table_size,
			     int val, int val2)
{
	int i;

	for (i = 0; i < table_size; i++) {
		if (val == table[i][0] && val2 == table[i][1])
			return i;
	}

	return -EINVAL;
}

static int stk3310_read_event(struct iio_dev *indio_dev,
			      const struct iio_chan_spec *chan,
			      enum iio_event_type type,
			      enum iio_event_direction dir,
			      enum iio_event_info info,
			      int *val, int *val2)
{
	u8 reg;
	__be16 buf;
	int ret;
	struct stk3310_data *data = iio_priv(indio_dev);

	if (info != IIO_EV_INFO_VALUE)
		return -EINVAL;

	/* Only proximity interrupts are implemented at the moment. */
	if (dir == IIO_EV_DIR_RISING)
		reg = STK3310_REG_THDH_PS;
	else if (dir == IIO_EV_DIR_FALLING)
		reg = STK3310_REG_THDL_PS;
	else
		return -EINVAL;

	mutex_lock(&data->lock);
	ret = regmap_bulk_read(data->regmap, reg, &buf, sizeof(buf));
	mutex_unlock(&data->lock);
	if (ret < 0) {
		dev_err(&data->client->dev, "register read failed\n");
		return ret;
	}
	*val = be16_to_cpu(buf);

	return IIO_VAL_INT;
}

static int stk3310_write_event(struct iio_dev *indio_dev,
			       const struct iio_chan_spec *chan,
			       enum iio_event_type type,
			       enum iio_event_direction dir,
			       enum iio_event_info info,
			       int val, int val2)
{
	u8 reg;
	__be16 buf;
	int ret;
	unsigned int index;
	struct stk3310_data *data = iio_priv(indio_dev);
	struct i2c_client *client = data->client;

	ret = regmap_field_read(data->reg_ps_gain, &index);
	if (ret < 0)
		return ret;

	if (val < 0 || val > stk3310_ps_max[index])
		return -EINVAL;

	if (dir == IIO_EV_DIR_RISING)
		reg = STK3310_REG_THDH_PS;
	else if (dir == IIO_EV_DIR_FALLING)
		reg = STK3310_REG_THDL_PS;
	else
		return -EINVAL;

	buf = cpu_to_be16(val);
	ret = regmap_bulk_write(data->regmap, reg, &buf, sizeof(buf));
	if (ret < 0) {
		dev_err(&client->dev, "failed to set PS threshold!\n");
		return ret;
	}

	if (reg == STK3310_REG_THDH_PS)
		data->ps_thdh = val;
	else
		data->ps_thdl = val;

	return 0;
}

static int stk3310_read_event_config(struct iio_dev *indio_dev,
				     const struct iio_chan_spec *chan,
				     enum iio_event_type type,
				     enum iio_event_direction dir)
{
	unsigned int event_val;
	int ret;
	struct stk3310_data *data = iio_priv(indio_dev);

	ret = regmap_field_read(data->reg_int_ps, &event_val);
	if (ret < 0)
		return ret;

	return event_val;
}

static int stk3310_write_event_config(struct iio_dev *indio_dev,
				      const struct iio_chan_spec *chan,
				      enum iio_event_type type,
				      enum iio_event_direction dir,
				      bool state)
{
	int ret;
	struct stk3310_data *data = iio_priv(indio_dev);
	struct i2c_client *client = data->client;

	/* Set INT_PS value */
	mutex_lock(&data->lock);
	ret = regmap_field_write(data->reg_int_ps, state);
	if (ret < 0) {
		dev_err(&client->dev, "failed to set interrupt mode\n");
		mutex_unlock(&data->lock);
		return ret;
	}

	data->ps_int_enabled = state;

	mutex_unlock(&data->lock);

	return 0;
}

static int stk3310_read_raw(struct iio_dev *indio_dev,
			    struct iio_chan_spec const *chan,
			    int *val, int *val2, long mask)
{
	u8 reg;
	__be16 buf;
	int ret;
	unsigned int index;
	struct stk3310_data *data = iio_priv(indio_dev);
	struct i2c_client *client = data->client;

	if (chan->type != IIO_LIGHT && chan->type != IIO_PROXIMITY)
		return -EINVAL;

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		if (chan->type == IIO_LIGHT)
			reg = STK3310_REG_ALS_DATA_MSB;
		else
			reg = STK3310_REG_PS_DATA_MSB;

		mutex_lock(&data->lock);
		ret = regmap_bulk_read(data->regmap, reg, &buf, sizeof(buf));
		if (ret < 0) {
			dev_err(&client->dev, "register read failed\n");
			mutex_unlock(&data->lock);
			return ret;
		}
		*val = be16_to_cpu(buf);
		mutex_unlock(&data->lock);
		return IIO_VAL_INT;
	case IIO_CHAN_INFO_INT_TIME:
		if (chan->type == IIO_LIGHT)
			ret = regmap_field_read(data->reg_als_it, &index);
		else
			ret = regmap_field_read(data->reg_ps_it, &index);
		if (ret < 0)
			return ret;

		*val = stk3310_it_table[index][0];
		*val2 = stk3310_it_table[index][1];
		return IIO_VAL_INT_PLUS_MICRO;
	case IIO_CHAN_INFO_SCALE:
		if (chan->type == IIO_LIGHT)
			ret = regmap_field_read(data->reg_als_gain, &index);
		else
			ret = regmap_field_read(data->reg_ps_gain, &index);
		if (ret < 0)
			return ret;

		*val = stk3310_scale_table[index][0];
		*val2 = stk3310_scale_table[index][1];
		return IIO_VAL_INT_PLUS_MICRO;
	}

	return -EINVAL;
}

static int stk3310_write_raw(struct iio_dev *indio_dev,
			     struct iio_chan_spec const *chan,
			     int val, int val2, long mask)
{
	int ret;
	int index;
	struct stk3310_data *data = iio_priv(indio_dev);

	if (chan->type != IIO_LIGHT && chan->type != IIO_PROXIMITY)
		return -EINVAL;

	switch (mask) {
	case IIO_CHAN_INFO_INT_TIME:
		index = stk3310_get_index(stk3310_it_table,
					  ARRAY_SIZE(stk3310_it_table),
					  val, val2);
		if (index < 0)
			return -EINVAL;
		mutex_lock(&data->lock);
		if (chan->type == IIO_LIGHT)
			ret = regmap_field_write(data->reg_als_it, index);
		else
			ret = regmap_field_write(data->reg_ps_it, index);
		if (ret < 0)
			dev_err(&data->client->dev,
				"sensor configuration failed\n");
		mutex_unlock(&data->lock);
		return ret;

	case IIO_CHAN_INFO_SCALE:
		index = stk3310_get_index(stk3310_scale_table,
					  ARRAY_SIZE(stk3310_scale_table),
					  val, val2);
		if (index < 0)
			return -EINVAL;
		mutex_lock(&data->lock);
		if (chan->type == IIO_LIGHT)
			ret = regmap_field_write(data->reg_als_gain, index);
		else
			ret = regmap_field_write(data->reg_ps_gain, index);
		if (ret < 0)
			dev_err(&data->client->dev,
				"sensor configuration failed\n");
		mutex_unlock(&data->lock);
		return ret;
	}

	return -EINVAL;
}

static const struct iio_info stk3310_info = {
	.read_raw		= stk3310_read_raw,
	.write_raw		= stk3310_write_raw,
	.attrs			= &stk3310_attribute_group,
	.read_event_value	= stk3310_read_event,
	.write_event_value	= stk3310_write_event,
	.read_event_config	= stk3310_read_event_config,
	.write_event_config	= stk3310_write_event_config,
};

static int stk3310_set_state(struct stk3310_data *data, u8 state)
{
	int ret;
	struct i2c_client *client = data->client;

	/* 3-bit state; 0b100 is not supported. */
	if (state > 7 || state == 4)
		return -EINVAL;

	mutex_lock(&data->lock);
	ret = regmap_field_write(data->reg_state, state);
	if (ret < 0) {
		dev_err(&client->dev, "failed to change sensor state\n");
	} else if (state != STK3310_STATE_STANDBY) {
		/* Don't reset the 'enabled' flags if we're going in standby */
		data->ps_enabled  = !!(state & STK3310_STATE_EN_PS);
		data->als_enabled = !!(state & STK3310_STATE_EN_ALS);
	}
	mutex_unlock(&data->lock);

	return ret;
}

static int stk3310_init(struct iio_dev *indio_dev)
{
	int ret;
	int chipid;
	u8 state;
	struct stk3310_data *data = iio_priv(indio_dev);
	struct i2c_client *client = data->client;

	ret = regmap_read(data->regmap, STK3310_REG_ID, &chipid);
	if (ret < 0)
		return ret;

	ret = stk3310_check_chip_id(chipid);
	if (ret < 0)
		dev_info(&client->dev, "new unknown chip id: 0x%x\n", chipid);

	state = STK3310_STATE_EN_ALS | STK3310_STATE_EN_PS;
	ret = stk3310_set_state(data, state);
	if (ret < 0) {
		dev_err(&client->dev, "failed to enable sensor");
		return ret;
	}

	/* Enable PS interrupts */
	ret = regmap_field_write(data->reg_int_ps, STK3310_PSINT_EN);
	if (ret < 0) {
		dev_err(&client->dev, "failed to enable interrupts!\n");
		return ret;
	}

	data->ps_int_enabled = true;
	data->ps_thdh = ps_thd_high;	/* A6L: stock thresholds written by stk3338_a6l_prepare() */
	data->ps_thdl = ps_thd_low;

	return 0;
}

static bool stk3310_is_volatile_reg(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case STK3310_REG_ALS_DATA_MSB:
	case STK3310_REG_ALS_DATA_LSB:
	case STK3310_REG_PS_DATA_LSB:
	case STK3310_REG_PS_DATA_MSB:
	case STK3310_REG_FLAG:
		return true;
	default:
		return false;
	}
}

static const struct regmap_config stk3310_regmap_config = {
	.name = "stk3310_regmap",
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = STK3310_MAX_REG,
	.cache_type = REGCACHE_RBTREE,
	.volatile_reg = stk3310_is_volatile_reg,
};

static int stk3310_regmap_init(struct stk3310_data *data)
{
	struct regmap *regmap;
	struct i2c_client *client;

	client = data->client;
	regmap = devm_regmap_init_i2c(client, &stk3310_regmap_config);
	if (IS_ERR(regmap)) {
		dev_err(&client->dev, "regmap initialization failed.\n");
		return PTR_ERR(regmap);
	}
	data->regmap = regmap;

	STK3310_REGFIELD(state);
	STK3310_REGFIELD(als_gain);
	STK3310_REGFIELD(ps_gain);
	STK3310_REGFIELD(als_it);
	STK3310_REGFIELD(ps_it);
	STK3310_REGFIELD(int_ps);
	STK3310_REGFIELD(flag_psint);
	STK3310_REGFIELD(flag_nf);

	return 0;
}

static irqreturn_t stk3310_irq_handler(int irq, void *private)
{
	struct iio_dev *indio_dev = private;
	struct stk3310_data *data = iio_priv(indio_dev);

	data->timestamp = iio_get_time_ns(indio_dev);

	return IRQ_WAKE_THREAD;
}

static irqreturn_t stk3310_irq_event_handler(int irq, void *private)
{
	int ret;
	unsigned int dir;
	u64 event;

	struct iio_dev *indio_dev = private;
	struct stk3310_data *data = iio_priv(indio_dev);

	/* Read FLAG_NF to figure out what threshold has been met. */
	mutex_lock(&data->lock);
	if (data->suspended) {
		/* F3: wake IRQ replayed before our resume callback: report from resume (INT stays asserted until then) */
		data->irq_pending = true;
		pm_wakeup_event(&data->client->dev, 500);
		goto out;
	}
	ret = regmap_field_read(data->reg_flag_nf, &dir);
	if (ret < 0) {
		dev_err(&data->client->dev, "register read failed: %d\n", ret);
		goto out;
	}
	event = IIO_UNMOD_EVENT_CODE(IIO_PROXIMITY, 1,
				     IIO_EV_TYPE_THRESH,
				     (dir ? IIO_EV_DIR_FALLING :
					    IIO_EV_DIR_RISING));
	iio_push_event(indio_dev, event, data->timestamp);

	/* Reset the interrupt flag */
	ret = regmap_field_write(data->reg_flag_psint, 0);
	if (ret < 0)
		dev_err(&data->client->dev, "failed to reset interrupts\n");
out:
	mutex_unlock(&data->lock);

	return IRQ_HANDLED;
}

static const char * const stk3338_supplies[] = { "vdd", "vio" };

/* Power on, find the chip (stock address table), soft reset, stock presets. Raw SMBus: runs before regmap. */
static int stk3338_a6l_prepare(struct i2c_client *client)
{
	static const unsigned short stock_addrs[] = { 0x67, 0x47 };
	unsigned short dt_addr = client->addr;
	int ret, pid, rid, i;

	ret = devm_regulator_bulk_get_enable(&client->dev, ARRAY_SIZE(stk3338_supplies), stk3338_supplies);
	if (ret)
		return dev_err_probe(&client->dev, ret, "A6L_STK supplies\n");
	msleep(5);	/* stock: msleep(3) */

	pid = i2c_smbus_read_byte_data(client, STK3310_REG_ID);
	dev_info(&client->dev, "A6L_STK addr 0x%02x PID read %d\n", client->addr, pid);
	for (i = 0; pid < 0 && addr_fallback && i < ARRAY_SIZE(stock_addrs); i++) {
		if (stock_addrs[i] == dt_addr)
			continue;
		client->addr = stock_addrs[i];
		pid = i2c_smbus_read_byte_data(client, STK3310_REG_ID);
		dev_info(&client->dev, "A6L_STK addr 0x%02x PID read %d\n", client->addr, pid);
	}
	if (pid <= 0) {
		client->addr = dt_addr;
		dev_err(&client->dev, "A6L_STK no STK3338 answered (0x%02x/0x67/0x47): %d\n", dt_addr, pid);
		return pid < 0 ? pid : -ENODEV;
	}
	rid = i2c_smbus_read_byte_data(client, STK3310_REG_ID + 1);
	dev_info(&client->dev, "A6L_STK found at 0x%02x (DT 0x%02x): PID=0x%02x RID=0x%02x\n",
		 client->addr, dt_addr, pid, rid);

	/* stock stk3x3x_software_reset */
	ret = i2c_smbus_write_byte_data(client, 0x05, 0x7f);
	if (!ret && i2c_smbus_read_byte_data(client, 0x05) != 0x7f)
		ret = -EIO;
	if (!ret)
		ret = i2c_smbus_write_byte_data(client, 0x80, 0x00);
	if (ret) {
		dev_err(&client->dev, "A6L_STK software reset failed %d\n", ret);
		return ret;
	}
	usleep_range(13000, 15000);

	/* stock stk3x3x_init_all_reg order; PS/ALS enable bits are set later by stk3310_init() */
	ret = i2c_smbus_write_byte_data(client, STK3310_REG_STATE, reg_state & ~0x07);
	ret = ret ?: i2c_smbus_write_byte_data(client, STK3310_REG_PSCTRL, reg_psctrl);
	ret = ret ?: i2c_smbus_write_byte_data(client, STK3310_REG_ALSCTRL, reg_alsctrl);
	ret = ret ?: i2c_smbus_write_byte_data(client, 0x03, reg_ledctrl);
	ret = ret ?: i2c_smbus_write_byte_data(client, 0x05, reg_wait);
	ret = ret ?: i2c_smbus_write_byte_data(client, STK3310_REG_INT, 0x01);
	ret = ret ?: i2c_smbus_write_byte_data(client, 0x4e, 0x20);
	ret = ret ?: i2c_smbus_write_byte_data(client, 0xfa, 0x01);
	ret = ret ?: i2c_smbus_write_word_swapped(client, STK3310_REG_THDH_PS, ps_thd_high);
	ret = ret ?: i2c_smbus_write_word_swapped(client, STK3310_REG_THDL_PS, ps_thd_low);
	if (ret)
		dev_err(&client->dev, "A6L_STK preset write failed %d\n", ret);
	return ret;
}

static int stk3310_probe(struct i2c_client *client)
{
	int ret;
	struct iio_dev *indio_dev;
	struct stk3310_data *data;

	indio_dev = devm_iio_device_alloc(&client->dev, sizeof(*data));
	if (!indio_dev)
		return -ENOMEM;

	data = iio_priv(indio_dev);
	data->client = client;
	i2c_set_clientdata(client, indio_dev);

	device_property_read_u32(&client->dev, "proximity-near-level",
				 &data->ps_near_level);

	mutex_init(&data->lock);

	ret = stk3338_a6l_prepare(client);
	if (ret < 0)
		return ret;

	ret = stk3310_regmap_init(data);
	if (ret < 0)
		return ret;

	indio_dev->info = &stk3310_info;
	indio_dev->name = STK3310_DRIVER_NAME;
	indio_dev->modes = INDIO_DIRECT_MODE;
	indio_dev->channels = stk3310_channels;
	indio_dev->num_channels = ARRAY_SIZE(stk3310_channels);

	ret = stk3310_init(indio_dev);
	if (ret < 0)
		return ret;

	if (client->irq > 0) {
		ret = devm_request_threaded_irq(&client->dev, client->irq,
						stk3310_irq_handler,
						stk3310_irq_event_handler,
						IRQF_TRIGGER_FALLING |
						IRQF_ONESHOT,
						"stk3310_event", indio_dev);
		if (ret < 0) {
			dev_err(&client->dev, "request irq %d failed\n",
				client->irq);
			goto err_standby;
		}
		/* F3: wake-up proximity; the IRQ is armed for wake only while proximity is enabled (suspend) */
		ret = devm_device_init_wakeup(&client->dev);
		if (ret)
			dev_warn(&client->dev, "A6L_STK wakeup init failed %d: proximity will not wake the system\n", ret);
	}

	ret = iio_device_register(indio_dev);
	if (ret < 0) {
		dev_err(&client->dev, "device_register failed\n");
		goto err_standby;
	}

	return 0;

err_standby:
	stk3310_set_state(data, STK3310_STATE_STANDBY);
	return ret;
}

static void stk3310_remove(struct i2c_client *client)
{
	struct iio_dev *indio_dev = i2c_get_clientdata(client);

	iio_device_unregister(indio_dev);
	stk3310_set_state(iio_priv(indio_dev), STK3310_STATE_STANDBY);
}

/* F3: push the current near/far state if the PS interrupt flag is set (or unconditionally with force) */
static void stk3338_a6l_report_ps(struct iio_dev *indio_dev, bool force)
{
	struct stk3310_data *data = iio_priv(indio_dev);
	unsigned int psint = 0, nf = 0;
	int ret;

	mutex_lock(&data->lock);
	ret = regmap_field_read(data->reg_flag_psint, &psint);
	if (ret >= 0 && (psint || force))
		ret = regmap_field_read(data->reg_flag_nf, &nf);
	if (ret < 0) {
		dev_err(&data->client->dev, "A6L_STK resume: flag read failed %d\n", ret);
	} else if (psint || force) {
		iio_push_event(indio_dev,
			       IIO_UNMOD_EVENT_CODE(IIO_PROXIMITY, 1, IIO_EV_TYPE_THRESH,
						    (nf ? IIO_EV_DIR_FALLING : IIO_EV_DIR_RISING)),
			       iio_get_time_ns(indio_dev));
		if (psint && regmap_field_write(data->reg_flag_psint, 0) < 0)
			dev_err(&data->client->dev, "failed to reset interrupts\n");
		pm_wakeup_event(&data->client->dev, 500);	/* let the sensors HAL read it before re-suspend */
		dev_dbg(&data->client->dev, "A6L_STK resume: reported %s\n", nf ? "far" : "near");
	}
	mutex_unlock(&data->lock);
}

static int stk3310_suspend(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct stk3310_data *data;
	int ret;

	data = iio_priv(i2c_get_clientdata(client));

	if (data->ps_enabled && data->ps_int_enabled && client->irq > 0 && device_may_wakeup(dev)) {
		/* F3: wake-up proximity: keep PS sensing (supplies stay on), ALS to standby, arm the PS IRQ for wake */
		mutex_lock(&data->lock);
		ret = regmap_field_write(data->reg_state, STK3310_STATE_EN_PS);	/* flags kept for resume */
		if (!ret)
			data->suspended = true;
		mutex_unlock(&data->lock);
		if (ret < 0) {
			dev_err(dev, "failed to put ALS in standby at suspend.\n");
			return ret;
		}
		ret = enable_irq_wake(client->irq);
		if (!ret) {
			data->wake_armed = true;
			return 0;
		}
		dev_err(dev, "A6L_STK enable_irq_wake(%d) failed %d: full standby\n", client->irq, ret);
	}

	if (data->ps_int_enabled) {
		ret = regmap_field_write(data->reg_int_ps, 0x0);
		if (ret < 0) {
			dev_err(dev, "failed to disable ps int at suspend.\n");
			return ret;
		}
	}

	ret = stk3310_set_state(data, STK3310_STATE_STANDBY);
	if (!ret) {
		mutex_lock(&data->lock);
		data->suspended = true;
		mutex_unlock(&data->lock);
	}
	return ret;
}

static int stk3310_resume(struct device *dev)
{
	u8 state = 0;
	struct i2c_client *client = to_i2c_client(dev);
	struct iio_dev *indio_dev = i2c_get_clientdata(client);
	struct stk3310_data *data;
	bool armed, pending;
	__be16 buf;
	int ret;

	data = iio_priv(indio_dev);

	mutex_lock(&data->lock);
	armed = data->wake_armed;
	pending = data->irq_pending;
	data->wake_armed = false;
	data->irq_pending = false;
	data->suspended = false;
	mutex_unlock(&data->lock);
	if (armed) {
		disable_irq_wake(client->irq);
		/* F3: near/far change during suspend: report it (flag set = interrupt not yet handled) */
		stk3338_a6l_report_ps(indio_dev, pending);
	}

	if (data->ps_enabled)
		state |= STK3310_STATE_EN_PS;
	if (data->als_enabled)
		state |= STK3310_STATE_EN_ALS;

	ret = stk3310_set_state(data, state);
	if (ret < 0)
		return ret;

	if (data->ps_thdl != 0x0) {
		buf = cpu_to_be16(data->ps_thdl);
		ret = regmap_bulk_write(data->regmap, STK3310_REG_THDL_PS, &buf, sizeof(buf));
		if (ret < 0) {
			dev_err(dev, "failed to set reg THDL_PS at resume.\n");
			return ret;
		}
	}

	if (data->ps_thdh != STK3310_PS_MAX_VAL) {
		buf = cpu_to_be16(data->ps_thdh);
		ret = regmap_bulk_write(data->regmap, STK3310_REG_THDH_PS, &buf, sizeof(buf));
		if (ret < 0) {
			dev_err(dev, "failed to set reg THDH_PS at resume.\n");
			return ret;
		}
	}

	if (data->ps_int_enabled && !armed) {
		ret = regmap_field_write(data->reg_int_ps, STK3310_PSINT_EN);
		if (ret < 0) {
			dev_err(dev, "failed to enable ps int at resume.\n");
			return ret;
		}
	}

	return 0;
}

static DEFINE_SIMPLE_DEV_PM_OPS(stk3310_pm_ops, stk3310_suspend,
				stk3310_resume);

static const struct i2c_device_id stk3310_i2c_id[] = {
	{ .name = "stk3338" },
	{ }
};
MODULE_DEVICE_TABLE(i2c, stk3310_i2c_id);



static const struct of_device_id stk3310_of_match[] = {
	{ .compatible = "sensortek,stk3338", },
	/* V74 recovery DT candidate S1 described the part as stk3335 at 0x47 (no STK3335 on the A6L) */
	{ .compatible = "sensortek,stk3335", },
	{ }
};
MODULE_DEVICE_TABLE(of, stk3310_of_match);

static struct i2c_driver stk3310_driver = {
	.driver = {
		.name = "stk3338_a6l",
		.of_match_table = stk3310_of_match,
		.pm = pm_sleep_ptr(&stk3310_pm_ops),
	},
	.probe =        stk3310_probe,
	.remove =           stk3310_remove,
	.id_table =         stk3310_i2c_id,
};

module_i2c_driver(stk3310_driver);

MODULE_AUTHOR("Tiberiu Breana <tiberiu.a.breana@intel.com>");
MODULE_DESCRIPTION("STK3338 (Hisense A6L front) light/proximity driver, based on stk3310");
MODULE_LICENSE("GPL v2");
