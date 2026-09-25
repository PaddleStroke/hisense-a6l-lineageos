// SPDX-License-Identifier: GPL-2.0-only
/*
 * Minimal IIO driver for the ams TMD3702VC ALS/colour + proximity module (Hisense A6L, I2C 0x49 on c176000).
 * Register map from the ams TMD3702VC datasheet (direct 8-bit register addressing, 0x80..0xF2).
 * Polled only (no IRQ): each sysfs read returns the latest completed conversion.
 * Channels: in_intensity_{clear,red,green,blue}_raw, in_proximity_raw, in_illuminance_input (uncalibrated estimate).
 *
 * v2 (23 Sep 2026 evening, "als2"): v1 gave working ALS but a flat proximity (~90 covered or not).
 * The proximity set-up now follows what the STOCK Hisense driver does (disassembled from stock-symbolized.elf,
 * tmd3702_offset_calibration / tmd3702_ps_set_enable / tmd3702_read_ps / tmd3702_als_init_paras):
 *   - CFG6 (0xAE) = ams,ps_apc = 0x7F  -> APC DISABLED (bit 6 = 1); v1 left the reset value 0x3F (APC enabled)
 *   - PCFG1 (0x8F) = ps_gain << 6 | ps_drive = 0x09 (stock drive code 9; v1 used 4)
 *   - PCFG0 (0x8E) = ps_pulse_len << 6 | ps_pulse_cnt = 0x9D, PRATE 0x32, WTIME 0x08, PERS 0x21, CFG1 = 0x20 | als_gain
 *   - stock NEVER writes CFG4 (0xAC) or TEST3 (0xF2); v1 wrote the datasheet "must" values 0x3D / 0xC4.
 *     Default now = stock (not written); ds_init=1 restores the v1 behaviour for A/B testing.
 *   - stock reads PDATA as ONE byte (0x9C) and subtracts a software crosstalk value measured at every enable
 *     (PEN only, wait for PINT, read PDATA; crosstalk accepted only if < ams,ps_crosstalk_max = 255).
 *     Near/far thresholds = crosstalk + 58 / + 30 (ams,ps_th_max / ps_th_min).
 * in_proximity_raw returns the full PDATA word (10 bit with APC off, 14 bit with APC on); prox_crosstalk,
 * prox_near and regs are extra sysfs attributes for the attended test. No separate emitter supply exists in the
 * stock DT (LEDA/VCSEL anode is not switched by the SoC: only vdd = pm660 L13 and vio = gpio64 fixed regulator).
 */
#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/mod_devicetable.h>
#include <linux/mutex.h>
#include <linux/regulator/consumer.h>
#include <linux/iio/iio.h>
#include <linux/iio/sysfs.h>

#define TMD_ENABLE	0x80
#define  TMD_EN_PON	BIT(0)
#define  TMD_EN_AEN	BIT(1)
#define  TMD_EN_PEN	BIT(2)
#define  TMD_EN_WEN	BIT(3)
#define TMD_ATIME	0x81
#define TMD_PRATE	0x82
#define TMD_WTIME	0x83
#define TMD_PILTL	0x88
#define TMD_PIHTL	0x8a
#define TMD_PERS	0x8c
#define TMD_CFG0	0x8d
#define TMD_PCFG0	0x8e
#define TMD_PCFG1	0x8f
#define TMD_CFG1	0x90
#define  TMD_CFG1_RSVD5	BIT(5)	/* datasheet: must be 1, else VCSEL current is 2x nominal (stock also sets it) */
#define TMD_REVID	0x91
#define TMD_ID		0x92
#define TMD_STATUS	0x93
#define  TMD_ST_PSAT	BIT(6)
#define  TMD_ST_PINT	BIT(5)
#define TMD_CDATAL	0x94	/* C,R,G,B: 4 x le16 at 0x94..0x9b */
#define TMD_PDATAL	0x9c
#define TMD_REVID2	0x9e
#define TMD_CFG3	0xab
#define TMD_CFG4	0xac
#define TMD_CFG6	0xae
#define  TMD_CFG6_APC_DIS BIT(6)
#define TMD_POFFSETL	0xc0
#define TMD_POFFSETH	0xc1
#define TMD_CALIB	0xd7
#define TMD_CALIBCFG	0xd9
#define TMD_CALIBSTAT	0xdc
#define TMD_INTENAB	0xdd
#define TMD_TEST3	0xf2
#define TMD_ID_VAL	0x10	/* bits 7:2 = 0b000100 */

