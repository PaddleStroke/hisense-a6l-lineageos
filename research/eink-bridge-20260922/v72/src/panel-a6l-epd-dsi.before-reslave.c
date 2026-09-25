// SPDX-License-Identifier: GPL-2.0-only
/*
 * Hisense A6L rear e-paper as a plain DSI video sink — NO bridge programming.
 *
 * Ground truth from the stock kernel log (adb bugreport, 21 Sep 2026): stock's I2C bridge init fails on every update
 * ("tc358762_send_init_cmd, ret=-107", NACK at 0x0b and 0x0f) and the panel has no DSI on-commands, yet e-ink works.
 * So whatever sits between DSI1 and the panel needs no configuration. This driver therefore replaces the mainline
 * tc358762 bridge driver (which sends DSI generic writes) AND panels/panel-a6l-epd.c. It binds to the existing V71 DT
 * node (compatible "toshiba,tc358762" on DSI1) so no new image is needed; the TPS65185 rails are taken from the
 * /a6l-epd-panel node. Do not load tc358762-a6l.ko or panel-a6l-epd.ko together with this module.
 *
 * Stock order (mdss_dsi_panel_power_ctrl / mdss_dsi_on): epd_pwr(42)=1, XON(61)=1, tps power_on(gpio2), vdcc(45)=1,
 * i2c_en(56)=1, 5 ms, reset low 10 ms / high 10 ms, clock lane HS, tps65185_active_mode (rails), video.
 * XON (gpio61) is not in the V71 DT: hold it high from userspace (a6l_gpio_hold) for now.
 */
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/regulator/consumer.h>
#include <drm/drm_mipi_dsi.h>
#include <drm/drm_modes.h>
#include <drm/drm_panel.h>
#include <drm/drm_probe_helper.h>

static bool hv;
module_param(hv, bool, 0644);
static unsigned int lanes = 2;
module_param(lanes, uint, 0444);
static unsigned int dsi_mode;	/* experiment: 0 = stock (non-burst sync pulse), 1 = non-burst sync event, 2 = burst */
module_param(dsi_mode, uint, 0444);
static bool lpm;		/* experiment: allow LP during blanking / non-continuous clock */
module_param(lpm, bool, 0444);
static bool eot = true;	/* stock appends EoT (EOT_PACKET_CTRL, absolute 0xcc, reads 1); eot=0 is an experiment only */
module_param(eot, bool, 0444);
static unsigned int bridge = 1;	/* 0 = never touch the bridge, 1 = stock TC358767 I2C table, 2 = ID read only */
module_param(bridge, uint, 0644);
static char *bridge_bus = "/soc@0/i2c@c176000";
module_param(bridge_bus, charp, 0444);
MODULE_PARM_DESC(hv, "enable the e-paper high-voltage rails and VCOM while the output is enabled");

struct a6l_epd_dsi {
	struct drm_panel panel;
	struct mipi_dsi_device *dsi;
	struct gpio_desc *reset, *xon;
	struct regulator *vddc, *v3p3, *vposneg, *vcom;
	bool hv_on;
	bool bridge_ok;
};

/*
 * 22 Sep 2026, rooted stock: a TC358767 (IDREG 0x0500 = 0x6601) ACKs at I2C 0x0f on the e-paper bus ONLY while an update
 * runs, and holds exactly the stock "tc358767_cmd" table below (DSI-RX -> parallel out, 19.2 MHz REFCLK, 2 lanes).
 * Registers are 16-bit big-endian addresses with 32-bit little-endian values.
 */
#define A6L_BRIDGE_ADDR 0x0f
static const struct { u16 reg; u32 val; } a6l_tc358767_table[] = {
	{0x0448, 0x86}, {0x06a0, 0x3080}, {0x0914, 0x11c201}, {0x0904, 0}, {0x0904, 4}, {0x0908, 1}, {0x0908, 5},
	{0x0918, 0x110}, {0x0800, 0x1100}, {0x0800, 0x1100}, {0x013c, 0x30005}, {0x0114, 3}, {0x0164, 4}, {0x0168, 4},
	{0x016c, 4}, {0x0170, 4}, {0x0134, 7}, {0x0210, 7}, {0x0104, 1}, {0x0204, 1}, {0x0450, 0x03f00100},
	{0x0454, 0x007d0005}, {0x045c, 0x00040002}, {0x0464, 1}, {0x0510, 1},
};

static int a6l_bridge_rd(struct i2c_adapter *a, u16 reg, u32 *val)
{
	u8 r[2] = { reg >> 8, reg }, v[4] = { 0 };
	struct i2c_msg m[2] = { { A6L_BRIDGE_ADDR, 0, 2, r }, { A6L_BRIDGE_ADDR, I2C_M_RD, 4, v } };
	int ret = i2c_transfer(a, m, 2);

	if (ret != 2)
		return ret < 0 ? ret : -EIO;
	*val = v[0] | v[1] << 8 | v[2] << 16 | (u32)v[3] << 24;
	return 0;
}

