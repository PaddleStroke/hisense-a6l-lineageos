// SPDX-License-Identifier: GPL-2.0
/*
 * Giantec GT9769 VCM (voice coil) lens driver for the Hisense A6L main camera.
 *
 * Register usage taken from the stock Hisense actuator library
 * vendor/lib/libactuator_gt9769.so (actuator_driver_params):
 *   I2C 0x18 (8-bit) = 0x0c, byte address / word data, 10-bit DAC,
 *   init: 0x02 = 0x02 (ring control), 0x06 = 0x61 (mode/SAC), 0x07 = 0x39 (timing),
 *   position: 16-bit write to 0x03 (MSB) / 0x04 (LSB).
 * Same register layout as DW9719/DW9800 (drivers/media/i2c/dw9719.c), on which
 * this is modelled. UNTESTED ON HARDWARE.
 */

#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/pm_runtime.h>
#include <linux/regulator/consumer.h>

#include <media/v4l2-cci.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-subdev.h>

#define GT9769_MAX_FOCUS_POS	1023
#define GT9769_CTRL_STEPS	16
#define GT9769_CTRL_DELAY_US	1000

#define GT9769_CONTROL		CCI_REG8(0x02)
#define GT9769_STANDBY		0x00
#define GT9769_SHUTDOWN		0x01
#define GT9769_RING		0x02
#define GT9769_VCM_CURRENT	CCI_REG16(0x03)
#define GT9769_MODE		CCI_REG8(0x06)
#define GT9769_STOCK_MODE	0x61
#define GT9769_TIMING		CCI_REG8(0x07)
#define GT9769_STOCK_TIMING	0x39

struct gt9769 {
	struct v4l2_subdev sd;
	struct device *dev;
	struct regmap *regmap;
	struct regulator *vdd;
	struct v4l2_ctrl_handler handler;
	struct v4l2_ctrl *focus;
};

#define to_gt9769(x) container_of(x, struct gt9769, sd)

static int gt9769_power_down(struct gt9769 *g)
{
	cci_write(g->regmap, GT9769_CONTROL, GT9769_SHUTDOWN, NULL);
	return regulator_disable(g->vdd);
}

static int gt9769_power_up(struct gt9769 *g)
{
	int ret;

	ret = regulator_enable(g->vdd);
	if (ret)
		return ret;

	/* wake from shutdown; first access may NAK */
	cci_write(g->regmap, GT9769_CONTROL, GT9769_STANDBY, NULL);
	fsleep(200);
	ret = 0;
	cci_write(g->regmap, GT9769_CONTROL, GT9769_STANDBY, &ret);
	fsleep(1000);
	cci_write(g->regmap, GT9769_CONTROL, GT9769_RING, &ret);
	cci_write(g->regmap, GT9769_MODE, GT9769_STOCK_MODE, &ret);
	cci_write(g->regmap, GT9769_TIMING, GT9769_STOCK_TIMING, &ret);
	if (ret)
		gt9769_power_down(g);
	return ret;
}

static int gt9769_set_pos(struct gt9769 *g, s32 val)
{
	return cci_write(g->regmap, GT9769_VCM_CURRENT, val, NULL);
}

static int gt9769_set_ctrl(struct v4l2_ctrl *ctrl)
{
	struct gt9769 *g = container_of(ctrl->handler, struct gt9769, handler);
	int ret;

	if (!pm_runtime_get_if_in_use(g->dev))
		return 0;

	ret = ctrl->id == V4L2_CID_FOCUS_ABSOLUTE ? gt9769_set_pos(g, ctrl->val) : -EINVAL;

	pm_runtime_put(g->dev);
	return ret;
}

static const struct v4l2_ctrl_ops gt9769_ctrl_ops = {
	.s_ctrl = gt9769_set_ctrl,
};

static int gt9769_suspend(struct device *dev)
{
	struct gt9769 *g = to_gt9769(dev_get_drvdata(dev));
	int val;

	/* park the lens slowly to avoid the click */
	for (val = g->focus->val; val >= 0; val -= GT9769_CTRL_STEPS) {
		gt9769_set_pos(g, val);
		usleep_range(GT9769_CTRL_DELAY_US, GT9769_CTRL_DELAY_US + 10);
	}
	return gt9769_power_down(g);
}

static int gt9769_resume(struct device *dev)
{
	struct gt9769 *g = to_gt9769(dev_get_drvdata(dev));
	int cur = g->focus->val, val, ret;

	ret = gt9769_power_up(g);
	if (ret)
		return ret;

	for (val = cur % GT9769_CTRL_STEPS; val <= cur; val += GT9769_CTRL_STEPS) {
		ret = gt9769_set_pos(g, val);
		if (ret) {
			gt9769_power_down(g);
			return ret;
		}
		usleep_range(GT9769_CTRL_DELAY_US, GT9769_CTRL_DELAY_US + 10);
	}
	return 0;
}

