#!/usr/bin/env python3
# camss diag17 (29 Sep 2026, docs/hi846-20260929.md round 2): runtime-only diagnostics on top of the rom1 camss
# (camss_rom1_patch.py output, a6l_wm default 6 = the camera15/16 bundle build). Every knob defaults to OFF/unchanged
# behaviour, so the module behaves exactly like rom1 until a sysfs param is written. Never needs rmmod.
#   usage: camss_diag17_patch.py <camss_dir>
# Params (qcom_camss.*):
#   a6l_c17        (0)   bit0 CSID packet counters while streaming: STATS TOTAL 0x98 / ECC 0x9c (hi|lo halves) / CRC 0xa0,
#                        IRQ status 0x6c, captured headers 0x70-0x80, PIF MISR DL0-3 0x88-0x94, CORE_CTRL_0/1, CID LUT/CFG
#                        -> "A6L_C17 CSIDn ON|MID#k|OFF ..." at stream on, every a6l_c17_ms (3x) and at stream off
#                        bit1 CSIPHY readback after lane config (CTRL5/6/7, LN CFG1/2/3/5 of the used lanes + clock LN7)
#                        and COMMON_STATUS0-10 at lanes-disable -> "A6L_C17 CSIPHYn ..."
#   a6l_c17_ms     (400) period of the MID samples
#   a6l_c17_phy    (1)   the CSIPHY the overrides below apply to (1 = hi846)
#   a6l_c17_settle (0)   settle count override on a6l_c17_phy (0 = mainline formula; stock hi846 lib 0x14 = 20)
#   a6l_c17_lpos   (0)   data-lane position override on a6l_c17_phy, CSID lane_assign encoding (nibble i = position of data
#                        lane i; 0x10 = the DT value <0 1>, e.g. 0x32, 0x01, 0x21). Applied to the CSIPHY lane config AND the
#                        CSID CORE_CTRL_0 lane_assign of the CSID fed by that PHY. 0 = DT.
import sys

cs = sys.argv[1]
CSID = f"{cs}/camss-csid-4-7.c"
PHY = f"{cs}/camss-csiphy-3ph-1-0.c"

def sub1(path, old, new):
    s = open(path).read()
    n = s.count(old)
    assert n == 1, f"{path}: anchor found {n}x: {old[:70]!r}"
    open(path, 'w').write(s.replace(old, new))
    print(f"PATCHED {path.split('/')[-1]}: {old.strip().splitlines()[0][:60]}")

# ---------------- CSID ----------------
sub1(CSID, "static void csid_configure_stream(struct csid_device *csid, u8 enable)\n{\n", r'''/* ================= A6L diag17 (runtime only, default off) ================= */
uint a6l_c17;
module_param(a6l_c17, uint, 0644);
MODULE_PARM_DESC(a6l_c17, "A6L diag17: bit0 CSID packet counters/headers at stream on, every a6l_c17_ms (3x) and at stream off; bit1 CSIPHY lane readback + status");
static uint a6l_c17_ms = 400;
module_param(a6l_c17_ms, uint, 0644);
MODULE_PARM_DESC(a6l_c17_ms, "A6L diag17: period of the MID CSID samples (ms)");
uint a6l_c17_phy = 1;
module_param(a6l_c17_phy, uint, 0644);
MODULE_PARM_DESC(a6l_c17_phy, "A6L diag17: CSIPHY index the settle/lane-position overrides apply to (default 1 = hi846)");
uint a6l_c17_settle;
module_param(a6l_c17_settle, uint, 0644);
MODULE_PARM_DESC(a6l_c17_settle, "A6L diag17: settle count override on a6l_c17_phy (0 = mainline formula; stock hi846 0x14)");
uint a6l_c17_lpos;
module_param(a6l_c17_lpos, uint, 0644);
MODULE_PARM_DESC(a6l_c17_lpos, "A6L diag17: data lane positions on a6l_c17_phy, lane_assign encoding (0x10 = DT <0 1>; 0 = DT)");

static struct csid_device *a6l_c17_csid;
static unsigned int a6l_c17_n;
static u32 a6l_c17_tot0, a6l_c17_crc0;
static void a6l_c17_work_fn(struct work_struct *w);
static DECLARE_DELAYED_WORK(a6l_c17_work, a6l_c17_work_fn);

static void a6l_c17_log(struct csid_device *csid, const char *tag, unsigned int k)
{
	void __iomem *b = csid->base;
	u32 tot = readl_relaxed(b + 0x098), ecc = readl_relaxed(b + 0x09c), crc = readl_relaxed(b + 0x0a0);

	if (!strcmp(tag, "ON")) {
		a6l_c17_tot0 = tot;
		a6l_c17_crc0 = crc;
	}
	dev_info(csid->camss->dev,
		 "A6L_C17 CSID%u %s#%u phy%u lanes %u assign 0x%x tot %u (+%u) ecc hi %u lo %u crc %u (+%u) st %08x unm %08x mm %08x sh %08x lh %08x lf %08x misr %08x %08x %08x %08x ctrl0 %08x ctrl1 %08x lut0 %08x cid0 %08x\n",
		 csid->id, tag, k, csid->phy.csiphy_id, csid->phy.lane_cnt, csid->phy.lane_assign, tot, tot - a6l_c17_tot0,
		 ecc >> 16, ecc & 0xffff, crc, crc - a6l_c17_crc0, readl_relaxed(b + 0x06c), readl_relaxed(b + 0x070),
		 readl_relaxed(b + 0x074), readl_relaxed(b + 0x078), readl_relaxed(b + 0x07c), readl_relaxed(b + 0x080),
		 readl_relaxed(b + 0x088), readl_relaxed(b + 0x08c), readl_relaxed(b + 0x090), readl_relaxed(b + 0x094),
		 readl_relaxed(b + 0x004), readl_relaxed(b + 0x008), readl_relaxed(b + 0x014), readl_relaxed(b + 0x024));
}

static void a6l_c17_work_fn(struct work_struct *w)
{
	struct csid_device *csid = READ_ONCE(a6l_c17_csid);

	if (!csid)
		return;
	a6l_c17_n++;
	a6l_c17_log(csid, "MID", a6l_c17_n);
	if (a6l_c17_n < 3)
		schedule_delayed_work(&a6l_c17_work, msecs_to_jiffies(a6l_c17_ms ? a6l_c17_ms : 400));
}

static void csid_configure_stream(struct csid_device *csid, u8 enable)
{
''')

