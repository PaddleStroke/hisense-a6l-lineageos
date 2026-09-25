// SPDX-License-Identifier: GPL-2.0
/*
 * @NAME@ camera sensor driver for the Hisense A6L (SDM660), mainline V4L2.
 *
 * Register tables were extracted from the stock Hisense sensor library
 * (vendor/lib/libmmcamera_@LIB@.so, sensor_lib_t init/res arrays) and are
 * replayed verbatim. Controls follow the MIPI CCS-like register map
 * (0x0100 mode select, 0x0202 coarse integration, 0x0204 analogue gain,
 * 0x0340 frame length, 0x0342 line length).
 * Structure modelled on drivers/media/i2c/s5kjn1.c (Linaro).
 *
 * A6L port, 2026. UNTESTED ON HARDWARE.
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/pm_runtime.h>
#include <linux/regulator/consumer.h>
#include <linux/units.h>
#include <media/v4l2-cci.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-fwnode.h>

#define SNS_MCLK_FREQ			(24 * HZ_PER_MHZ)
#define SNS_DATA_LANES			4

#define SNS_REG_CHIP_ID			CCI_REG16(@IDREG@)
#define SNS_CHIP_ID			@IDVAL@

#define SNS_REG_CTRL_MODE		CCI_REG8(0x0100)
#define SNS_MODE_STREAMING		BIT(0)
#define SNS_REG_ORIENTATION		CCI_REG8(0x0101)
#define SNS_VFLIP			BIT(1)
#define SNS_HFLIP			BIT(0)
#define SNS_REG_EXPOSURE		CCI_REG16(0x0202)
#define SNS_EXPOSURE_MIN		@EXPMIN@
#define SNS_REG_AGAIN			CCI_REG16(0x0204)
#define SNS_AGAIN_MIN			@GMIN@
#define SNS_AGAIN_MAX			@GMAX@
#define SNS_AGAIN_DEFAULT		@GDEF@
#define SNS_REG_VTS			CCI_REG16(0x0340)
#define SNS_VTS_MAX			0xffff
#define SNS_REG_TEST_PATTERN		CCI_REG16(0x0600)

struct sns_seg {
	const struct cci_reg_sequence *regs;
	unsigned int num;
	unsigned int delay_us;		/* sleep after this segment */
};

struct sns_mode {
	u32 width, height, hts, vts, exposure, exposure_margin;
	u32 link_freq_index;
	u64 pixel_rate;			/* VT pixel rate = hts * vts * fps */
	const struct sns_seg *segs;
	unsigned int num_segs;
};

@TABLES@

static const s64 sns_link_freq_menu[] = { @LINKFREQS@ };

static const struct sns_mode sns_modes[] = {
@MODES@
};

/* index = vflip << 1 | hflip */
static const u32 sns_mbus_formats[] = { @FMTS@ };

static const char * const sns_test_pattern_menu[] = {
	"Disabled", "Solid color", "Color bars", "Fade to grey color bars", "PN9",
};

struct sns {
	struct device *dev;
	struct regmap *regmap;
	struct clk *mclk;
	struct gpio_desc *reset_gpio;
	struct regulator *vdda, *vddd, *vddio;

	struct v4l2_subdev sd;
	struct media_pad pad;

	struct v4l2_ctrl_handler ctrl_handler;
	struct v4l2_ctrl *link_freq, *pixel_rate, *hblank, *vblank;
	struct v4l2_ctrl *exposure, *vflip, *hflip;

	const struct sns_mode *mode;
};

#define to_sns(_sd) container_of(_sd, struct sns, sd)

static int sns_write_segs(struct sns *s, const struct sns_seg *segs,
			  unsigned int n)
{
	unsigned int i;
	int ret = 0;

	for (i = 0; i < n && !ret; i++) {
		cci_multi_reg_write(s->regmap, segs[i].regs, segs[i].num, &ret);
		if (segs[i].delay_us)
			fsleep(segs[i].delay_us);
	}
	return ret;
}

