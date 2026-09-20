// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 FIXME
// Generated with linux-mdss-dsi-panel-driver-generator from vendor device tree:
//   Copyright (c) 2013, The Linux Foundation. All rights reserved. (FIXME)

#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>

#include <drm/drm_mipi_dsi.h>
#include <drm/drm_modes.h>
#include <drm/drm_panel.h>
#include <drm/drm_probe_helper.h>

struct epd_eink {
	struct drm_panel panel;
	struct mipi_dsi_device *dsi;
	struct gpio_desc *reset_gpio;
};

static inline struct epd_eink *to_epd_eink(struct drm_panel *panel)
{
	return container_of_const(panel, struct epd_eink, panel);
}

static void epd_eink_reset(struct epd_eink *ctx)
{
	gpiod_set_value_cansleep(ctx->reset_gpio, 1);
	usleep_range(10000, 11000);
	gpiod_set_value_cansleep(ctx->reset_gpio, 0);
	usleep_range(10000, 11000);
}

static int epd_eink_on(struct epd_eink *ctx)
{
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = ctx->dsi };

	return dsi_ctx.accum_err;
}

static int epd_eink_off(struct epd_eink *ctx)
{
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = ctx->dsi };

	return dsi_ctx.accum_err;
}

static int epd_eink_prepare(struct drm_panel *panel)
{
	struct epd_eink *ctx = to_epd_eink(panel);
	struct device *dev = &ctx->dsi->dev;
	int ret;

	epd_eink_reset(ctx);

	ret = epd_eink_on(ctx);
	if (ret < 0) {
		dev_err(dev, "Failed to initialize panel: %d\n", ret);
		gpiod_set_value_cansleep(ctx->reset_gpio, 1);
		return ret;
	}

	return 0;
}

static int epd_eink_unprepare(struct drm_panel *panel)
{
	struct epd_eink *ctx = to_epd_eink(panel);
	struct device *dev = &ctx->dsi->dev;
	int ret;

	ret = epd_eink_off(ctx);
	if (ret < 0)
		dev_err(dev, "Failed to un-initialize panel: %d\n", ret);

	gpiod_set_value_cansleep(ctx->reset_gpio, 1);

	return 0;
}

static const struct drm_display_mode epd_eink_mode = {
	.clock = (384 + 126 + 6 + 125) * (725 + 4 + 2 + 4) * 85 / 1000,
	.hdisplay = 384,
	.hsync_start = 384 + 126,
	.hsync_end = 384 + 126 + 6,
	.htotal = 384 + 126 + 6 + 125,
	.vdisplay = 725,
	.vsync_start = 725 + 4,
	.vsync_end = 725 + 4 + 2,
	.vtotal = 725 + 4 + 2 + 4,
	.width_mm = 68,
	.height_mm = 121,
	.type = DRM_MODE_TYPE_DRIVER,
};

static int epd_eink_get_modes(struct drm_panel *panel,
			      struct drm_connector *connector)
{
	return drm_connector_helper_get_modes_fixed(connector, &epd_eink_mode);
}

static const struct drm_panel_funcs epd_eink_panel_funcs = {
	.prepare = epd_eink_prepare,
	.unprepare = epd_eink_unprepare,
	.get_modes = epd_eink_get_modes,
};

static int epd_eink_probe(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	struct epd_eink *ctx;
	int ret;

	ctx = devm_drm_panel_alloc(dev, struct epd_eink, panel,
				   &epd_eink_panel_funcs,
				   DRM_MODE_CONNECTOR_DSI);
	if (IS_ERR(ctx))
		return PTR_ERR(ctx);

	ctx->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(ctx->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(ctx->reset_gpio),
				     "Failed to get reset-gpios\n");

	ctx->dsi = dsi;
	mipi_dsi_set_drvdata(dsi, ctx);

	dsi->lanes = 2;
	dsi->format = MIPI_DSI_FMT_RGB888;
	dsi->mode_flags = MIPI_DSI_MODE_VIDEO | MIPI_DSI_MODE_VIDEO_SYNC_PULSE |
			  MIPI_DSI_MODE_VIDEO_HSE | MIPI_DSI_MODE_LPM;

	ctx->panel.prepare_prev_first = true;

	ret = drm_panel_of_backlight(&ctx->panel);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to get backlight\n");

	drm_panel_add(&ctx->panel);

	ret = mipi_dsi_attach(dsi);
	if (ret < 0) {
		drm_panel_remove(&ctx->panel);
		return dev_err_probe(dev, ret, "Failed to attach to DSI host\n");
	}

	return 0;
}

static void epd_eink_remove(struct mipi_dsi_device *dsi)
{
	struct epd_eink *ctx = mipi_dsi_get_drvdata(dsi);
	int ret;

	ret = mipi_dsi_detach(dsi);
	if (ret < 0)
		dev_err(&dsi->dev, "Failed to detach from DSI host: %d\n", ret);

	drm_panel_remove(&ctx->panel);
}

static const struct of_device_id epd_eink_of_match[] = {
	{ .compatible = "epd,eink" }, // FIXME
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, epd_eink_of_match);

static struct mipi_dsi_driver epd_eink_driver = {
	.probe = epd_eink_probe,
	.remove = epd_eink_remove,
	.driver = {
		.name = "panel-epd-eink",
		.of_match_table = epd_eink_of_match,
	},
};
module_mipi_dsi_driver(epd_eink_driver);

MODULE_AUTHOR("linux-mdss-dsi-panel-driver-generator <fix@me>"); // FIXME
MODULE_DESCRIPTION("DRM driver for eink epd qhd");
MODULE_LICENSE("GPL");