static int a6l_bridge_wr(struct i2c_adapter *a, u16 reg, u32 val)
{
	u8 b[6] = { reg >> 8, reg, val, val >> 8, val >> 16, val >> 24 };
	struct i2c_msg m = { A6L_BRIDGE_ADDR, 0, 6, b };
	int ret = i2c_transfer(a, &m, 1);

	return ret == 1 ? 0 : (ret < 0 ? ret : -EIO);
}

/* Returns 0 when the bridge answered (and, for bridge=1, took the whole table). */
static int a6l_bridge_init(struct device *dev, const char *when)
{
	struct device_node *np;
	struct i2c_adapter *a;
	u32 id = 0;
	int ret, i;

	if (!bridge)
		return 0;
	np = of_find_node_by_path(bridge_bus);
	a = np ? of_get_i2c_adapter_by_node(np) : NULL;
	of_node_put(np);
	if (!a) {
		dev_warn(dev, "bridge (%s): no I2C adapter at %s\n", when, bridge_bus);
		return -ENODEV;
	}
	ret = a6l_bridge_rd(a, 0x0500, &id);
	dev_info(dev, "bridge (%s): IDREG ret=%d id=%08x\n", when, ret, id);
	if (!ret && bridge == 1) {
		for (i = 0; i < ARRAY_SIZE(a6l_tc358767_table) && !ret; i++)
			ret = a6l_bridge_wr(a, a6l_tc358767_table[i].reg, a6l_tc358767_table[i].val);
		dev_info(dev, "bridge (%s): stock table %s (%d/%zu writes, ret=%d)\n", when, ret ? "FAILED" : "written",
			 i, ARRAY_SIZE(a6l_tc358767_table), ret);
	}
	i2c_put_adapter(a);
	return ret;
}

static inline struct a6l_epd_dsi *to_ctx(struct drm_panel *panel)
{
	return container_of(panel, struct a6l_epd_dsi, panel);
}

static int a6l_epd_dsi_prepare(struct drm_panel *panel)
{
	struct a6l_epd_dsi *ctx = to_ctx(panel);
	int ret;

	ret = regulator_enable(ctx->vddc);
	if (ret)
		return ret;
	ret = regulator_enable(ctx->v3p3);
	if (ret)
		goto err_vddc;
	usleep_range(5000, 6000);
	gpiod_set_value_cansleep(ctx->reset, 1);	/* asserted = physically low */
	usleep_range(10000, 11000);
	gpiod_set_value_cansleep(ctx->reset, 0);
	usleep_range(10000, 11000);
	gpiod_set_value_cansleep(ctx->xon, 1);	/* XON low = all gates on: must be high before the rails rise (22 Sep) */
	ctx->bridge_ok = !a6l_bridge_init(panel->dev, "prepare");	/* link is in LP-11 here (prepare_prev_first) */
	if (hv) {
		ret = regulator_enable(ctx->vposneg);
		if (ret)
			goto err_v3p3;
		msleep(20);
		ret = regulator_enable(ctx->vcom);
		if (ret) {
			regulator_disable(ctx->vposneg);
			goto err_v3p3;
		}
		ctx->hv_on = true;
		dev_info(panel->dev, "e-paper rails ON\n");
	}
	return 0;
err_v3p3:
	regulator_disable(ctx->v3p3);
err_vddc:
	regulator_disable(ctx->vddc);
	return ret;
}

static int a6l_epd_dsi_unprepare(struct drm_panel *panel)
{
	struct a6l_epd_dsi *ctx = to_ctx(panel);

	if (ctx->hv_on) {
		regulator_disable(ctx->vcom);
		regulator_disable(ctx->vposneg);
		ctx->hv_on = false;
		dev_info(panel->dev, "e-paper rails OFF\n");
	}
	gpiod_set_value_cansleep(ctx->xon, 0);
	gpiod_set_value_cansleep(ctx->reset, 1);
	regulator_disable(ctx->v3p3);
	regulator_disable(ctx->vddc);
	return 0;
}

/* Second chance once the clock lane is in HS and video runs, in case the chip needs the DSI clock to answer. */
static int a6l_epd_dsi_enable(struct drm_panel *panel)
{
	struct a6l_epd_dsi *ctx = to_ctx(panel);

	if (!ctx->bridge_ok)
		ctx->bridge_ok = !a6l_bridge_init(panel->dev, "enable");
	return 0;
}

static const struct drm_display_mode a6l_epd_dsi_mode = {
	.clock = 40046,
	.hdisplay = 384, .hsync_start = 384 + 126, .hsync_end = 384 + 126 + 6, .htotal = 384 + 126 + 6 + 125,
	.vdisplay = 725, .vsync_start = 725 + 4, .vsync_end = 725 + 4 + 2, .vtotal = 725 + 4 + 2 + 4,
	.width_mm = 65, .height_mm = 130,
	.type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED,
};

