#!/usr/bin/env python3
# camfix16 hi846 diagnostics (29 Sep 2026, docs/hi846-20260929.md). Applies on top of hi846-4lane-default.patch.
# Adds ONE knob, hi846.a6l_rd (default 0 = no behaviour change): when set, dumps the sensor MIPI/PLL/size/stream
# registers right after stream-on (A6L_H846 ON ...) and again just before stream-off (A6L_H846 OFF ...), plus the
# lane count, mode and link frequency the driver used. Read-only I2C reads; nothing else changes.
import sys
fn = sys.argv[1]
s = open(fn).read()

def rep(old, new, cnt=1):
    global s
    n = s.count(old)
    if n != cnt:
        print('ANCHOR_FAIL', repr(old[:60]), n); sys.exit(1)
    s = s.replace(old, new)

rep('static int hi846_start_streaming(struct hi846 *hi846)\n{',
'''static int a6l_rd;
module_param(a6l_rd, int, 0644);
MODULE_PARM_DESC(a6l_rd, "A6L camfix16: 1 = dump MIPI/PLL/size/stream registers at stream on/off");

static void a6l_h846_dump(struct hi846 *hi846, const char *tag)
{
	static const u16 r[] = {
		0x0a00, 0x0a01, 0x0a04, 0x0a05, 0x0900, 0x0901, 0x0902, 0x0903,
		0x0914, 0x0915, 0x0916, 0x0917, 0x090c, 0x090d, 0x090e, 0x090f,
		0x0f2a, 0x0f2b, 0x0f38, 0x0f39, 0x0a12, 0x0a13, 0x0a14, 0x0a15,
		0x0006, 0x0007, 0x0008, 0x0009, 0x0034, 0x020a, 0x020b, 0x2000,
		0x2001, 0x2004, 0x2005, 0x004c, 0x004d, 0x0044, 0x0045,
	};
	struct i2c_client *client = v4l2_get_subdevdata(&hi846->sd);
	char buf[ARRAY_SIZE(r) * 11 + 1];
	int i, n = 0;
	u8 v;

	if (!a6l_rd)
		return;
	for (i = 0; i < ARRAY_SIZE(r); i++) {
		if (hi846_read_reg(hi846, r[i], &v))
			n += scnprintf(buf + n, sizeof(buf) - n, " %04x=??", r[i]);
		else
			n += scnprintf(buf + n, sizeof(buf) - n, " %04x=%02x", r[i], v);
	}
	dev_info(&client->dev, "A6L_H846 %s lanes=%u mode=%ux%u link_freq=%lld regs%s\\n", tag,
		 hi846->nr_lanes, hi846->cur_mode->width, hi846->cur_mode->height,
		 (long long)hi846_link_freqs[hi846->cur_mode->link_freq_index], buf);
}

static int hi846_start_streaming(struct hi846 *hi846)
{''')

rep('''	hi846->streaming = 1;

	dev_dbg(&client->dev, "%s: started streaming successfully\\n", __func__);''',
'''	hi846->streaming = 1;

	if (a6l_rd) {
		usleep_range(20000, 21000);
		a6l_h846_dump(hi846, "ON");
	}

	dev_dbg(&client->dev, "%s: started streaming successfully\\n", __func__);''')

rep('''	struct i2c_client *client = v4l2_get_subdevdata(&hi846->sd);

	if (hi846_write_reg(hi846, HI846_REG_MODE_SELECT, HI846_MODE_STANDBY))''',
'''	struct i2c_client *client = v4l2_get_subdevdata(&hi846->sd);

	a6l_h846_dump(hi846, "OFF");
	if (hi846_write_reg(hi846, HI846_REG_MODE_SELECT, HI846_MODE_STANDBY))''')
open(fn, 'w').write(s)
print('CAMFIX16_HI846_PATCH_OK')
