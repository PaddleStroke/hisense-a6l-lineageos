#!/usr/bin/env python3
# camfix12 (29 Sep 2026): applied AFTER camfix_patch.py .. camfix11_patch.py.   usage: camfix12_patch.py <camss_dir>
# t34 (camera13 / camfix11 sweep, 12 sensor rows + a TG row that rebooted the phone): every sensor row identical
# whatever settle (10/14/20/28), COMMON_CTRL7 (0x02/reset/0), CORE_CTRL_1 (0x9/0xF) or CID LUT. Decoded:
#   CSID STATS_ECC (0x9c) = two 16-bit saturating counters, hi16 + lo16 == STATS_TOTAL exactly, hi ~94.5 % (imx576) /
#   ~77 % (s5k3t1): every packet the CSID sees is counted as an ECC event; only ~330 of ~2158 packets per imx576
#   frame are seen at all; s5k3t1 shows 4.7 CSID SOFs per real sensor frame (spurious frame starts).
#   => the bytes reaching the CSID are corrupted before the packet layer, on both PHYs, independently of the analog
#   lane settings. Stock (sdm660-camera.dtsi csiphy@c82x000) enables MMSS_CAMSS_CSIPHYn_CLK + CSIPHY_CLK_SRC at
#   200 MHz (and CSIn_CLK_SRC 310 MHz, CPHY_CSIDn); mainline sdm660 camss never names the csiphyN clocks and never
#   sets csiphy_clk_src: CSID0's cphy_csid0 only enables csiphy0 as its parent at the RCG's reset rate, and the front
#   camera (CSIPHY2 -> CSID1) enables cphy_csid1 -> csiphy1, so CSIPHY2's digital clock is never enabled at all.
# New runtime params (all undone at power-off; older knobs unchanged):
#   qcom_camss.a6l_v12 (default 0x13)
#     bit0 (1)  diagnostics: CSIPHY clock tree (clk API rates + MMCC RCG/CBCR raw) at power on/after fix/off,
#               clk_mux readback, CSIPHY lane-block readback (lanes 0/2/4/6 + clock 7) at stream on and lane 0/7
#               0x00-0xfc at stream off, CSID PIF MISR DL0-3 + STATS_ECC hi16/lo16 at SOF 1-4 (A6L_V12 P) and
#               A6L_V12 SUM (ecc hi/lo deltas, saturation), CSID rail voltages at power on
#     bit1 (2)  stock CSIPHY clocks: for CSIPHY k enable cphy_csid<k> (parent camss_csiphy<k>_clk, grandparent
#               csiphy_clk_src) at a6l_v12_phyclk (default 200 MHz, stock) and csi<k> at a6l_v12_csiclk (310 MHz)
#     bit2 (4)  stock CSID rails: vdda 1.2 V, vdd_sec 0.925 V (regulator_set_voltage before enable; restored at off)
#     bit3 (8)  TG isolation: with the CSID test generator, CORE_CTRL_0/1 = 0 (no PHY input, as after a stock reset)
#     bit4 (16) clear the camfix10 CSID/CSIPHY poll pointers at VFE stop (t34: stale pointer of the previous
#               stream's CSID seen at VFE START, i.e. a possible unclocked register read)
#   qcom_camss.a6l_v12_phyclk (200000000)  0 = enable only, keep the rate
#   qcom_camss.a6l_v12_csiclk (310000000)  0 = do not touch csi<k>
import sys

def sub1(path, old, new):
    s = open(path).read()
    n = s.count(old)
    assert n == 1, f"{path}: anchor found {n}x: {old[:70]!r}"
    open(path, 'w').write(s.replace(old, new))
    print(f"PATCHED {path.split('/')[-1]}: {old.strip().splitlines()[0][:60]}")

cs = sys.argv[1]
H = f"{cs}/camss-vfe.h"
V48 = f"{cs}/camss-vfe-4-8.c"
CSID = f"{cs}/camss-csid-4-7.c"
CSIDC = f"{cs}/camss-csid.c"
PHY = f"{cs}/camss-csiphy-3ph-1-0.c"
PHYC = f"{cs}/camss-csiphy.c"