static unsigned int atime = 0x11;	/* stock ams,als_time: (17+1)*2.78 ms = 50 ms */
module_param(atime, uint, 0444);
static unsigned int again = 5;		/* stock ams,als_gain: 16x */
module_param(again, uint, 0444);
static unsigned int prate = 0x32;	/* stock ams,ptime */
module_param(prate, uint, 0444);
static unsigned int ppulse = 0x1d;	/* stock ams,ps_pulse_cnt: 30 pulses */
module_param(ppulse, uint, 0444);
static unsigned int ppulse_len = 2;	/* stock ams,ps_pulse_len: 16 us */
module_param(ppulse_len, uint, 0444);
static unsigned int pgain;		/* stock ams,ps_gain: 1x */
module_param(pgain, uint, 0444);
static unsigned int pdrive = 9;		/* stock ams,ps_drive = 9 (PLDRIVE field is 4 bits; datasheet table lists 0..8) */
module_param(pdrive, uint, 0444);
static unsigned int cfg6 = 0x7f;	/* stock ams,ps_apc: CFG6 with bit 6 = APC disable */
module_param(cfg6, uint, 0444);
static bool ds_init;			/* 1 = also write the datasheet CFG4=0x3D / TEST3=0xC4 (v1 behaviour; stock does not) */
module_param(ds_init, bool, 0444);
static bool prox = true;
module_param(prox, bool, 0444);
static bool force;			/* bind even if the ID register does not match */
module_param(force, bool, 0444);
static bool debug_write;		/* 1 = allow "reg val" writes to the reg_write attribute (attended tuning only) */
module_param(debug_write, bool, 0444);

#define TMD_PS_TH_MAX	58	/* stock ams,ps_th_max: near when PDATA >= crosstalk + 58 */
#define TMD_PS_TH_MIN	30	/* stock ams,ps_th_min: far again when PDATA <= crosstalk + 30 */
#define TMD_XTALK_MAX	255	/* stock ams,ps_crosstalk_max */

struct tmd3702 {
	struct i2c_client *client;
	struct mutex lock;
	u8 enable;
	int crosstalk;		/* -1 = calibration failed */
	bool near;
};

static const unsigned int gain_x[] = { [1] = 1, [3] = 4, [5] = 16, [7] = 64, [8] = 128, [9] = 256, [10] = 512 };

#define TMD_COLOR(_mod, _idx) { \
	.type = IIO_INTENSITY, .modified = 1, .channel2 = _mod, .address = _idx, \
	.info_mask_separate = BIT(IIO_CHAN_INFO_RAW), \
	.info_mask_shared_by_type = BIT(IIO_CHAN_INFO_INT_TIME) | BIT(IIO_CHAN_INFO_HARDWAREGAIN), }

static const struct iio_chan_spec tmd3702_channels[] = {
	{ .type = IIO_LIGHT, .info_mask_separate = BIT(IIO_CHAN_INFO_PROCESSED), },
	TMD_COLOR(IIO_MOD_LIGHT_CLEAR, 0),
	TMD_COLOR(IIO_MOD_LIGHT_RED, 1),
	TMD_COLOR(IIO_MOD_LIGHT_GREEN, 2),
	TMD_COLOR(IIO_MOD_LIGHT_BLUE, 3),
	{ .type = IIO_PROXIMITY, .info_mask_separate = BIT(IIO_CHAN_INFO_RAW), },
};

static int tmd_w(struct tmd3702 *t, u8 reg, u8 val)
{
	return i2c_smbus_write_byte_data(t->client, reg, val);
}

static int tmd3702_read_crgb(struct tmd3702 *t, u16 *v)
{
	u8 buf[8];
	int ret = i2c_smbus_read_i2c_block_data(t->client, TMD_CDATAL, sizeof(buf), buf);

	if (ret < 0)
		return ret;
	if (ret != sizeof(buf))
		return -EIO;
	for (ret = 0; ret < 4; ret++)
		v[ret] = buf[2 * ret] | (buf[2 * ret + 1] << 8);
	return 0;
}

