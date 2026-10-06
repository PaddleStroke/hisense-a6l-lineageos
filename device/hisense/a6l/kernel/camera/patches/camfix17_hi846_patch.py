#!/usr/bin/env python3
# camfix17 hi846 (29 Sep 2026, docs/hi846-20260929.md round 2). Applied AFTER camfix16_hi846_patch.py.
#   usage: camfix17_hi846_patch.py <hi846.c>
# ROOT CAUSE found offline in t37 (camera16): hi846 never streamed. hi846_set_ctrl() stores the return value of
# pm_runtime_get_if_in_use() (= 1 while the sensor is powered) in `ret`; the EXPOSURE and VBLANK cases only OR i2c errors
# into it through hi846_write_reg_16(..., &ret), which skips when *err < 0 but leaves the 1 in place. So during
# hi846_start_streaming() -> __v4l2_ctrl_handler_setup() the VBLANK/EXPOSURE s_ctrl returns 1, start_streaming returns 1
# before MODE_SELECT (0x0a00) is written, hi846_set_stream() then calls hi846_stop_streaming() (the t37 "A6L_H846 OFF" line
# 1.1 s after each row start, 0a00=00, and no "A6L_H846 ON" line ever) and returns +1. camss only treats ret < 0 as an
# error, so STREAMON succeeds with a sensor in standby; call_s_stream() leaves s_stream_enabled=0 (ret != 0) and WARNs at
# stream-off (v4l2-subdev.c:484, the WARN seen in t36/t37). Same bug in the ROM hi846.ko (mainline + 4-lane patch).
# New runtime params (sysfs, no reload needed):
#   hi846.a6l_fix   (1)  1 = ret = 0 after a successful pm_runtime_get_if_in_use() (THE fix); 0 = mainline bug (neg. control)
#   hi846.a6l_rd    (camfix16, bit0 dump at stream on/off) + bit1: second dump 500 ms after stream on ("ON2")
#   hi846.a6l_rdx   up to 8 extra registers (u16 addresses) appended to every dump
#   hi846.a6l_mclk  (0)  if != 0: clk_set_rate(MCLK) at every power-on (e.g. 19200000 / 24000000); logged
#   hi846.a6l_stock (0)  bit0: after the init list also write the stock init entry 0x0076=0x0000 (mainline writes 0x0077)
# Always on (no knob): A6L_H846 START_FAIL / ctrl-setup return codes are logged.
import sys

f = sys.argv[1]
s = open(f).read()

def sub1(old, new):
    global s
    n = s.count(old)
    assert n == 1, f"anchor found {n}x: {old[:70]!r}"
    s = s.replace(old, new)

# params, before hi846_set_ctrl()
sub1("""static const struct hi846_datafmt hi846_colour_fmts[] = {
""", """/* A6L camfix17 (docs/hi846-20260929.md round 2) */
static int a6l_fix = 1;
module_param(a6l_fix, int, 0644);
MODULE_PARM_DESC(a6l_fix, "A6L camfix17: 1 = fix s_ctrl returning pm_runtime_get_if_in_use()==1 (sensor never streamed), 0 = mainline behaviour");
static unsigned int a6l_mclk;
module_param(a6l_mclk, uint, 0644);
MODULE_PARM_DESC(a6l_mclk, "A6L camfix17: MCLK rate set at every power-on (0 = leave the DT assigned rate)");
static int a6l_stock;
module_param(a6l_stock, int, 0644);
MODULE_PARM_DESC(a6l_stock, "A6L camfix17: bit0 = also write the stock init entry 0x0076=0x0000 after the init list");
static unsigned short a6l_rdx[8];
static int a6l_rdx_n;
module_param_array(a6l_rdx, ushort, &a6l_rdx_n, 0644);
MODULE_PARM_DESC(a6l_rdx, "A6L camfix17: up to 8 extra register addresses appended to the a6l_rd dumps");

static const struct hi846_datafmt hi846_colour_fmts[] = {
""")

# THE fix
sub1("""	ret = pm_runtime_get_if_in_use(&client->dev);
	if (!ret || ret == -EAGAIN)
		return 0;
""", """	ret = pm_runtime_get_if_in_use(&client->dev);
	if (!ret || ret == -EAGAIN)
		return 0;
	if (a6l_fix)
		ret = 0; /* A6L camfix17: do not leak the '1' of pm_runtime_get_if_in_use() into the s_ctrl result */
""")