static int sns_set_ctrl(struct v4l2_ctrl *ctrl)
{
	struct sns *s = container_of(ctrl->handler, struct sns, ctrl_handler);
	const struct sns_mode *mode = s->mode;
	int ret;

	if (ctrl->id == V4L2_CID_VBLANK) {
		s64 max = mode->height + ctrl->val - mode->exposure_margin;

		__v4l2_ctrl_modify_range(s->exposure, s->exposure->minimum, max,
					 1, min_t(s64, s->exposure->default_value, max));
	}

	if (!pm_runtime_get_if_active(s->dev))
		return 0;

	switch (ctrl->id) {
	case V4L2_CID_ANALOGUE_GAIN:
		ret = cci_write(s->regmap, SNS_REG_AGAIN, ctrl->val, NULL);
		break;
	case V4L2_CID_EXPOSURE:
		ret = cci_write(s->regmap, SNS_REG_EXPOSURE, ctrl->val, NULL);
		break;
	case V4L2_CID_VBLANK:
		ret = cci_write(s->regmap, SNS_REG_VTS, ctrl->val + mode->height, NULL);
		break;
	case V4L2_CID_VFLIP:
	case V4L2_CID_HFLIP:
		ret = cci_write(s->regmap, SNS_REG_ORIENTATION,
				(s->vflip->val ? SNS_VFLIP : 0) |
				(s->hflip->val ? SNS_HFLIP : 0), NULL);
		break;
	case V4L2_CID_TEST_PATTERN:
		ret = cci_write(s->regmap, SNS_REG_TEST_PATTERN, ctrl->val, NULL);
		break;
	default:
		ret = -EINVAL;
		break;
	}

	pm_runtime_put(s->dev);
	return ret;
}

static const struct v4l2_ctrl_ops sns_ctrl_ops = {
	.s_ctrl = sns_set_ctrl,
};

static u64 sns_max_pixel_rate(void)
{
	u64 m = 0;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(sns_modes); i++)
		m = max(m, sns_modes[i].pixel_rate);
	return m;
}

static int sns_init_controls(struct sns *s)
{
	struct v4l2_ctrl_handler *h = &s->ctrl_handler;
	const struct sns_mode *mode = s->mode;
	struct v4l2_fwnode_device_properties props;
	s64 hblank, vblank;
	int ret;

	v4l2_ctrl_handler_init(h, 12);

	s->link_freq = v4l2_ctrl_new_int_menu(h, &sns_ctrl_ops, V4L2_CID_LINK_FREQ,
					      ARRAY_SIZE(sns_link_freq_menu) - 1,
					      mode->link_freq_index,
					      sns_link_freq_menu);
	if (s->link_freq)
		s->link_freq->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	s->pixel_rate = v4l2_ctrl_new_std(h, &sns_ctrl_ops, V4L2_CID_PIXEL_RATE,
					  1, sns_max_pixel_rate(), 1,
					  mode->pixel_rate);

	hblank = mode->hts - mode->width;
	s->hblank = v4l2_ctrl_new_std(h, &sns_ctrl_ops, V4L2_CID_HBLANK,
				      hblank, hblank, 1, hblank);
	if (s->hblank)
		s->hblank->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	vblank = mode->vts - mode->height;
	s->vblank = v4l2_ctrl_new_std(h, &sns_ctrl_ops, V4L2_CID_VBLANK, vblank,
				      SNS_VTS_MAX - mode->height, 1, vblank);

	v4l2_ctrl_new_std(h, &sns_ctrl_ops, V4L2_CID_ANALOGUE_GAIN,
			  SNS_AGAIN_MIN, SNS_AGAIN_MAX, 1, SNS_AGAIN_DEFAULT);

	s->exposure = v4l2_ctrl_new_std(h, &sns_ctrl_ops, V4L2_CID_EXPOSURE,
					SNS_EXPOSURE_MIN,
					mode->vts - mode->exposure_margin, 1,
					mode->exposure);

	v4l2_ctrl_new_std_menu_items(h, &sns_ctrl_ops, V4L2_CID_TEST_PATTERN,
				     ARRAY_SIZE(sns_test_pattern_menu) - 1,
				     0, 0, sns_test_pattern_menu);

	s->hflip = v4l2_ctrl_new_std(h, &sns_ctrl_ops, V4L2_CID_HFLIP, 0, 1, 1, 0);
	if (s->hflip)
		s->hflip->flags |= V4L2_CTRL_FLAG_MODIFY_LAYOUT;
	s->vflip = v4l2_ctrl_new_std(h, &sns_ctrl_ops, V4L2_CID_VFLIP, 0, 1, 1, 0);
	if (s->vflip)
		s->vflip->flags |= V4L2_CTRL_FLAG_MODIFY_LAYOUT;

	ret = v4l2_fwnode_device_parse(s->dev, &props);
	if (ret)
		goto err;
	ret = v4l2_ctrl_new_fwnode_properties(h, &sns_ctrl_ops, &props);
	if (ret)
		goto err;
	if (h->error) {
		ret = h->error;
		goto err;
	}

	s->sd.ctrl_handler = h;
	return 0;
err:
	v4l2_ctrl_handler_free(h);
	return ret;
}

