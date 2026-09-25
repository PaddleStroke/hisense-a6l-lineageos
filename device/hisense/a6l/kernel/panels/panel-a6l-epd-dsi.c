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
 * XON (gpio61) is not in the V71 DT: hold it high from userspace (a6l_gpio_hold / a6l_epdd) for now.
 *
 * V73 (23 Sep 2026): the bring-up that made the panel draw in r116-r138 is done here, in .enable (video running):
 *   1. DSI1 PHY reset (DSI1 ctrl 0x12c) + full reprogram of the PHY with the values it had just before (LDO, CMN,
 *      PLL_CNTRL=0, lanes, PLL, CMN_CTRL_1 pulse, CTRL_0=0xff, PLL_CNTRL=1) -> the TC358767 starts answering on I2C;
 *   2. INTF2 timing engine paused while the 25-entry stock table is written (with readbacks), then resumed;
 *   3. check: bridge still ACKs and the INTF2 frame counter moves (r113 failure = stream stuck); retry otherwise.
 * The e-paper rails are no longer tied to prepare: userspace switches them per update through the "epd_power" sysfs
 * attribute (1 = VCOM 2.40 V written, VPOS/VNEG on, 20 ms, VCOM on = stock tps65185_active_mode order; 0 = VCOM off,
 * VPOS/VNEG off = standby). .disable/.unprepare force them off.
 */
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/io.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/regulator/consumer.h>
#include <drm/drm_mipi_dsi.h>
#include <drm/drm_modes.h>
#include <drm/drm_panel.h>
#include <drm/drm_probe_helper.h>

static bool bringup = true;	/* V73: PHY reset+reprogram + INTF2-paused table in .enable (the r116-r138 recipe) */
module_param(bringup, bool, 0644);
static unsigned int retries = 3;
module_param(retries, uint, 0644);
static unsigned int phy_write_delay_us;	/* 0 = back-to-back (userspace spawned one process per write) */
module_param(phy_write_delay_us, uint, 0644);
static unsigned int vcom_mv = 2400;	/* panel NOR: VCOM -2.40 V */
module_param(vcom_mv, uint, 0644);
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