sub1(CSID, "			val = phy->lane_cnt - 1;\n			val |= phy->lane_assign << 4;\n",
"""			if (a6l_c17_lpos && phy->csiphy_id == a6l_c17_phy) { /* A6L diag17: lane position override */
				dev_info(csid->camss->dev, "A6L_C17 CSID%u lane_assign 0x%x -> 0x%x (a6l_c17_lpos, phy%u)\\n",
					 csid->id, phy->lane_assign, a6l_c17_lpos & 0xffff, phy->csiphy_id);
				phy->lane_assign = a6l_c17_lpos & 0xffff;
			}
			val = phy->lane_cnt - 1;
			val |= phy->lane_assign << 4;
""")

sub1(CSID, """		if (tg->enabled) {
			val = CAMSS_CSID_TG_CTRL_ENABLE;
			writel_relaxed(val, csid->base + CAMSS_CSID_TG_CTRL);
		}
	} else {
""", """		if (tg->enabled) {
			val = CAMSS_CSID_TG_CTRL_ENABLE;
			writel_relaxed(val, csid->base + CAMSS_CSID_TG_CTRL);
		}
		if ((a6l_c17 & 1) && !tg->enabled) { /* A6L diag17 */
			cancel_delayed_work_sync(&a6l_c17_work);
			a6l_c17_n = 0;
			a6l_c17_log(csid, "ON", 0);
			WRITE_ONCE(a6l_c17_csid, csid);
			schedule_delayed_work(&a6l_c17_work, msecs_to_jiffies(a6l_c17_ms ? a6l_c17_ms : 400));
		}
	} else {
		if (READ_ONCE(a6l_c17_csid) == csid) { /* A6L diag17 */
			WRITE_ONCE(a6l_c17_csid, NULL);
			cancel_delayed_work_sync(&a6l_c17_work);
			a6l_c17_log(csid, "OFF", a6l_c17_n);
		}
""")

# ---------------- CSIPHY ----------------
sub1(PHY, "static void csiphy_hw_version_read(struct csiphy_device *csiphy,\n",
"""/* A6L diag17 (params in camss-csid-4-7.c) */
extern uint a6l_c17, a6l_c17_phy, a6l_c17_settle, a6l_c17_lpos;
static u8 a6l_c17_orig[4][4];
static bool a6l_c17_saved[4];

static void csiphy_hw_version_read(struct csiphy_device *csiphy,
""")