static int sns_enable_streams(struct v4l2_subdev *sd,
			      struct v4l2_subdev_state *state, u32 pad,
			      u64 streams_mask)
{
	struct sns *s = to_sns(sd);
	int ret;

	ret = pm_runtime_resume_and_get(s->dev);
	if (ret)
		return ret;

	ret = sns_write_segs(s, sns_init, ARRAY_SIZE(sns_init));
	if (!ret)
		ret = sns_write_segs(s, s->mode->segs, s->mode->num_segs);
	if (ret)
		goto error;

	ret = __v4l2_ctrl_handler_setup(s->sd.ctrl_handler);
	if (ret)
		goto error;

	ret = cci_write(s->regmap, SNS_REG_CTRL_MODE, SNS_MODE_STREAMING, NULL);
	if (ret)
		goto error;

	return 0;

error:
	dev_err(s->dev, "failed to start streaming: %d\n", ret);
	pm_runtime_put_autosuspend(s->dev);
	return ret;
}

static int sns_disable_streams(struct v4l2_subdev *sd,
			       struct v4l2_subdev_state *state, u32 pad,
			       u64 streams_mask)
{
	struct sns *s = to_sns(sd);
	int ret;

	ret = cci_write(s->regmap, SNS_REG_CTRL_MODE, 0, NULL);
	if (ret)
		dev_err(s->dev, "failed to stop streaming: %d\n", ret);

	pm_runtime_put_autosuspend(s->dev);
	return ret;
}

static u32 sns_get_format_code(struct sns *s)
{
	return sns_mbus_formats[(s->vflip->val ? 2 : 0) | (s->hflip->val ? 1 : 0)];
}

static void sns_update_pad_format(struct sns *s, const struct sns_mode *mode,
				  struct v4l2_mbus_framefmt *fmt)
{
	fmt->code = sns_get_format_code(s);
	fmt->width = mode->width;
	fmt->height = mode->height;
	fmt->field = V4L2_FIELD_NONE;
	fmt->colorspace = V4L2_COLORSPACE_RAW;
	fmt->ycbcr_enc = V4L2_YCBCR_ENC_DEFAULT;
	fmt->quantization = V4L2_QUANTIZATION_FULL_RANGE;
	fmt->xfer_func = V4L2_XFER_FUNC_NONE;
}

static int sns_set_pad_format(struct v4l2_subdev *sd,
			      struct v4l2_subdev_state *state,
			      struct v4l2_subdev_format *fmt)
{
	struct sns *s = to_sns(sd);
	const struct sns_mode *mode;
	s64 hblank, vblank;

	mode = v4l2_find_nearest_size(sns_modes, ARRAY_SIZE(sns_modes),
				      width, height,
				      fmt->format.width, fmt->format.height);

	sns_update_pad_format(s, mode, &fmt->format);

	if (fmt->which == V4L2_SUBDEV_FORMAT_TRY || s->mode == mode)
		goto set_format;

	s->mode = mode;

	__v4l2_ctrl_s_ctrl(s->link_freq, mode->link_freq_index);
	__v4l2_ctrl_s_ctrl_int64(s->pixel_rate, mode->pixel_rate);

	hblank = mode->hts - mode->width;
	__v4l2_ctrl_modify_range(s->hblank, hblank, hblank, 1, hblank);

