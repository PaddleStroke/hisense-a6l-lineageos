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

/* A6L bring-up experiment (22 Sep 2026): skip_init=1 never touches reset and sends no DCS commands, so the panel
 * keeps the state the bootloader programmed. Black with init + picture with skip_init => init/reset sequence is wrong;
 * black in both => the video stream (clocks/timing) is wrong. */
static bool skip_init;
module_param(skip_init, bool, 0444);

struct ft8719_tianma_1080x2340 {
	struct drm_panel panel;
	struct mipi_dsi_device *dsi;
	struct gpio_desc *reset_gpio;
};

static inline
struct ft8719_tianma_1080x2340 *to_ft8719_tianma_1080x2340(struct drm_panel *panel)
{
	return container_of_const(panel, struct ft8719_tianma_1080x2340, panel);
}

static void ft8719_tianma_1080x2340_reset(struct ft8719_tianma_1080x2340 *ctx)
{
	gpiod_set_value_cansleep(ctx->reset_gpio, 0);
	usleep_range(10000, 11000);
	gpiod_set_value_cansleep(ctx->reset_gpio, 1);
	usleep_range(10000, 11000);
	gpiod_set_value_cansleep(ctx->reset_gpio, 0);
	usleep_range(10000, 11000);
}

static int ft8719_tianma_1080x2340_on(struct ft8719_tianma_1080x2340 *ctx)
{
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = ctx->dsi };

	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x00, 0x00);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xff, 0x87, 0x19, 0x01);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x00, 0x80);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xff, 0x87, 0x19);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x00, 0x90);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xc0,
				     0x00, 0x7b, 0x00, 0x6b, 0x00, 0x10);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x00, 0xb0);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xc0,
				     0x00, 0x7b, 0x01, 0xfa, 0x10);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x00, 0xc1);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xc0,
				     0x00, 0xb4, 0x00, 0x8c, 0x00, 0x78, 0x00,
				     0xd2);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x00, 0x90);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xcf,
				     0x14, 0x00, 0x06, 0x00, 0x06, 0x14);
	mipi_dsi_dcs_set_tear_on_multi(&dsi_ctx, MIPI_DSI_DCS_TEAR_MODE_VBLANK);
	mipi_dsi_dcs_exit_sleep_mode_multi(&dsi_ctx);
	mipi_dsi_msleep(&dsi_ctx, 120);
	mipi_dsi_dcs_set_display_on_multi(&dsi_ctx);

	return dsi_ctx.accum_err;
}

static int ft8719_tianma_1080x2340_off(struct ft8719_tianma_1080x2340 *ctx)
{
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = ctx->dsi };

	mipi_dsi_dcs_set_display_off_multi(&dsi_ctx);
	mipi_dsi_msleep(&dsi_ctx, 20);
	mipi_dsi_dcs_enter_sleep_mode_multi(&dsi_ctx);
	mipi_dsi_msleep(&dsi_ctx, 120);

	return dsi_ctx.accum_err;
}

static int ft8719_tianma_1080x2340_prepare(struct drm_panel *panel)
{
	struct ft8719_tianma_1080x2340 *ctx = to_ft8719_tianma_1080x2340(panel);
	struct device *dev = &ctx->dsi->dev;
	int ret;

	if (skip_init)
		return 0;

	ft8719_tianma_1080x2340_reset(ctx);

	ret = ft8719_tianma_1080x2340_on(ctx);
	if (ret < 0) {
		dev_err(dev, "Failed to initialize panel: %d\n", ret);
		gpiod_set_value_cansleep(ctx->reset_gpio, 1);
		return ret;
	}

	return 0;
}

static int ft8719_tianma_1080x2340_unprepare(struct drm_panel *panel)
{
	struct ft8719_tianma_1080x2340 *ctx = to_ft8719_tianma_1080x2340(panel);
	struct device *dev = &ctx->dsi->dev;
	int ret;

	if (skip_init)
		return 0;

	ret = ft8719_tianma_1080x2340_off(ctx);
	if (ret < 0)
		dev_err(dev, "Failed to un-initialize panel: %d\n", ret);

	gpiod_set_value_cansleep(ctx->reset_gpio, 1);

	return 0;
}