# dump: extra regs + mclk rate
sub1("""	dev_info(&client->dev, "A6L_H846 %s lanes=%u mode=%ux%u link_freq=%lld regs%s\\n", tag,
		 hi846->nr_lanes, hi846->cur_mode->width, hi846->cur_mode->height,
		 (long long)hi846_link_freqs[hi846->cur_mode->link_freq_index], buf);
""", """	dev_info(&client->dev, "A6L_H846 %s lanes=%u mode=%ux%u link_freq=%lld regs%s\\n", tag,
		 hi846->nr_lanes, hi846->cur_mode->width, hi846->cur_mode->height,
		 (long long)hi846_link_freqs[hi846->cur_mode->link_freq_index], buf);
	n = 0;
	buf[0] = 0;
	for (i = 0; i < a6l_rdx_n && i < ARRAY_SIZE(a6l_rdx); i++) {
		if (hi846_read_reg(hi846, a6l_rdx[i], &v))
			n += scnprintf(buf + n, sizeof(buf) - n, " %04x=??", a6l_rdx[i]);
		else
			n += scnprintf(buf + n, sizeof(buf) - n, " %04x=%02x", a6l_rdx[i], v);
	}
	dev_info(&client->dev, "A6L_H846 %s mclk=%lu fix=%d stock=%d extra%s\\n", tag,
		 clk_get_rate(hi846->clock), a6l_fix, a6l_stock, n ? buf : " none");
""")

# stock 0x0076
sub1("""	if (ret) {
		dev_err(&client->dev, "failed to set plls: %d\\n", ret);
		return ret;
	}
""", """	if (ret) {
		dev_err(&client->dev, "failed to set plls: %d\\n", ret);
		return ret;
	}
	if (a6l_stock & 1) /* A6L camfix17: stock hi846_init writes 0x0076=0x0000 where mainline writes 0x0077 */
		hi846_write_reg_16(hi846, 0x0076, 0x0000, &ret);
	if (ret)
		return ret;
""")

# log ctrl setup result
sub1("""	ret = __v4l2_ctrl_handler_setup(hi846->sd.ctrl_handler);
	if (ret)
		return ret;
""", """	ret = __v4l2_ctrl_handler_setup(hi846->sd.ctrl_handler);
	if (ret) {
		dev_err(&client->dev, "A6L_H846 ctrl_handler_setup returned %d (a6l_fix %d): MODE_SELECT not written\\n",
			ret, a6l_fix);
		return ret;
	}
""")

# second dump
sub1("""	if (a6l_rd) {
		usleep_range(20000, 21000);
		a6l_h846_dump(hi846, "ON");
	}
""", """	if (a6l_rd) {
		usleep_range(20000, 21000);
		a6l_h846_dump(hi846, "ON");
		if (a6l_rd & 2) {
			msleep(500);
			a6l_h846_dump(hi846, "ON2");
		}
	}
""")

# start failure log
sub1("""		ret = hi846_start_streaming(hi846);
	}
""", """		ret = hi846_start_streaming(hi846);
		if (ret)
			dev_err(&client->dev, "A6L_H846 START_FAIL ret=%d (stream stopped again)\\n", ret);
	}
""")

# MCLK rate
sub1("""	ret = clk_prepare_enable(hi846->clock);
	if (ret < 0)
		goto err_reg;
""", """	if (a6l_mclk) { /* A6L camfix17 */
		int r = clk_set_rate(hi846->clock, a6l_mclk);

		pr_info("hi846: A6L_H846 MCLK set %u -> %lu (rc %d)\\n", a6l_mclk, clk_get_rate(hi846->clock), r);
	}

	ret = clk_prepare_enable(hi846->clock);
	if (ret < 0)
		goto err_reg;
""")

if "#include <linux/delay.h>" not in s:
    sub1("#include <linux/pm_runtime.h>\n", "#include <linux/delay.h>\n#include <linux/pm_runtime.h>\n")
open(f, "w").write(s)
print("CAMFIX17_HI846_PATCH_OK")