	vblank = mode->vts - mode->height;
	__v4l2_ctrl_modify_range(s->vblank, vblank, SNS_VTS_MAX - mode->height,
				 1, vblank);
	__v4l2_ctrl_s_ctrl(s->vblank, vblank);

	__v4l2_ctrl_modify_range(s->exposure, SNS_EXPOSURE_MIN,
				 mode->vts - mode->exposure_margin, 1,
				 mode->exposure);
	__v4l2_ctrl_s_ctrl(s->exposure, mode->exposure);

	if (s->sd.ctrl_handler->error)
		return s->sd.ctrl_handler->error;

set_format:
	*v4l2_subdev_state_get_format(state, 0) = fmt->format;
	return 0;
}

static int sns_enum_mbus_code(struct v4l2_subdev *sd,
			      struct v4l2_subdev_state *sd_state,
			      struct v4l2_subdev_mbus_code_enum *code)
{
	if (code->index > 0)
		return -EINVAL;
	code->code = sns_get_format_code(to_sns(sd));
	return 0;
}

static int sns_enum_frame_size(struct v4l2_subdev *sd,
			       struct v4l2_subdev_state *sd_state,
			       struct v4l2_subdev_frame_size_enum *fse)
{
	struct sns *s = to_sns(sd);

	if (fse->index >= ARRAY_SIZE(sns_modes))
		return -EINVAL;
	if (fse->code != sns_get_format_code(s))
		return -EINVAL;

	fse->min_width = fse->max_width = sns_modes[fse->index].width;
	fse->min_height = fse->max_height = sns_modes[fse->index].height;
	return 0;
}

static int sns_get_selection(struct v4l2_subdev *sd,
			     struct v4l2_subdev_state *sd_state,
			     struct v4l2_subdev_selection *sel)
{
	switch (sel->target) {
	case V4L2_SEL_TGT_CROP:
	case V4L2_SEL_TGT_CROP_DEFAULT:
	case V4L2_SEL_TGT_CROP_BOUNDS:
	case V4L2_SEL_TGT_NATIVE_SIZE:
		sel->r.left = 0;
		sel->r.top = 0;
		sel->r.width = @NATIVE_W@;
		sel->r.height = @NATIVE_H@;
		return 0;
	default:
		return -EINVAL;
	}
}

static int sns_init_state(struct v4l2_subdev *sd, struct v4l2_subdev_state *state)
{
	struct sns *s = to_sns(sd);
	struct v4l2_subdev_format fmt = {
		.which = V4L2_SUBDEV_FORMAT_TRY,
		.pad = 0,
		.format = {
			.width = s->mode->width,
			.height = s->mode->height,
		},
	};

	sns_set_pad_format(sd, state, &fmt);
	return 0;
}

static const struct v4l2_subdev_video_ops sns_video_ops = {
	.s_stream = v4l2_subdev_s_stream_helper,
};

static const struct v4l2_subdev_pad_ops sns_pad_ops = {
	.set_fmt = sns_set_pad_format,
	.get_fmt = v4l2_subdev_get_fmt,
	.get_selection = sns_get_selection,
	.enum_mbus_code = sns_enum_mbus_code,
	.enum_frame_size = sns_enum_frame_size,
	.enable_streams = sns_enable_streams,
	.disable_streams = sns_disable_streams,
};

static const struct v4l2_subdev_ops sns_subdev_ops = {
	.video = &sns_video_ops,
	.pad = &sns_pad_ops,
};

static const struct v4l2_subdev_internal_ops sns_internal_ops = {
	.init_state = sns_init_state,
};

static const struct media_entity_operations sns_entity_ops = {
	.link_validate = v4l2_subdev_link_validate,
};

static int sns_identify(struct sns *s)
{
	u64 val;
	int ret;

	ret = cci_read(s->regmap, SNS_REG_CHIP_ID, &val, NULL);
	if (ret)
		return dev_err_probe(s->dev, ret, "failed to read chip id\n");

	if (val != SNS_CHIP_ID)
		return dev_err_probe(s->dev, -ENODEV,
				     "chip id mismatch: 0x%x != 0x%llx\n",
				     SNS_CHIP_ID, val);

	dev_info(s->dev, "@NAME@ chip id 0x%llx\n", val);
	return 0;
}