static int a6l_epd_dsi_get_modes(struct drm_panel *panel, struct drm_connector *connector)
{
	return drm_connector_helper_get_modes_fixed(connector, &a6l_epd_dsi_mode);
}

static const struct drm_panel_funcs a6l_epd_dsi_funcs = {
	.prepare = a6l_epd_dsi_prepare,
	.unprepare = a6l_epd_dsi_unprepare,
	.enable = a6l_epd_dsi_enable,
	.get_modes = a6l_epd_dsi_get_modes,
};

static int a6l_epd_dsi_probe(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	struct device_node *rails;
	struct a6l_epd_dsi *ctx;
	int ret;

	ctx = devm_drm_panel_alloc(dev, struct a6l_epd_dsi, panel, &a6l_epd_dsi_funcs, DRM_MODE_CONNECTOR_DSI);
	if (IS_ERR(ctx))
		return PTR_ERR(ctx);
	ctx->dsi = dsi;
	ctx->reset = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(ctx->reset))
		return dev_err_probe(dev, PTR_ERR(ctx->reset), "reset gpio\n");
	ctx->xon = devm_gpiod_get_optional(dev, "xon", GPIOD_OUT_LOW);
	if (IS_ERR(ctx->xon))
		return dev_err_probe(dev, PTR_ERR(ctx->xon), "xon gpio\n");
	ctx->vddc = devm_regulator_get(dev, "vddc");
	if (IS_ERR(ctx->vddc))
		return dev_err_probe(dev, PTR_ERR(ctx->vddc), "vddc\n");
	rails = of_find_node_by_path("/a6l-epd-panel");
	if (!rails)
		return dev_err_probe(dev, -ENODEV, "/a6l-epd-panel node missing\n");
	ctx->v3p3 = devm_of_regulator_get(dev, rails, "power");
	ctx->vposneg = devm_of_regulator_get(dev, rails, "vposneg");
	ctx->vcom = devm_of_regulator_get(dev, rails, "vcom");
	of_node_put(rails);
	if (IS_ERR(ctx->v3p3) || IS_ERR(ctx->vposneg) || IS_ERR(ctx->vcom))
		return dev_err_probe(dev, -EPROBE_DEFER, "TPS65185 rails not ready\n");

	mipi_dsi_set_drvdata(dsi, ctx);
	dsi->lanes = (lanes >= 1 && lanes <= 4) ? lanes : 2;
	dsi->format = MIPI_DSI_FMT_RGB888;
	/* stock: non_burst_sync_pulse, h-sync-pulse=1, tx-eot-append, clock lane forced HS (= continuous clock) */
	dsi->mode_flags = MIPI_DSI_MODE_VIDEO | MIPI_DSI_MODE_VIDEO_HSE;
	if (dsi_mode == 0)
		dsi->mode_flags |= MIPI_DSI_MODE_VIDEO_SYNC_PULSE;
	else if (dsi_mode == 2)
		dsi->mode_flags |= MIPI_DSI_MODE_VIDEO_BURST;
	if (lpm)
		dsi->mode_flags |= MIPI_DSI_CLOCK_NON_CONTINUOUS;
	if (!eot)
		dsi->mode_flags |= MIPI_DSI_MODE_NO_EOT_PACKET;
	ctx->panel.prepare_prev_first = true;	/* stock lp11-init: DSI host up before the reset pulse */
	drm_panel_add(&ctx->panel);
	ret = mipi_dsi_attach(dsi);
	if (ret) {
		drm_panel_remove(&ctx->panel);
		return dev_err_probe(dev, ret, "dsi attach\n");
	}
	return 0;
}

static void a6l_epd_dsi_remove(struct mipi_dsi_device *dsi)
{
	struct a6l_epd_dsi *ctx = mipi_dsi_get_drvdata(dsi);

	mipi_dsi_detach(dsi);
	drm_panel_remove(&ctx->panel);
}

static const struct of_device_id a6l_epd_dsi_of_match[] = {
	{ .compatible = "hisense,a6l-epd-dsi" },
	{ .compatible = "toshiba,tc358762" },	/* V71 DT node; see header comment */
	{ }
};
MODULE_DEVICE_TABLE(of, a6l_epd_dsi_of_match);

static struct mipi_dsi_driver a6l_epd_dsi_driver = {
	.probe = a6l_epd_dsi_probe,
	.remove = a6l_epd_dsi_remove,
	.driver = { .name = "panel-a6l-epd-dsi", .of_match_table = a6l_epd_dsi_of_match },
};
module_mipi_dsi_driver(a6l_epd_dsi_driver);

MODULE_DESCRIPTION("Hisense A6L e-paper: unprogrammed DSI video sink with TPS65185 rail sequencing");
MODULE_LICENSE("GPL");