static int tmd3702_read_pdata(struct tmd3702 *t)
{
	int ret = i2c_smbus_read_word_data(t->client, TMD_PDATAL);

	if (ret < 0)
		return ret;
	return ret & ((cfg6 & TMD_CFG6_APC_DIS) ? 0x3ff : 0x3fff);
}

/*
 * Stock tmd3702_offset_calibration(), simplified: proximity only (ALS off), clear STATUS, wait up to 100 ms for a
 * completed proximity cycle, read PDATA = optical + electrical crosstalk with nothing in front of the sensor.
 */
static int tmd3702_calibrate(struct tmd3702 *t)
{
	int i, st = 0, p, ret;

	ret = tmd_w(t, TMD_ENABLE, TMD_EN_PON);
	ret = ret ?: tmd_w(t, TMD_STATUS, 0xff);
	ret = ret ?: tmd_w(t, TMD_ENABLE, TMD_EN_PON | TMD_EN_PEN);
	if (ret)
		return ret;
	for (i = 0; i < 10; i++) {
		msleep(10);
		st = i2c_smbus_read_byte_data(t->client, TMD_STATUS);
		if (st >= 0 && (st & TMD_ST_PINT))
			break;
	}
	p = tmd3702_read_pdata(t);
	tmd_w(t, TMD_STATUS, 0xff);
	tmd_w(t, TMD_ENABLE, t->enable);
	if (p < 0)
		return p;
	dev_info(&t->client->dev, "prox calibration: PDATA %d status 0x%02x after %d ms%s\n", p, st, (i + 1) * 10,
		 (st >= 0 && (st & TMD_ST_PINT)) ? "" : " (no PINT: cycle not seen)");
	t->crosstalk = p < TMD_XTALK_MAX ? p : -1;
	t->near = false;
	return 0;
}

static int tmd3702_read_raw(struct iio_dev *indio_dev, struct iio_chan_spec const *chan,
			    int *val, int *val2, long mask)
{
	struct tmd3702 *t = iio_priv(indio_dev);
	u16 crgb[4];
	int ret;

	mutex_lock(&t->lock);
	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		if (chan->type == IIO_PROXIMITY) {
			ret = tmd3702_read_pdata(t);
			if (ret >= 0) {
				int xt = t->crosstalk < 0 ? 0 : t->crosstalk;

				*val = ret;
				if (ret >= xt + TMD_PS_TH_MAX)
					t->near = true;
				else if (ret <= xt + TMD_PS_TH_MIN)
					t->near = false;
				ret = IIO_VAL_INT;
			}
			break;
		}
		ret = tmd3702_read_crgb(t, crgb);
		if (!ret) { *val = crgb[chan->address]; ret = IIO_VAL_INT; }
		break;
	case IIO_CHAN_INFO_PROCESSED: {
		/* Uncalibrated estimate (ams "counts per lux" form): lux = C * DGF / (t_ms * gain).
		 * DGF 833 = stock ams,als_dgf; the stock IR/segment coefficients are not applied yet. */
		unsigned int tus = (atime + 1) * 2780, g = gain_x[again < ARRAY_SIZE(gain_x) ? again : 0] ?: 1;
		u64 num;

		ret = tmd3702_read_crgb(t, crgb);
		if (ret)
			break;
		num = (u64)crgb[0] * 833 * 1000;
		*val = div_u64(num, (u64)tus * g);
		ret = IIO_VAL_INT;
		break;
	}
	case IIO_CHAN_INFO_INT_TIME:
		*val = 0; *val2 = (atime + 1) * 2780; ret = IIO_VAL_INT_PLUS_MICRO;
		break;
	case IIO_CHAN_INFO_HARDWAREGAIN:
		*val = gain_x[again < ARRAY_SIZE(gain_x) ? again : 0]; ret = IIO_VAL_INT;
		break;
	default:
		ret = -EINVAL;
	}
	mutex_unlock(&t->lock);
	return ret;
}

static ssize_t prox_crosstalk_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct tmd3702 *t = iio_priv(dev_to_iio_dev(dev));

	return sysfs_emit(buf, "%d\n", t->crosstalk);
}