# ---------------- header ----------------
sub1(H, "bool a6l_v11_csid_irq(u32 id, u32 status);\n", """bool a6l_v11_csid_irq(u32 id, u32 status);
/* A6L camfix12 (camss-vfe-4-8.c) */
extern uint a6l_v12, a6l_v12_phyclk, a6l_v12_csiclk;
""")

# ---------------- camss-vfe-4-8.c: params, SOF/SUM diagnostics, stale pointer clear ----------------
sub1(V48, "int a6l_wmmax = 1;\n", r'''/* ================= camfix12: CSIPHY clocks / rails / packet counters ================= */
uint a6l_v12 = 0x13;
module_param(a6l_v12, uint, 0644);
MODULE_PARM_DESC(a6l_v12, "A6L camfix12: bit0 diagnostics (CSIPHY clock tree, lane readback, MISR, ECC hi/lo), bit1 stock CSIPHY clocks (cphy_csidK/csiphyK at a6l_v12_phyclk, csiK at a6l_v12_csiclk), bit2 stock CSID rails (vdda 1.2 V, vdd_sec 0.925 V), bit3 TG isolation (CORE_CTRL_0/1 = 0 with the test generator), bit4 clear stale CSID/CSIPHY poll pointers at VFE stop (default 0x13)");
uint a6l_v12_phyclk = 200000000;
module_param(a6l_v12_phyclk, uint, 0644);
MODULE_PARM_DESC(a6l_v12_phyclk, "A6L camfix12: csiphy_clk_src rate for a6l_v12 bit1 (stock 200000000; 0 = enable only)");
uint a6l_v12_csiclk = 310000000;
module_param(a6l_v12_csiclk, uint, 0644);
MODULE_PARM_DESC(a6l_v12_csiclk, "A6L camfix12: csiK_clk_src rate for a6l_v12 bit1 (stock 310000000; 0 = do not touch csiK)");

int a6l_wmmax = 1;
''')

sub1(V48, "\t\t\t a6l_v10_nsof[v], a6l_v10_csid_id, st, unm, mm, sh, lh, lf, tot, ecc, crc);\n\t}\n}\n",
     "\t\t\t a6l_v10_nsof[v], a6l_v10_csid_id, st, unm, mm, sh, lh, lf, tot, ecc, crc);\n\t}\n"
     "\tif ((a6l_v12 & 1) && a6l_v10_nsof[v] <= 4) /* camfix12: per-lane MISR + ECC counter halves */\n"
     "\t\tdev_info(vfe->camss->dev,\n"
     "\t\t\t \"A6L_V12 P sof#%u csid%u misr dl0 %08x dl1 %08x dl2 %08x dl3 %08x | ecc hi %u lo %u (hi+lo %u) tot %u crc %u\\n\",\n"
     "\t\t\t a6l_v10_nsof[v], a6l_v10_csid_id, readl_relaxed(c + 0x088), readl_relaxed(c + 0x08c),\n"
     "\t\t\t readl_relaxed(c + 0x090), readl_relaxed(c + 0x094), ecc >> 16, ecc & 0xffff,\n"
     "\t\t\t (ecc >> 16) + (ecc & 0xffff), tot, crc);\n}\n")

