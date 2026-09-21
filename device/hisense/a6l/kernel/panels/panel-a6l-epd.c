// SPDX-License-Identifier: GPL-2.0-only
/*
 * Hisense A6L rear e-paper "panel" as seen by DRM: a 384x725@85 RGB888 DPI sink behind the TC358762.
 * Each scanned-out frame is ONE waveform drive frame produced by the software TCON (docs/eink-swtcon-abi-20260919.md);
 * DRM never sees the 720x1440 grey image. This driver only sequences the TPS65185 rails around the video stream:
 *   prepare:   v3p3 -> (hv) vposneg (driver waits for PWR_GOOD) -> 20 ms -> vcom
 *   unprepare: vcom -> vposneg -> v3p3
 * hv=0 (default) keeps the +-15 V rails and VCOM OFF: transport bring-up without driving the ink.
 * hv=1 must only be used by the frame daemon, which guarantees that the last frames of an update are "no drive".
 */
#include <linux/delay.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/regulator/consumer.h>
#include <drm/drm_modes.h>
#include <drm/drm_panel.h>
#include <drm/drm_probe_helper.h>
#include <linux/media-bus-format.h>
#include <drm/drm_connector.h>

static bool hv;
module_param(hv, bool, 0644);
MODULE_PARM_DESC(hv, "enable the e-paper high-voltage rails and VCOM while the output is enabled");

struct a6l_epd {
	struct drm_panel panel;
	struct regulator *v3p3, *vposneg, *vcom;
	bool hv_on;
};

static inline struct a6l_epd *to_a6l_epd(struct drm_panel *panel)
{
	return container_of(panel, struct a6l_epd, panel);
}

static int a6l_epd_prepare(struct drm_panel *panel)
{
	struct a6l_epd *ctx = to_a6l_epd(panel);
	int ret;

	ret = regulator_enable(ctx->v3p3);
	if (ret)
		return ret;
	if (!hv)
		return 0;
	ret = regulator_enable(ctx->vposneg);
	if (ret)
		goto err_v3p3;
	msleep(20);
	ret = regulator_enable(ctx->vcom);
	if (ret)
		goto err_hv;
	ctx->hv_on = true;
	dev_info(panel->dev, "e-paper rails ON\n");
	return 0;
err_hv:
	regulator_disable(ctx->vposneg);
err_v3p3:
	regulator_disable(ctx->v3p3);
	return ret;
}

static int a6l_epd_unprepare(struct drm_panel *panel)
{
	struct a6l_epd *ctx = to_a6l_epd(panel);

	if (ctx->hv_on) {
		regulator_disable(ctx->vcom);
		regulator_disable(ctx->vposneg);
		ctx->hv_on = false;
		dev_info(panel->dev, "e-paper rails OFF\n");
	}
	regulator_disable(ctx->v3p3);
	return 0;
}

/* stock TC358762 + MDSS timing, 85 Hz (docs/eink-transport-tc358762-20260920.md) */
static const struct drm_display_mode a6l_epd_mode = {
	.clock = 40046,
	.hdisplay = 384, .hsync_start = 384 + 126, .hsync_end = 384 + 126 + 6, .htotal = 384 + 126 + 6 + 125,
	.vdisplay = 725, .vsync_start = 725 + 4, .vsync_end = 725 + 4 + 2, .vtotal = 725 + 4 + 2 + 4,
	.width_mm = 65, .height_mm = 130,
	.type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED,
};

static int a6l_epd_get_modes(struct drm_panel *panel, struct drm_connector *connector)
{
	static const u32 fmt = MEDIA_BUS_FMT_RGB888_1X24;

	drm_display_info_set_bus_formats(&connector->display_info, &fmt, 1);
	return drm_connector_helper_get_modes_fixed(connector, &a6l_epd_mode);
}

static const struct drm_panel_funcs a6l_epd_funcs = {
	.prepare = a6l_epd_prepare,
	.unprepare = a6l_epd_unprepare,
	.get_modes = a6l_epd_get_modes,
};

static int a6l_epd_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct a6l_epd *ctx;

	ctx = devm_drm_panel_alloc(dev, struct a6l_epd, panel, &a6l_epd_funcs, DRM_MODE_CONNECTOR_DPI);
	if (IS_ERR(ctx))
		return PTR_ERR(ctx);
	ctx->v3p3 = devm_regulator_get(dev, "power");
	if (IS_ERR(ctx->v3p3))
		return dev_err_probe(dev, PTR_ERR(ctx->v3p3), "power supply\n");
	ctx->vposneg = devm_regulator_get(dev, "vposneg");
	if (IS_ERR(ctx->vposneg))
		return dev_err_probe(dev, PTR_ERR(ctx->vposneg), "vposneg supply\n");
	ctx->vcom = devm_regulator_get(dev, "vcom");
	if (IS_ERR(ctx->vcom))
		return dev_err_probe(dev, PTR_ERR(ctx->vcom), "vcom supply\n");
	platform_set_drvdata(pdev, ctx);
	drm_panel_add(&ctx->panel);
	return 0;
}

static void a6l_epd_remove(struct platform_device *pdev)
{
	struct a6l_epd *ctx = platform_get_drvdata(pdev);

	drm_panel_remove(&ctx->panel);
}

static const struct of_device_id a6l_epd_of_match[] = {
	{ .compatible = "hisense,a6l-epd-panel" },
	{ }
};
MODULE_DEVICE_TABLE(of, a6l_epd_of_match);

static struct platform_driver a6l_epd_driver = {
	.probe = a6l_epd_probe,
	.remove = a6l_epd_remove,
	.driver = { .name = "panel-a6l-epd", .of_match_table = a6l_epd_of_match },
};
module_platform_driver(a6l_epd_driver);

MODULE_DESCRIPTION("Hisense A6L e-paper DPI sink with TPS65185 rail sequencing");
MODULE_LICENSE("GPL");
