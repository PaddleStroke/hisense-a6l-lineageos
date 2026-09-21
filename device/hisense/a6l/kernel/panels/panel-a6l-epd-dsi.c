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
MODULE_PARM_DESC(hv, "enable the e-paper high-voltage rails and VCOM while the output is enabled");

struct a6l_epd_dsi {
	struct drm_panel panel;
	struct mipi_dsi_device *dsi;
	struct gpio_desc *reset;
	struct regulator *vddc, *v3p3, *vposneg, *vcom;
	bool hv_on;
};

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
	gpiod_set_value_cansleep(ctx->reset, 1);
	regulator_disable(ctx->v3p3);
	regulator_disable(ctx->vddc);
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
	dsi->lanes = 2;
	dsi->format = MIPI_DSI_FMT_RGB888;
	/* stock: non_burst_sync_pulse, h-sync-pulse=1, tx-eot-append, clock lane forced HS (= continuous clock) */
	dsi->mode_flags = MIPI_DSI_MODE_VIDEO | MIPI_DSI_MODE_VIDEO_SYNC_PULSE | MIPI_DSI_MODE_VIDEO_HSE;
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