sub1(V48, "\ta6l_v11_hprint(vfe, \"long\", a6l_v11_hl);\n", r'''	if (a6l_v12 & 1) { /* camfix12: STATS_ECC is hi16 + lo16 (saturating 16-bit halves), hi + lo == STATS_TOTAL */
		u32 h0 = a6l_v11_ecc0 >> 16, l0 = a6l_v11_ecc0 & 0xffff, h1 = a6l_v11_eccl >> 16, l1 = a6l_v11_eccl & 0xffff;
		u32 dt = a6l_v11_totl - a6l_v11_tot0;

		dev_info(vfe->camss->dev,
			 "A6L_V12 SUM csid%u frames %u pkts +%u (%u/frame) ecc_hi %u->%u (+%u)%s ecc_lo %u->%u (+%u)%s hi+lo==tot %s | hi%% %u lo%% %u\n",
			 a6l_v10_csid_id, a6l_v11_np, dt, a6l_v11_np ? dt / a6l_v11_np : 0, h0, h1, h1 - h0,
			 h1 == 0xffff ? " SAT" : "", l0, l1, l1 - l0, l1 == 0xffff ? " SAT" : "",
			 (h1 + l1 == a6l_v11_totl) ? "yes" : "no", dt ? (h1 - h0) * 100 / dt : 0, dt ? (l1 - l0) * 100 / dt : 0);
	}
	a6l_v11_hprint(vfe, "long", a6l_v11_hl);
''')

sub1(V48, "\t\ta6l_v11_sum(vfe); /* camfix11 */\n", """		a6l_v11_sum(vfe); /* camfix11 */
	if (a6l_v12 & 16) { /* camfix12: no stale CSID/CSIPHY pointer survives into the next stream's VFE START poll */
		WRITE_ONCE(a6l_v10_csid, NULL);
		WRITE_ONCE(a6l_v10_phy, NULL);
	}
""")

# ---------------- CSID: TG isolation ----------------
sub1(CSID, "\t\t\tval = tg->mode - 1;\n\t\t\twritel_relaxed(val, csid->base + CAMSS_CSID_TG_DT_n_CGG_2(0));\n",
"""			val = tg->mode - 1;
			writel_relaxed(val, csid->base + CAMSS_CSID_TG_DT_n_CGG_2(0));
			if (a6l_v12 & 8) { /* camfix12: TG isolation, no PHY input selected (stock reset state) */
				writel_relaxed(0, csid->base + CAMSS_CSID_CORE_CTRL_0);
				writel_relaxed(0, csid->base + CAMSS_CSID_CORE_CTRL_1);
				dev_info(csid->camss->dev, "A6L_V12 CSID%u TG isolation: CORE_CTRL_0/1 = %08x/%08x\\n", csid->id,
					 readl_relaxed(csid->base + CAMSS_CSID_CORE_CTRL_0),
					 readl_relaxed(csid->base + CAMSS_CSID_CORE_CTRL_1));
			}
""")

# ---------------- CSID rails (camss-csid.c) ----------------
sub1(CSIDC, "static int csid_set_power(struct v4l2_subdev *sd, int on)\n{\n", r'''/* camfix12 bit2: stock CSID rail voltages (sdm660-camera.dtsi: mipi-csi-vdd pm660_l1 1.2 V, vdd_sec pm660l_l1 0.925 V) */
static int a6l_v12_uv_old[4][4];
static bool a6l_v12_uv_set[4][4];

static void a6l_v12_csid_vreg(struct csid_device *csid, int on)
{
	unsigned int k = csid->id & 3;
	int i, ret, want;

	for (i = 0; i < csid->num_supplies && i < 4; i++) {
		struct regulator *r = csid->supplies[i].consumer;
		const char *n = csid->supplies[i].supply;

		if (!r || !n)
			continue;
		if (on) {
			int before = regulator_get_voltage(r);

			want = !strcmp(n, "vdda") ? 1200000 : !strcmp(n, "vdd_sec") ? 925000 : 0;
			ret = 0;
			if ((a6l_v12 & 4) && want) {
				a6l_v12_uv_old[k][i] = before;
				ret = regulator_set_voltage(r, want, want);
				a6l_v12_uv_set[k][i] = !ret;
			}
			if (a6l_v12 & 5)
				dev_info(csid->camss->dev, "A6L_V12 CSID%u rail %s %d uV -> %d uV (want %d, set %d)\n",
					 csid->id, n, before, regulator_get_voltage(r), (a6l_v12 & 4) ? want : 0, ret);
		} else if (a6l_v12_uv_set[k][i]) {
			a6l_v12_uv_set[k][i] = false;
			if (a6l_v12_uv_old[k][i] > 0)
				regulator_set_voltage(r, a6l_v12_uv_old[k][i], a6l_v12_uv_old[k][i]);
		}
	}
}

static int csid_set_power(struct v4l2_subdev *sd, int on)
{
''')
sub1(CSIDC, "\t\tret = regulator_bulk_enable(csid->num_supplies,\n\t\t\t\t\t    csid->supplies);\n",
     "\t\ta6l_v12_csid_vreg(csid, 1); /* camfix12 */\n\t\tret = regulator_bulk_enable(csid->num_supplies,\n\t\t\t\t\t    csid->supplies);\n")