/* write anything: re-run the crosstalk measurement (sensor must be uncovered) */
static ssize_t prox_crosstalk_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t len)
{
	struct tmd3702 *t = iio_priv(dev_to_iio_dev(dev));
	int ret;

	mutex_lock(&t->lock);
	ret = tmd3702_calibrate(t);
	mutex_unlock(&t->lock);
	return ret ? ret : len;
}

static ssize_t prox_near_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct tmd3702 *t = iio_priv(dev_to_iio_dev(dev));

	return sysfs_emit(buf, "%d\n", t->near);
}

static ssize_t regs_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	static const u8 regs[] = { 0x80, 0x81, 0x82, 0x83, 0x8c, 0x8d, 0x8e, 0x8f, 0x90, 0x91, 0x92, 0x93, 0x9c, 0x9d,
				   0x9e, 0x9f, 0xab, 0xac, 0xae, 0xc0, 0xc1, 0xd7, 0xd9, 0xdc, 0xdd, 0xf2 };
	struct tmd3702 *t = iio_priv(dev_to_iio_dev(dev));
	int i, len = 0;

	mutex_lock(&t->lock);
	for (i = 0; i < ARRAY_SIZE(regs); i++)
		len += sysfs_emit_at(buf, len, "%02x=%02x%c", regs[i],
				     i2c_smbus_read_byte_data(t->client, regs[i]) & 0xff,
				     i + 1 < ARRAY_SIZE(regs) ? ' ' : '\n');
	mutex_unlock(&t->lock);
	return len;
}

static ssize_t reg_write_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t len)
{
	struct tmd3702 *t = iio_priv(dev_to_iio_dev(dev));
	unsigned int reg, val;
	int ret;

	if (!debug_write)
		return -EPERM;
	if (sscanf(buf, "%x %x", &reg, &val) != 2 || reg < 0x80 || reg > 0xff || val > 0xff)
		return -EINVAL;
	mutex_lock(&t->lock);
	ret = tmd_w(t, reg, val);
	if (!ret && reg == TMD_ENABLE)
		t->enable = val;
	mutex_unlock(&t->lock);
	dev_info(dev, "debug write 0x%02x = 0x%02x -> %d\n", reg, val, ret);
	return ret ? ret : len;
}

static IIO_DEVICE_ATTR_RW(prox_crosstalk, 0);
static IIO_DEVICE_ATTR_RO(prox_near, 0);
static IIO_DEVICE_ATTR_RO(regs, 0);
static IIO_DEVICE_ATTR_WO(reg_write, 0);

static struct attribute *tmd3702_attrs[] = {
	&iio_dev_attr_prox_crosstalk.dev_attr.attr,
	&iio_dev_attr_prox_near.dev_attr.attr,
	&iio_dev_attr_regs.dev_attr.attr,
	&iio_dev_attr_reg_write.dev_attr.attr,
	NULL
};

static const struct attribute_group tmd3702_attr_group = { .attrs = tmd3702_attrs };

static const struct iio_info tmd3702_info = {
	.read_raw = tmd3702_read_raw,
	.attrs = &tmd3702_attr_group,
};

static void tmd3702_off(void *data)
{
	struct tmd3702 *t = data;

	tmd_w(t, TMD_ENABLE, 0);
}