struct a6l_epd_dsi {
	struct drm_panel panel;
	struct mipi_dsi_device *dsi;
	struct gpio_desc *reset, *xon;
	struct regulator *vddc, *v3p3, *vposneg, *vcom;
	bool hv_on;
	bool bridge_ok;
	bool prepared, enabled;
	struct mutex lock;
	char status[160];
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


/* ---- V73 bring-up: exact kernel port of the userspace recipe (t116..t137) ---- */
#define A6L_PHY1_BASE	0x0c996400	/* DSI1 PHY: CMN 0x400.., lanes 0x500.., PLL 0x800.. (offsets from DSI1 base 0x0c996000) */
#define A6L_PHY1_WORDS	322		/* 0x400 .. 0x904 */
#define A6L_DSI1_PHY_RESET 0x0c99612c
#define A6L_INTF2_BASE	0x0c96c000	/* INTF_2: +0x000 TIMING_ENGINE_EN, +0x0ac FRAME_COUNT */
#define A6L_INTF_FRAME_COUNT 0x0ac

static bool a6l_bridge_ack(struct i2c_adapter *a)
{
	u32 v;

	return !a6l_bridge_rd(a, 0x04a0, &v);
}

static void a6l_phy_wr(void __iomem *phy, unsigned int off, u32 val)
{
	writel(val, phy + off - 0x400);
	if (phy_write_delay_us)
		udelay(phy_write_delay_us);
}

/* restore() of t116: reset pulse, LDO, CMN (minus CTRL_0/CMN_CTRL_1/PLL_CNTRL/LDO), PLL_CNTRL=0, lanes, PLL, start */
static void a6l_phy_reprogram(void __iomem *phy, void __iomem *rst, const u32 *save)
{
	unsigned int i, off;

	writel(1, rst);
	usleep_range(2000, 2500);
	writel(0, rst);
	usleep_range(2000, 2500);
	a6l_phy_wr(phy, 0x44c, save[(0x44c - 0x400) / 4]);
	for (off = 0x410; off < 0x500; off += 4)
		if (off != 0x41c && off != 0x424 && off != 0x448 && off != 0x44c)
			a6l_phy_wr(phy, off, save[(off - 0x400) / 4]);
	a6l_phy_wr(phy, 0x448, 0);
	for (off = 0x500; off < 0x780; off += 4)
		a6l_phy_wr(phy, off, save[(off - 0x400) / 4]);
	for (i = (0x800 - 0x400) / 4; i < A6L_PHY1_WORDS; i++)
		a6l_phy_wr(phy, 0x400 + 4 * i, save[i]);
	a6l_phy_wr(phy, 0x424, 2);
	usleep_range(2000, 2500);
	a6l_phy_wr(phy, 0x424, 0);
	a6l_phy_wr(phy, 0x41c, 0xff);
	a6l_phy_wr(phy, 0x448, 1);
	msleep(20);
}

static int a6l_bringup(struct a6l_epd_dsi *ctx)
{
	struct device *dev = ctx->panel.dev;
	void __iomem *phy, *rst, *intf;
	struct device_node *np;
	struct i2c_adapter *a;
	unsigned int attempt, i, ok, fc_moves;
	u32 *save, en, f0, f1;
	int ret = -EIO;

	np = of_find_node_by_path(bridge_bus);
	a = np ? of_get_i2c_adapter_by_node(np) : NULL;
	of_node_put(np);
	if (!a)
		return -ENODEV;
	save = kmalloc_array(A6L_PHY1_WORDS, sizeof(*save), GFP_KERNEL);
	phy = ioremap(A6L_PHY1_BASE, A6L_PHY1_WORDS * 4);
	rst = ioremap(A6L_DSI1_PHY_RESET, 4);
	intf = ioremap(A6L_INTF2_BASE, 0x100);
	if (!save || !phy || !rst || !intf) {
		ret = -ENOMEM;
		goto out;
	}
	for (i = 0; i < A6L_PHY1_WORDS; i++)
		save[i] = readl(phy + 4 * i);
	f0 = readl(intf + A6L_INTF_FRAME_COUNT);
	msleep(60);
	f1 = readl(intf + A6L_INTF_FRAME_COUNT);
	fc_moves = f1 != f0;	/* if the counter never moves the stream check is skipped */
	dev_info(dev, "bringup: INTF2 en=%08x frame counter %s (%u->%u), PHY CFG0=%08x PLL_CNTRL=%08x\n",
		 readl(intf), fc_moves ? "runs" : "static", f0, f1, save[(0x410 - 0x400) / 4], save[(0x448 - 0x400) / 4]);
	for (attempt = 1; attempt <= max(retries, 1u); attempt++) {
		a6l_phy_reprogram(phy, rst, save);
		if (!a6l_bridge_ack(a)) {
			dev_warn(dev, "bringup %u: bridge NAK after PHY reset+reprogram\n", attempt);
			continue;
		}
		en = readl(intf);
		writel(0, intf);
		msleep(30);
		for (i = 0, ok = 0; i < ARRAY_SIZE(a6l_tc358767_table); i++) {
			u32 back;

			if (!a6l_bridge_wr(a, a6l_tc358767_table[i].reg, a6l_tc358767_table[i].val))
				ok++;
			a6l_bridge_rd(a, a6l_tc358767_table[i].reg, &back);	/* as a6l_dsi2dpi_init: readback paces the table */
		}
		writel(en ? en : 1, intf);
		msleep(30);
		f0 = readl(intf + A6L_INTF_FRAME_COUNT);
		msleep(60);
		f1 = readl(intf + A6L_INTF_FRAME_COUNT);
		ret = (ok == ARRAY_SIZE(a6l_tc358767_table) && a6l_bridge_ack(a) && (!fc_moves || f1 != f0)) ? 0 : -EIO;
		scnprintf(ctx->status, sizeof(ctx->status), "attempt=%u table=%u/%zu ack=%d stream=%s(%u->%u) %s", attempt, ok,
			  ARRAY_SIZE(a6l_tc358767_table), a6l_bridge_ack(a), fc_moves ? (f1 != f0 ? "runs" : "STUCK") : "unchecked",
			  f0, f1, ret ? "FAIL" : "OK");
		dev_info(dev, "bringup: %s\n", ctx->status);
		if (!ret)
			break;
	}
out:
	if (intf)
		iounmap(intf);
	if (rst)
		iounmap(rst);
	if (phy)
		iounmap(phy);
	kfree(save);
	i2c_put_adapter(a);
	return ret;
}

/* VCOM register written directly (bypasses the regmap cache, which cannot see a chip-side reset): 10 mV units, 9 bits */
static void a6l_vcom_write(struct device *dev)
{
	struct device_node *np = of_find_node_by_path(bridge_bus);
	struct i2c_adapter *a = np ? of_get_i2c_adapter_by_node(np) : NULL;
	unsigned int sel = vcom_mv / 10;
	u8 w1[2] = { 0x03, sel & 0xff }, r4 = 0x04, v4 = 0, w2[2], back = 0;
	struct i2c_msg rd[2] = { { 0x68, 0, 1, &r4 }, { 0x68, I2C_M_RD, 1, &v4 } };
	struct i2c_msg m1 = { 0x68, 0, 2, w1 }, m2 = { 0x68, 0, 2, w2 };
	u8 r3 = 0x03;
	struct i2c_msg rb[2] = { { 0x68, 0, 1, &r3 }, { 0x68, I2C_M_RD, 1, &back } };

	of_node_put(np);
	if (!a)
		return;
	if (i2c_transfer(a, rd, 2) == 2) {
		w2[0] = 0x04;
		w2[1] = (v4 & ~1) | ((sel >> 8) & 1);
		i2c_transfer(a, &m2, 1);
	}
	i2c_transfer(a, &m1, 1);
	i2c_transfer(a, rb, 2);
	dev_info(dev, "VCOM set %u mV (VCOM1 reads %02x)\n", vcom_mv, back);
	i2c_put_adapter(a);
}

static int a6l_rails(struct a6l_epd_dsi *ctx, bool on)
{
	struct device *dev = ctx->panel.dev;
	int ret;

	if (on == ctx->hv_on)
		return 0;
	if (!on) {
		regulator_disable(ctx->vcom);
		regulator_disable(ctx->vposneg);
		ctx->hv_on = false;
		dev_info(dev, "e-paper rails OFF\n");
		return 0;
	}
	if (!ctx->enabled || !ctx->bridge_ok)
		return -EAGAIN;
	a6l_vcom_write(dev);
	ret = regulator_enable(ctx->vposneg);
	if (ret)
		return ret;
	msleep(20);
	ret = regulator_enable(ctx->vcom);
	if (ret) {
		regulator_disable(ctx->vposneg);
		return ret;
	}
	ctx->hv_on = true;
	dev_info(dev, "e-paper rails ON\n");
	return 0;
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
	ctx->bridge_ok = false;
	if (!bringup)
		ctx->bridge_ok = !a6l_bridge_init(panel->dev, "prepare");	/* link is in LP-11 here (prepare_prev_first) */
	mutex_lock(&ctx->lock);
	ctx->prepared = true;
	mutex_unlock(&ctx->lock);
	return 0;
err_vddc:
	regulator_disable(ctx->vddc);
	return ret;
}

static int a6l_epd_dsi_disable(struct drm_panel *panel)
{
	struct a6l_epd_dsi *ctx = to_ctx(panel);

	mutex_lock(&ctx->lock);
	a6l_rails(ctx, false);	/* video still scanning here: rails off before the stream stops (r118/r126) */
	ctx->enabled = false;
	mutex_unlock(&ctx->lock);
	return 0;
}

static int a6l_epd_dsi_unprepare(struct drm_panel *panel)
{
	struct a6l_epd_dsi *ctx = to_ctx(panel);

	mutex_lock(&ctx->lock);
	a6l_rails(ctx, false);
	ctx->prepared = false;
	ctx->bridge_ok = false;
	mutex_unlock(&ctx->lock);
	gpiod_set_value_cansleep(ctx->xon, 0);
	gpiod_set_value_cansleep(ctx->reset, 1);
	regulator_disable(ctx->v3p3);
	regulator_disable(ctx->vddc);
	return 0;
}

/* Video runs here (DPU encoder enabled before the panel bridge's enable). */
static int a6l_epd_dsi_enable(struct drm_panel *panel)
{
	struct a6l_epd_dsi *ctx = to_ctx(panel);

	mutex_lock(&ctx->lock);
	if (bringup && bridge)
		ctx->bridge_ok = !a6l_bringup(ctx);
	else if (!ctx->bridge_ok)
		ctx->bridge_ok = !a6l_bridge_init(panel->dev, "enable");
	ctx->enabled = true;
	mutex_unlock(&ctx->lock);
	return 0;
}

static ssize_t epd_power_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct a6l_epd_dsi *ctx = mipi_dsi_get_drvdata(to_mipi_dsi_device(dev));