sub1(CSIDC, "\t\tregulator_bulk_disable(csid->num_supplies,\n\t\t\t\t       csid->supplies);\n\t\tpm_runtime_put_sync(dev);\n\t\tcsid->res->parent_dev_ops->put(camss, csid->id);\n",
     "\t\tregulator_bulk_disable(csid->num_supplies,\n\t\t\t\t       csid->supplies);\n\t\ta6l_v12_csid_vreg(csid, 0); /* camfix12 */\n\t\tpm_runtime_put_sync(dev);\n\t\tcsid->res->parent_dev_ops->put(camss, csid->id);\n")

# ---------------- CSIPHY clocks (camss-csiphy.c) ----------------
sub1(PHYC, "#include <linux/clk.h>\n", "#include <linux/clk.h>\n#include <linux/clk-provider.h> /* camfix12: __clk_get_name, __clk_is_enabled */\n")
sub1(PHYC, "static int csiphy_set_power(struct v4l2_subdev *sd, int on)\n{\n", r'''/*
 * camfix12: stock sdm660 CSIPHY clocks. Stock csiphy@c82{4,5,6}000 enables MMSS_CAMSS_CSIPHYk_CLK with
 * CSIPHY_CLK_SRC at 200 MHz, MMSS_CAMSS_CPHY_CSIDk_CLK and CSIk_CLK (CSIk_CLK_SRC 310 MHz). Mainline names
 * none of the csiphyK clocks; camss_cphy_csidK_clk's parent is camss_csiphyK_clk (then csiphy_clk_src), so
 * enabling "cphy_csidK" from the camss node for CSIPHY K enables exactly the stock branch chain.
 */
#define A6L_V12_MMCC	0x0c8c0000
static void __iomem *a6l_v12_mmcc;
static struct clk *a6l_v12_cphy[3], *a6l_v12_csi[3];

static unsigned long a6l_v12_rate(struct clk *c)
{
	return (c && !IS_ERR(c)) ? clk_get_rate(c) : 0;
}

static void a6l_v12_clk_dump(struct csiphy_device *csiphy, const char *tag)
{
	struct device *dev = csiphy->camss->dev;
	unsigned int k = csiphy->id;
	struct clk *cp, *p1 = NULL, *p2 = NULL, *csi, *phyc;
	char n[16];

	if (!a6l_v12_mmcc)
		a6l_v12_mmcc = ioremap(A6L_V12_MMCC, 0x10000);
	snprintf(n, sizeof(n), "cphy_csid%u", k);
	cp = clk_get(dev, n);
	if (!IS_ERR(cp)) {
		p1 = clk_get_parent(cp);
		if (p1)
			p2 = clk_get_parent(p1);
	}
	snprintf(n, sizeof(n), "csi%u", k);
	csi = clk_get(dev, n);
	snprintf(n, sizeof(n), "csi%u_phy", k);
	phyc = clk_get(dev, n);
	dev_info(dev,
		 "A6L_V12 CSIPHY%u CLK %s cphy_csid%u %lu en %d | parent %s %lu en %d | gparent %s %lu | csi%u %lu | csi%u_phy %lu | mmcc csiphy_src cmd %08x cfg %08x cbcr csiphy0/1/2 %08x %08x %08x cphy_csid0-3 %08x %08x %08x %08x\n",
		 k, tag, k, a6l_v12_rate(cp), IS_ERR(cp) ? -1 : __clk_is_enabled(cp),
		 p1 ? __clk_get_name(p1) : "-", a6l_v12_rate(p1), p1 ? __clk_is_enabled(p1) : -1,
		 p2 ? __clk_get_name(p2) : "-", a6l_v12_rate(p2), k, a6l_v12_rate(csi), k, a6l_v12_rate(phyc),
		 a6l_v12_mmcc ? readl_relaxed(a6l_v12_mmcc + 0x3800) : 0, a6l_v12_mmcc ? readl_relaxed(a6l_v12_mmcc + 0x3804) : 0,
		 a6l_v12_mmcc ? readl_relaxed(a6l_v12_mmcc + 0x3740) : 0, a6l_v12_mmcc ? readl_relaxed(a6l_v12_mmcc + 0x3744) : 0,
		 a6l_v12_mmcc ? readl_relaxed(a6l_v12_mmcc + 0x3748) : 0, a6l_v12_mmcc ? readl_relaxed(a6l_v12_mmcc + 0x3730) : 0,
		 a6l_v12_mmcc ? readl_relaxed(a6l_v12_mmcc + 0x3734) : 0, a6l_v12_mmcc ? readl_relaxed(a6l_v12_mmcc + 0x3738) : 0,
		 a6l_v12_mmcc ? readl_relaxed(a6l_v12_mmcc + 0x373c) : 0);
	if (!IS_ERR(cp))
		clk_put(cp);
	if (!IS_ERR(csi))
		clk_put(csi);
	if (!IS_ERR(phyc))
		clk_put(phyc);
}

static void a6l_v12_phy_power(struct csiphy_device *csiphy, int on)
{
	struct device *dev = csiphy->camss->dev;
	unsigned int k = csiphy->id % 3;
	struct clk *c;
	char n[16];
	int r1 = 0, r2 = 0, r3 = 0, r4 = 0;

	if (on) {
		if (a6l_v12 & 1)
			a6l_v12_clk_dump(csiphy, "PRE");
		if (!(a6l_v12 & 2))
			return;
		snprintf(n, sizeof(n), "cphy_csid%u", csiphy->id);
		c = clk_get(dev, n);
		if (IS_ERR(c)) {
			dev_info(dev, "A6L_V12 CSIPHY%u no clock %s (%ld)\n", csiphy->id, n, PTR_ERR(c));
		} else {
			if (a6l_v12_phyclk)
				r1 = clk_set_rate(c, a6l_v12_phyclk);
			r2 = clk_prepare_enable(c);
			if (r2) {
				clk_put(c);
				c = NULL;
			}
			a6l_v12_cphy[k] = c;
		}
		snprintf(n, sizeof(n), "csi%u", csiphy->id);
		c = a6l_v12_csiclk ? clk_get(dev, n) : ERR_PTR(-ENOENT);
		if (!IS_ERR(c)) {
			if (clk_get_rate(c) != a6l_v12_csiclk)
				r3 = clk_set_rate(c, a6l_v12_csiclk);
			r4 = clk_prepare_enable(c);
			if (r4) {
				clk_put(c);
				c = NULL;
			}
			a6l_v12_csi[k] = c;
		}
		dev_info(dev, "A6L_V12 CSIPHY%u stock clocks: cphy_csid%u set_rate(%u) %d enable %d | csi%u set_rate(%u) %d enable %d\n",
			 csiphy->id, csiphy->id, a6l_v12_phyclk, r1, r2, csiphy->id, a6l_v12_csiclk, r3, r4);
		if (a6l_v12 & 1)
			a6l_v12_clk_dump(csiphy, "ON");
	} else {
		if (a6l_v12 & 1)
			a6l_v12_clk_dump(csiphy, "OFF");
		if (a6l_v12_cphy[k]) {
			clk_disable_unprepare(a6l_v12_cphy[k]);
			clk_put(a6l_v12_cphy[k]);
			a6l_v12_cphy[k] = NULL;
		}
		if (a6l_v12_csi[k]) {
			clk_disable_unprepare(a6l_v12_csi[k]);
			clk_put(a6l_v12_csi[k]);
			a6l_v12_csi[k] = NULL;
		}
	}
}

static int csiphy_set_power(struct v4l2_subdev *sd, int on)
{
''')
sub1(PHYC, "\t\t\treturn ret;\n\t\t}\n\n\t\tenable_irq(csiphy->irq);\n\n\t\tcsiphy->res->hw_ops->reset(csiphy);\n",
     "\t\t\treturn ret;\n\t\t}\n\n\t\ta6l_v12_phy_power(csiphy, 1); /* camfix12 */\n\n\t\tenable_irq(csiphy->irq);\n\n\t\tcsiphy->res->hw_ops->reset(csiphy);\n")