static int sns_check_hwcfg(struct sns *s)
{
	struct fwnode_handle *fwnode = dev_fwnode(s->dev), *ep;
	struct v4l2_fwnode_endpoint bus_cfg = {
		.bus_type = V4L2_MBUS_CSI2_DPHY,
	};
	unsigned long freq_bitmap;
	int ret;

	if (!fwnode)
		return -ENODEV;

	ep = fwnode_graph_get_next_endpoint(fwnode, NULL);
	if (!ep)
		return -EINVAL;

	ret = v4l2_fwnode_endpoint_alloc_parse(ep, &bus_cfg);
	fwnode_handle_put(ep);
	if (ret)
		return ret;

	if (bus_cfg.bus.mipi_csi2.num_data_lanes != SNS_DATA_LANES) {
		dev_err(s->dev, "only 4 data lanes supported (got %u)\n",
			bus_cfg.bus.mipi_csi2.num_data_lanes);
		ret = -EINVAL;
		goto out;
	}

	ret = v4l2_link_freq_to_bitmap(s->dev, bus_cfg.link_frequencies,
				       bus_cfg.nr_of_link_frequencies,
				       sns_link_freq_menu,
				       ARRAY_SIZE(sns_link_freq_menu),
				       &freq_bitmap);
out:
	v4l2_fwnode_endpoint_free(&bus_cfg);
	return ret;
}

/* Stock power-up (sensor_lib_t power_setting_array): @POWERSEQ@ */
static int sns_power_on(struct device *dev)
{
	struct sns *s = to_sns(dev_get_drvdata(dev));
	int ret;

	gpiod_set_value_cansleep(s->reset_gpio, 1);
	usleep_range(1000, 1500);

	if (s->vddio) {
		ret = regulator_enable(s->vddio);
		if (ret)
			return ret;
	}
	if (s->vddd) {
		ret = regulator_enable(s->vddd);
		if (ret)
			goto off_vddio;
	}
	ret = regulator_enable(s->vdda);
	if (ret)
		goto off_vddd;
	usleep_range(5000, 6000);

	ret = clk_prepare_enable(s->mclk);
	if (ret)
		goto off_vdda;
	usleep_range(2000, 3000);

	gpiod_set_value_cansleep(s->reset_gpio, 0);
	usleep_range(8000, 10000);
	return 0;

off_vdda:
	regulator_disable(s->vdda);
off_vddd:
	if (s->vddd)
		regulator_disable(s->vddd);
off_vddio:
	if (s->vddio)
		regulator_disable(s->vddio);
	return ret;
}

static int sns_power_off(struct device *dev)
{
	struct sns *s = to_sns(dev_get_drvdata(dev));

	gpiod_set_value_cansleep(s->reset_gpio, 1);
	clk_disable_unprepare(s->mclk);
	regulator_disable(s->vdda);
	if (s->vddd)
		regulator_disable(s->vddd);
	if (s->vddio)
		regulator_disable(s->vddio);
	return 0;
}

static struct regulator *sns_get_opt_reg(struct sns *s, const char *name)
{
	struct regulator *r = devm_regulator_get_optional(s->dev, name);

	if (IS_ERR(r) && PTR_ERR(r) == -ENODEV)
		return NULL;
	return r;
}