static int gt9769_open(struct v4l2_subdev *sd, struct v4l2_subdev_fh *fh)
{
	return pm_runtime_resume_and_get(sd->dev);
}

static int gt9769_close(struct v4l2_subdev *sd, struct v4l2_subdev_fh *fh)
{
	pm_runtime_put_autosuspend(sd->dev);
	return 0;
}

static const struct v4l2_subdev_internal_ops gt9769_internal_ops = {
	.open = gt9769_open,
	.close = gt9769_close,
};

static const struct v4l2_subdev_ops gt9769_ops = { };

static int gt9769_probe(struct i2c_client *client)
{
	struct gt9769 *g;
	int ret;

	g = devm_kzalloc(&client->dev, sizeof(*g), GFP_KERNEL);
	if (!g)
		return -ENOMEM;

	g->dev = &client->dev;
	g->regmap = devm_cci_regmap_init_i2c(client, 8);
	if (IS_ERR(g->regmap))
		return PTR_ERR(g->regmap);

	g->vdd = devm_regulator_get(&client->dev, "vdd");
	if (IS_ERR(g->vdd))
		return dev_err_probe(&client->dev, PTR_ERR(g->vdd), "vdd regulator\n");

	v4l2_i2c_subdev_init(&g->sd, client, &gt9769_ops);
	g->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	g->sd.internal_ops = &gt9769_internal_ops;

	v4l2_ctrl_handler_init(&g->handler, 1);
	g->focus = v4l2_ctrl_new_std(&g->handler, &gt9769_ctrl_ops,
				     V4L2_CID_FOCUS_ABSOLUTE, 0,
				     GT9769_MAX_FOCUS_POS, 1, 0);
	if (g->handler.error) {
		ret = g->handler.error;
		goto err_free;
	}
	g->sd.ctrl_handler = &g->handler;

	ret = media_entity_pads_init(&g->sd.entity, 0, NULL);
	if (ret)
		goto err_free;
	g->sd.entity.function = MEDIA_ENT_F_LENS;

	ret = gt9769_power_up(g);
	if (ret) {
		dev_err_probe(g->dev, ret, "GT9769 not responding at 0x%02x\n", client->addr);
		goto err_media;
	}
	dev_info(g->dev, "GT9769 VCM initialised (stock mode 0x61 / timing 0x39)\n");

	pm_runtime_set_active(g->dev);
	pm_runtime_get_noresume(g->dev);
	pm_runtime_enable(g->dev);

	ret = v4l2_async_register_subdev(&g->sd);
	if (ret)
		goto err_pm;

	pm_runtime_set_autosuspend_delay(g->dev, 1000);
	pm_runtime_use_autosuspend(g->dev);
	pm_runtime_put_autosuspend(g->dev);
	return 0;

err_pm:
	pm_runtime_disable(g->dev);
	pm_runtime_put_noidle(g->dev);
	gt9769_power_down(g);
err_media:
	media_entity_cleanup(&g->sd.entity);
err_free:
	v4l2_ctrl_handler_free(&g->handler);
	return ret;
}

static void gt9769_remove(struct i2c_client *client)
{
	struct gt9769 *g = to_gt9769(i2c_get_clientdata(client));

	v4l2_async_unregister_subdev(&g->sd);
	v4l2_ctrl_handler_free(&g->handler);
	media_entity_cleanup(&g->sd.entity);
	pm_runtime_disable(&client->dev);
	if (!pm_runtime_status_suspended(&client->dev))
		gt9769_power_down(g);
	pm_runtime_set_suspended(&client->dev);
}

static const struct of_device_id gt9769_of_table[] = {
	{ .compatible = "giantec,gt9769" },
	{ }
};
MODULE_DEVICE_TABLE(of, gt9769_of_table);

static DEFINE_RUNTIME_DEV_PM_OPS(gt9769_pm_ops, gt9769_suspend, gt9769_resume, NULL);

static struct i2c_driver gt9769_i2c_driver = {
	.driver = {
		.name = "gt9769",
		.pm = pm_ptr(&gt9769_pm_ops),
		.of_match_table = gt9769_of_table,
	},
	.probe = gt9769_probe,
	.remove = gt9769_remove,
};
module_i2c_driver(gt9769_i2c_driver);

MODULE_DESCRIPTION("Giantec GT9769 VCM driver (A6L)");
MODULE_LICENSE("GPL");