sub1(PHYC, "\t\tdisable_irq(csiphy->irq);\n\n\t\tcamss_disable_clocks(csiphy->nclocks, csiphy->clock);\n",
     "\t\tdisable_irq(csiphy->irq);\n\n\t\ta6l_v12_phy_power(csiphy, 0); /* camfix12 */\n\n\t\tcamss_disable_clocks(csiphy->nclocks, csiphy->clock);\n")

# ---------------- CSIPHY lane readback (3ph-1-0) ----------------
sub1(PHY, "\t/* IRQ_MASK registers - disable all interrupts */\n\tfor (i = 11; i < 22; i++) {\n", r'''	if (a6l_v12 & 1) { /* camfix12: clk_mux + per-lane config readback (0x00-0x3c of lanes 0/2/4/6 and clock 7) */
		static const u8 ln[5] = { 0, 2, 4, 6, 7 };
		int j, w;

		dev_info(csiphy->camss->dev, "A6L_V12 CSIPHY%u clk_mux %08x (csid %u)\n", csiphy->id,
			 csiphy->base_clk_mux ? readl_relaxed(csiphy->base_clk_mux) : 0xffffffff, cfg->csid_id);
		for (j = 0; j < 5; j++) {
			u32 x[16];

			for (w = 0; w < 16; w++)
				x[w] = readl_relaxed(csiphy->base + 0x100 * ln[j] + 4 * w) & 0xff;
			dev_info(csiphy->camss->dev,
				 "A6L_V12 CSIPHY%u LN%u 00-3c: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
				 csiphy->id, ln[j], x[0], x[1], x[2], x[3], x[4], x[5], x[6], x[7], x[8], x[9], x[10],
				 x[11], x[12], x[13], x[14], x[15]);
		}
	}

	/* IRQ_MASK registers - disable all interrupts */
	for (i = 11; i < 22; i++) {
''')
sub1(PHY, "\ta6l_v10_phy_off(); /* camfix10 */\n", r'''	a6l_v10_phy_off(); /* camfix10 */
	if (a6l_v12 & 1) { /* camfix12: lane 0 and clock lane blocks 0x00-0xfc at stop (status registers undocumented) */
		static const u8 ln[2] = { 0, 7 };
		int j, w, q;

		for (j = 0; j < 2; j++)
			for (q = 0; q < 64; q += 16) {
				u32 x[16];

				for (w = 0; w < 16; w++)
					x[w] = readl_relaxed(csiphy->base + 0x100 * ln[j] + 4 * (q + w)) & 0xff;
				dev_info(csiphy->camss->dev,
					 "A6L_V12 CSIPHY%u STOP LN%u +%02x: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
					 csiphy->id, ln[j], q * 4, x[0], x[1], x[2], x[3], x[4], x[5], x[6], x[7], x[8],
					 x[9], x[10], x[11], x[12], x[13], x[14], x[15]);
			}
	}
''')
print("CAMFIX12_PATCH_OK")