sub1(PHY, """	settle_cnt = csiphy_settle_cnt_calc(link_freq, csiphy->timer_clk_rate);

	val = CSIPHY_3PH_CMN_CSI_COMMON_CTRL5_CLK_ENABLE;
""", """	settle_cnt = csiphy_settle_cnt_calc(link_freq, csiphy->timer_clk_rate);

	/* A6L diag17: lane position / settle overrides on a6l_c17_phy (restored from the DT copy when the knob is 0) */
	{
		unsigned int id = csiphy->id & 3;

		if (!a6l_c17_saved[id]) {
			for (i = 0; i < c->num_data && i < 4; i++)
				a6l_c17_orig[id][i] = c->data[i].pos;
			a6l_c17_saved[id] = true;
		}
		for (i = 0; i < c->num_data && i < 4; i++)
			c->data[i].pos = (a6l_c17_lpos && csiphy->id == a6l_c17_phy) ?
					 (a6l_c17_lpos >> (4 * i)) & 0x3 : a6l_c17_orig[id][i];
		if (a6l_c17_lpos && csiphy->id == a6l_c17_phy)
			dev_info(csiphy->camss->dev, "A6L_C17 CSIPHY%u data lane pos override 0x%x (num_data %u)\\n",
				 csiphy->id, a6l_c17_lpos, c->num_data);
		if (a6l_c17_settle && a6l_c17_settle < 256 && csiphy->id == a6l_c17_phy) {
			dev_info(csiphy->camss->dev, "A6L_C17 CSIPHY%u settle_cnt %u (formula %u, link_freq %lld, timer %u)\\n",
				 csiphy->id, a6l_c17_settle, settle_cnt, link_freq, csiphy->timer_clk_rate);
			settle_cnt = a6l_c17_settle;
		}
	}

	val = CSIPHY_3PH_CMN_CSI_COMMON_CTRL5_CLK_ENABLE;
""")

sub1(PHY, """		csiphy_gen1_config_lanes(csiphy, cfg, settle_cnt);
""", """		csiphy_gen1_config_lanes(csiphy, cfg, settle_cnt);

	if (a6l_c17 & 2) { /* A6L diag17: readback of what the lanes got */
		char buf[200];
		int n = 0, l;

		for (i = 0; i <= c->num_data; i++) {
			l = (i == c->num_data) ? 7 : c->data[i].pos * 2;
			n += scnprintf(buf + n, sizeof(buf) - n, " LN%d %02x/%02x/%02x/%02x", l,
				       readl_relaxed(csiphy->base + CSIPHY_3PH_LNn_CFG1(l)) & 0xff,
				       readl_relaxed(csiphy->base + CSIPHY_3PH_LNn_CFG2(l)) & 0xff,
				       readl_relaxed(csiphy->base + CSIPHY_3PH_LNn_CFG3(l)) & 0xff,
				       readl_relaxed(csiphy->base + CSIPHY_3PH_LNn_CFG5(l)) & 0xff);
		}
		dev_info(csiphy->camss->dev,
			 "A6L_C17 CSIPHY%u link_freq %lld timer %u settle %u ctrl5 %02x ctrl6 %02x ctrl7 %02x cfg1/2/3/5:%s\\n",
			 csiphy->id, link_freq, csiphy->timer_clk_rate, settle_cnt,
			 readl_relaxed(csiphy->base + CSIPHY_3PH_CMN_CSI_COMMON_CTRLn(regs->offset, 5)) & 0xff,
			 readl_relaxed(csiphy->base + CSIPHY_3PH_CMN_CSI_COMMON_CTRLn(regs->offset, 6)) & 0xff,
			 readl_relaxed(csiphy->base + CSIPHY_3PH_CMN_CSI_COMMON_CTRLn(regs->offset, 7)) & 0xff, buf);
	}
""")

sub1(PHY, """	struct csiphy_device_regs *regs = csiphy->regs;

	writel_relaxed(0, csiphy->base +
			  CSIPHY_3PH_CMN_CSI_COMMON_CTRLn(regs->offset, 5));
""", """	struct csiphy_device_regs *regs = csiphy->regs;

	if (a6l_c17 & 2) { /* A6L diag17: PHY status before the lanes go down */
		char buf[120];
		int n = 0, i;

		for (i = 0; i < 11; i++)
			n += scnprintf(buf + n, sizeof(buf) - n, " %02x",
				       readl_relaxed(csiphy->base + CSIPHY_3PH_CMN_CSI_COMMON_STATUSn(regs->offset,
						     regs->common_status_offset, i)) & 0xff);
		dev_info(csiphy->camss->dev, "A6L_C17 CSIPHY%u OFF status0-10:%s\\n", csiphy->id, buf);
	}
	if (cfg && cfg->csi2 && a6l_c17_saved[csiphy->id & 3]) { /* A6L diag17: DT lane positions back for the next link setup */
		int i;

		for (i = 0; i < cfg->csi2->lane_cfg.num_data && i < 4; i++)
			cfg->csi2->lane_cfg.data[i].pos = a6l_c17_orig[csiphy->id & 3][i];
	}

	writel_relaxed(0, csiphy->base +
			  CSIPHY_3PH_CMN_CSI_COMMON_CTRLn(regs->offset, 5));
""")
print("CAMSS_DIAG17_PATCH_OK")