static int tmd3702_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct iio_dev *indio_dev;
	struct tmd3702 *t;
	int id, rev, rev2, ret;

	ret = devm_regulator_get_enable_optional(dev, "vdd");
	if (ret && ret != -ENODEV)
		return dev_err_probe(dev, ret, "vdd\n");
	ret = devm_regulator_get_enable_optional(dev, "vio");
	if (ret && ret != -ENODEV)
		return dev_err_probe(dev, ret, "vio\n");
	usleep_range(2000, 3000);

	indio_dev = devm_iio_device_alloc(dev, sizeof(*t));
	if (!indio_dev)
		return -ENOMEM;
	t = iio_priv(indio_dev);
	t->client = client;
	t->crosstalk = -1;
	mutex_init(&t->lock);

	id = i2c_smbus_read_byte_data(client, TMD_ID);
	rev = i2c_smbus_read_byte_data(client, TMD_REVID);
	rev2 = i2c_smbus_read_byte_data(client, TMD_REVID2);
	if (id < 0)
		return dev_err_probe(dev, id, "no answer at 0x%02x\n", client->addr);
	dev_info(dev, "TMD3702 ID 0x%02x REVID 0x%02x REVID2 0x%02x\n", id, rev, rev2);
	if ((id & 0xfc) != TMD_ID_VAL && !force)
		return dev_err_probe(dev, -ENODEV, "unexpected ID 0x%02x (load with force=1 to ignore)\n", id);

	if (again >= ARRAY_SIZE(gain_x) || !gain_x[again] || pdrive > 15 || ppulse > 63 || ppulse_len > 3 ||
	    pgain > 3 || atime > 255 || prate > 255 || cfg6 > 255)
		return dev_err_probe(dev, -EINVAL, "bad module parameter\n");

	/* same order as the stock driver: power off, programme, then PON + engines */
	ret = tmd_w(t, TMD_ENABLE, 0);
	if (ds_init) {
		ret = ret ?: tmd_w(t, TMD_CFG4, 0x3d);		/* datasheet "must be 0x3D" (stock: not written) */
		ret = ret ?: tmd_w(t, TMD_TEST3, 0xc4);		/* datasheet "must be 0xC4" (stock: not written) */
	}
	ret = ret ?: tmd_w(t, TMD_ATIME, atime);
	ret = ret ?: tmd_w(t, TMD_PRATE, prate);
	ret = ret ?: tmd_w(t, TMD_WTIME, 0x08);		/* stock ams,wait_time */
	ret = ret ?: tmd_w(t, TMD_PERS, 0x21);		/* stock ams,persist */
	ret = ret ?: tmd_w(t, TMD_PCFG0, (ppulse_len << 6) | ppulse);
	ret = ret ?: tmd_w(t, TMD_PCFG1, (pgain << 6) | pdrive);
	ret = ret ?: tmd_w(t, TMD_CFG6, cfg6);		/* stock ams,ps_apc = 0x7F: APC off */
	ret = ret ?: tmd_w(t, TMD_CFG1, TMD_CFG1_RSVD5 | again);
	ret = ret ?: tmd_w(t, TMD_INTENAB, 0);		/* polled: no interrupts */
	t->enable = TMD_EN_PON | TMD_EN_AEN | TMD_EN_WEN | (prox ? TMD_EN_PEN : 0);
	ret = ret ?: tmd_w(t, TMD_ENABLE, t->enable);
	if (ret)
		return dev_err_probe(dev, ret, "init failed\n");
	ret = devm_add_action_or_reset(dev, tmd3702_off, t);
	if (ret)
		return ret;
	if (prox && tmd3702_calibrate(t))
		dev_warn(dev, "prox calibration failed\n");
	dev_info(dev, "PCFG0 0x%02x PCFG1 0x%02x CFG6 0x%02x ds_init %d crosstalk %d\n",
		 (ppulse_len << 6) | ppulse, (pgain << 6) | pdrive, cfg6, ds_init, t->crosstalk);

	indio_dev->name = "tmd3702";
	indio_dev->info = &tmd3702_info;
	indio_dev->channels = tmd3702_channels;
	indio_dev->num_channels = ARRAY_SIZE(tmd3702_channels);
	indio_dev->modes = INDIO_DIRECT_MODE;
	return devm_iio_device_register(dev, indio_dev);
}

static const struct i2c_device_id tmd3702_id[] = { { "tmd3702" }, { } };
MODULE_DEVICE_TABLE(i2c, tmd3702_id);
static const struct of_device_id tmd3702_of_match[] = { { .compatible = "ams,tmd3702" }, { } };
MODULE_DEVICE_TABLE(of, tmd3702_of_match);

static struct i2c_driver tmd3702_driver = {
	.driver = { .name = "tmd3702", .of_match_table = tmd3702_of_match },
	.probe = tmd3702_probe,
	.id_table = tmd3702_id,
};
module_i2c_driver(tmd3702_driver);
MODULE_DESCRIPTION("ams TMD3702 ALS/colour/proximity (minimal, polled, stock-equivalent proximity set-up)");
MODULE_LICENSE("GPL");