static const struct drm_display_mode ft8719_tianma_1080x2340_mode = {
	.clock = (1080 + 36 + 4 + 32) * (2340 + 120 + 4 + 12) * 60 / 1000,
	.hdisplay = 1080,
	.hsync_start = 1080 + 36,
	.hsync_end = 1080 + 36 + 4,
	.htotal = 1080 + 36 + 4 + 32,
	.vdisplay = 2340,
	.vsync_start = 2340 + 120,
	.vsync_end = 2340 + 120 + 4,
	.vtotal = 2340 + 120 + 4 + 12,
	.width_mm = 69,
	.height_mm = 151,
	.type = DRM_MODE_TYPE_DRIVER,
};

static int ft8719_tianma_1080x2340_get_modes(struct drm_panel *panel,
					     struct drm_connector *connector)
{
	return drm_connector_helper_get_modes_fixed(connector, &ft8719_tianma_1080x2340_mode);
}

static const struct drm_panel_funcs ft8719_tianma_1080x2340_panel_funcs = {
	.prepare = ft8719_tianma_1080x2340_prepare,
	.unprepare = ft8719_tianma_1080x2340_unprepare,
	.get_modes = ft8719_tianma_1080x2340_get_modes,
};

static int ft8719_tianma_1080x2340_probe(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	struct ft8719_tianma_1080x2340 *ctx;
	int ret;

	ctx = devm_drm_panel_alloc(dev, struct ft8719_tianma_1080x2340, panel,
				   &ft8719_tianma_1080x2340_panel_funcs,
				   DRM_MODE_CONNECTOR_DSI);
	if (IS_ERR(ctx))
		return PTR_ERR(ctx);

	ctx->reset_gpio = devm_gpiod_get(dev, "reset", skip_init ? GPIOD_OUT_LOW : GPIOD_OUT_HIGH);
	if (IS_ERR(ctx->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(ctx->reset_gpio),
				     "Failed to get reset-gpios\n");

	ctx->dsi = dsi;
	mipi_dsi_set_drvdata(dsi, ctx);

	dsi->lanes = 4;
	dsi->format = MIPI_DSI_FMT_RGB888;
	dsi->mode_flags = MIPI_DSI_MODE_VIDEO | MIPI_DSI_MODE_VIDEO_BURST |
			  MIPI_DSI_CLOCK_NON_CONTINUOUS | MIPI_DSI_MODE_LPM;

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

static void ft8719_tianma_1080x2340_remove(struct mipi_dsi_device *dsi)
{
	struct ft8719_tianma_1080x2340 *ctx = mipi_dsi_get_drvdata(dsi);
	int ret;

	ret = mipi_dsi_detach(dsi);
	if (ret < 0)
		dev_err(&dsi->dev, "Failed to detach from DSI host: %d\n", ret);

	drm_panel_remove(&ctx->panel);
}

static const struct of_device_id ft8719_tianma_1080x2340_of_match[] = {
	{ .compatible = "mdss,ft8719-tianma-1080x2340" }, // FIXME
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, ft8719_tianma_1080x2340_of_match);

static struct mipi_dsi_driver ft8719_tianma_1080x2340_driver = {
	.probe = ft8719_tianma_1080x2340_probe,
	.remove = ft8719_tianma_1080x2340_remove,
	.driver = {
		.name = "panel-ft8719-tianma-1080x2340",
		.of_match_table = ft8719_tianma_1080x2340_of_match,
	},
};
module_mipi_dsi_driver(ft8719_tianma_1080x2340_driver);

MODULE_AUTHOR("linux-mdss-dsi-panel-driver-generator <fix@me>"); // FIXME
MODULE_DESCRIPTION("DRM driver for ft8719 tianma 1080x2340 video");
MODULE_LICENSE("GPL");