static int sns_probe(struct i2c_client *client)
{
	struct sns *s;
	unsigned long freq;
	int ret;

	s = devm_kzalloc(&client->dev, sizeof(*s), GFP_KERNEL);
	if (!s)
		return -ENOMEM;

	s->dev = &client->dev;
	v4l2_i2c_subdev_init(&s->sd, client, &sns_subdev_ops);

	s->regmap = devm_cci_regmap_init_i2c(client, 16);
	if (IS_ERR(s->regmap))
		return dev_err_probe(s->dev, PTR_ERR(s->regmap), "failed to init CCI\n");

	s->mclk = devm_v4l2_sensor_clk_get(s->dev, NULL);
	if (IS_ERR(s->mclk))
		return dev_err_probe(s->dev, PTR_ERR(s->mclk), "failed to get MCLK\n");

	freq = clk_get_rate(s->mclk);
	if (freq != SNS_MCLK_FREQ)
		return dev_err_probe(s->dev, -EINVAL, "MCLK %lu Hz not supported\n", freq);

	ret = sns_check_hwcfg(s);
	if (ret)
		return dev_err_probe(s->dev, ret, "failed to check HW configuration\n");

	s->reset_gpio = devm_gpiod_get_optional(s->dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(s->reset_gpio))
		return dev_err_probe(s->dev, PTR_ERR(s->reset_gpio), "cannot get reset GPIO\n");

	s->vdda = devm_regulator_get(s->dev, "vdda");
	if (IS_ERR(s->vdda))
		return dev_err_probe(s->dev, PTR_ERR(s->vdda), "vdda\n");
	s->vddd = sns_get_opt_reg(s, "vddd");
	if (IS_ERR(s->vddd))
		return dev_err_probe(s->dev, PTR_ERR(s->vddd), "vddd\n");
	s->vddio = sns_get_opt_reg(s, "vddio");
	if (IS_ERR(s->vddio))
		return dev_err_probe(s->dev, PTR_ERR(s->vddio), "vddio\n");

	ret = sns_power_on(s->dev);
	if (ret)
		return ret;

	ret = sns_identify(s);
	if (ret)
		goto power_off;

	s->mode = &sns_modes[0];
	ret = sns_init_controls(s);
	if (ret) {
		dev_err_probe(s->dev, ret, "failed to init controls\n");
		goto power_off;
	}

	s->sd.state_lock = s->ctrl_handler.lock;
	s->sd.internal_ops = &sns_internal_ops;
	s->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	s->sd.entity.ops = &sns_entity_ops;
	s->sd.entity.function = MEDIA_ENT_F_CAM_SENSOR;
	s->pad.flags = MEDIA_PAD_FL_SOURCE;

	ret = media_entity_pads_init(&s->sd.entity, 1, &s->pad);
	if (ret)
		goto free_ctrls;

	ret = v4l2_subdev_init_finalize(&s->sd);
	if (ret)
		goto entity_cleanup;

	pm_runtime_set_active(s->dev);
	pm_runtime_enable(s->dev);

	ret = v4l2_async_register_subdev_sensor(&s->sd);
	if (ret)
		goto subdev_cleanup;

	pm_runtime_set_autosuspend_delay(s->dev, 1000);
	pm_runtime_use_autosuspend(s->dev);
	pm_runtime_idle(s->dev);
	return 0;

subdev_cleanup:
	v4l2_subdev_cleanup(&s->sd);
	pm_runtime_disable(s->dev);
	pm_runtime_set_suspended(s->dev);
entity_cleanup:
	media_entity_cleanup(&s->sd.entity);
free_ctrls:
	v4l2_ctrl_handler_free(s->sd.ctrl_handler);
power_off:
	sns_power_off(s->dev);
	return ret;
}

static void sns_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct sns *s = to_sns(sd);

	v4l2_async_unregister_subdev(sd);
	v4l2_subdev_cleanup(sd);
	media_entity_cleanup(&sd->entity);
	v4l2_ctrl_handler_free(sd->ctrl_handler);
	pm_runtime_disable(s->dev);
	if (!pm_runtime_status_suspended(s->dev)) {
		sns_power_off(s->dev);
		pm_runtime_set_suspended(s->dev);
	}
}

static const struct dev_pm_ops sns_pm_ops = {
	SET_RUNTIME_PM_OPS(sns_power_off, sns_power_on, NULL)
};

static const struct of_device_id sns_of_match[] = {
	{ .compatible = "@COMPAT@" },
	{ }
};
MODULE_DEVICE_TABLE(of, sns_of_match);

static struct i2c_driver sns_i2c_driver = {
	.driver = {
		.name = "@MODNAME@",
		.pm = &sns_pm_ops,
		.of_match_table = sns_of_match,
	},
	.probe = sns_probe,
	.remove = sns_remove,
};
module_i2c_driver(sns_i2c_driver);

MODULE_DESCRIPTION("@NAME@ sensor driver (A6L, stock register tables)");
MODULE_LICENSE("GPL");