	return sysfs_emit(buf, "%d\n", ctx->hv_on);
}

static ssize_t epd_power_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t len)
{
	struct a6l_epd_dsi *ctx = mipi_dsi_get_drvdata(to_mipi_dsi_device(dev));
	bool on;
	int ret = kstrtobool(buf, &on);

	if (ret)
		return ret;
	mutex_lock(&ctx->lock);
	ret = a6l_rails(ctx, on);
	mutex_unlock(&ctx->lock);
	return ret ? ret : len;
}
static DEVICE_ATTR_RW(epd_power);

static ssize_t bringup_status_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct a6l_epd_dsi *ctx = mipi_dsi_get_drvdata(to_mipi_dsi_device(dev));

	return sysfs_emit(buf, "prepared=%d enabled=%d bridge_ok=%d rails=%d %s\n", ctx->prepared, ctx->enabled,
			  ctx->bridge_ok, ctx->hv_on, ctx->status);
}
static DEVICE_ATTR_RO(bringup_status);

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
	.disable = a6l_epd_dsi_disable,
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
	mutex_init(&ctx->lock);
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
	device_create_file(dev, &dev_attr_epd_power);
	device_create_file(dev, &dev_attr_bringup_status);
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
	device_remove_file(&dsi->dev, &dev_attr_bringup_status);
	device_remove_file(&dsi->dev, &dev_attr_epd_power);
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

MODULE_DESCRIPTION("Hisense A6L e-paper: DSI1 -> TC358767 bring-up (V73) with TPS65185 rail control");
MODULE_LICENSE("GPL");
