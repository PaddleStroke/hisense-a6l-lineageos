#!/usr/bin/env python3
# camfix11 (29 Sep 2026): applied AFTER camfix_patch.py .. camfix10_patch.py.   usage: camfix11_patch.py <camss_dir>
# t33 (camera12): every CSID test-generator row PASSES (full 7761600-byte frames, byte-exact incrementing pattern),
# every real-sensor row FAILS -> the VFE/WM/SMMU/DDR path is proven; the fault is sensor -> CSIPHY -> CSID RX.
# Real-sensor CSID status every frame: b0-7 (SOT/EOT DL0-3) + b16 b25 b27 b29 (+ b28 ~57 %, b20-23 ~3 %); none of
# these with the TG. camfix11 measures the packets themselves and attacks the RX configuration. See docs/camfix11-20260929.md.
# New runtime params (all undone at stream-off / re-applied per stream, older knobs unchanged):
#   qcom_camss.a6l_v11 (default 0x01)
#     bit0 (1)  CSID packet diagnostics (read-only, polled at every RDI SOF before the camfix10 clear):
#               CAPTURED_UNMAPPED_LONG_PKT_HDR 0x70, CAPTURED_MMAPPED_LONG_PKT_HDR 0x74, CAPTURED_SHORT_PKT 0x78,
#               CAPTURED_LONG_PKT_HDR 0x7c, CAPTURED_LONG_PKT_FTR 0x80, STATS_TOTAL_PKTS 0x98, STATS_ECC 0x9c,
#               STATS_CRC 0xa0 (stock msm_csid_3_5_1 map; same TG offsets as mainline 4-7) -> A6L_V11 P per frame
#               (1-8, every 64th), A6L_V11 SUM/HDR at stop: header histograms decoded two ways (WC/DT/VC)
#     bit1 (2)  CSID CORE_CTRL_1 low nibble 0xF (stock msm_csid) instead of mainline 0x9
#     bit2 (4)  CID LUT VC0 byte1 = a6l_v11_dt2 (default 0x36, the imx576 PDAF DT of stock res0/res1), CID1 disabled
#     bit3 (8)  CSID IRQ mask = stock 0x7f010800 | lane-overflow 0x00f00000 while streaming; per-bit IRQ counts
#               (events, not frames); after 4000 IRQs the mask goes back to RST_DONE (storm cap)
#     bit4 (16) like bit2 but CID1 enabled exactly like CID0 (stock csid_lut: cid1 dt 0x36 decode 10-bit)
#   qcom_camss.a6l_v11_settle  (0) CSIPHY settle count override (stock imx576 14, s5k3t1 19; 0 = mainline formula)
#   qcom_camss.a6l_v11_ctrl7   (-1) CSIPHY COMMON_CTRL7: -1 mainline 0x02, -2 do not write (reset value), 0..255 value
#   qcom_camss.a6l_v11_dt0     (0) CID0 data type override (0 = format DT, e.g. 0x2b)
#   qcom_camss.a6l_v11_dt2     (0x36) extra DT for bit2/bit4
#   qcom_camss.a6l_v11_lassign (0) CSID CORE_CTRL_0 lane_assign override (e.g. 0x3210; 0 = DT)
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
PHY = f"{cs}/camss-csiphy-3ph-1-0.c"

# ---------------- header ----------------
sub1(H, "void a6l_v10_phy_off(void);\n", """void a6l_v10_phy_off(void);
/* A6L camfix11 (camss-vfe-4-8.c) */
extern uint a6l_v11, a6l_v11_settle, a6l_v11_dt0, a6l_v11_dt2, a6l_v11_lassign;
extern int a6l_v11_ctrl7;
bool a6l_v11_csid_irq(u32 id, u32 status);
""")

# ---------------- camss-vfe-4-8.c: params + packet diagnostics ----------------
sub1(V48, "int a6l_wmmax = 1;\n", r'''/* ================= camfix11: sensor -> CSIPHY -> CSID RX ================= */
uint a6l_v11 = 1;
module_param(a6l_v11, uint, 0644);
MODULE_PARM_DESC(a6l_v11, "A6L camfix11: bit0 CSID packet diagnostics (captured headers, ECC/CRC/total stats), bit1 CSID CORE_CTRL_1 0xF (stock), bit2 map a6l_v11_dt2 on CID1 (disabled), bit3 stock CSID IRQ mask + per-bit counts (capped), bit4 map a6l_v11_dt2 on CID1 enabled like CID0 (default 1)");
uint a6l_v11_settle;
module_param(a6l_v11_settle, uint, 0644);
MODULE_PARM_DESC(a6l_v11_settle, "A6L camfix11: CSIPHY settle count override (0 = mainline formula; stock imx576 14, s5k3t1 19)");
int a6l_v11_ctrl7 = -1;
module_param(a6l_v11_ctrl7, int, 0644);
MODULE_PARM_DESC(a6l_v11_ctrl7, "A6L camfix11: CSIPHY COMMON_CTRL7 (-1 mainline 0x02, -2 not written, 0..255 value)");
uint a6l_v11_dt0;
module_param(a6l_v11_dt0, uint, 0644);
MODULE_PARM_DESC(a6l_v11_dt0, "A6L camfix11: CID0 data type override (0 = format data type)");
uint a6l_v11_dt2 = 0x36;
module_param(a6l_v11_dt2, uint, 0644);
MODULE_PARM_DESC(a6l_v11_dt2, "A6L camfix11: extra data type mapped on CID1 with a6l_v11 bit2/bit4 (default 0x36, imx576 PDAF)");
uint a6l_v11_lassign;
module_param(a6l_v11_lassign, uint, 0644);
MODULE_PARM_DESC(a6l_v11_lassign, "A6L camfix11: CSID lane_assign override (e.g. 0x3210, 0 = from DT)");

struct a6l_v11_h {
	u32 val, n;
};
static struct a6l_v11_h a6l_v11_hl[8], a6l_v11_hu[8], a6l_v11_hm[8], a6l_v11_hs[8], a6l_v11_hf[8];
static u32 a6l_v11_np, a6l_v11_nlog, a6l_v11_tot0, a6l_v11_ecc0, a6l_v11_crc0, a6l_v11_totl, a6l_v11_eccl, a6l_v11_crcl;
static u32 a6l_v11_irqn, a6l_v11_irqbits[32];
static bool a6l_v11_irqcap;

/* hard IRQ (csid_isr) with a6l_v11 bit3 armed: count every CSID event per bit; true once when the cap is hit */
bool a6l_v11_csid_irq(u32 id, u32 status)
{
	unsigned int b;

	if (a6l_v11_irqcap)
		return false;
	a6l_v11_irqn++;
	for (b = 0; b < 32; b++)
		if (status & BIT(b))
			a6l_v11_irqbits[b]++;
	if (a6l_v11_irqn >= 4000) {
		a6l_v11_irqcap = true;
		return true;
	}
	return false;
}

int a6l_wmmax = 1;
''')

sub1(V48, "/* hard IRQ, every VFE interrupt while an RDI line streams */\n", r'''/* camfix11: header histogram (8 distinct values) */
static void a6l_v11_hadd(struct a6l_v11_h *h, u32 x)
{
	int i;

	for (i = 0; i < 8; i++) {
		if (h[i].n && h[i].val == x) {
			h[i].n++;
			return;
		}
		if (!h[i].n) {
			h[i].val = x;
			h[i].n = 1;
			return;
		}
	}
}

/* camfix11 bit0: CSID captured packets + stats, at every RDI SOF, before the camfix10 poll clears the status */
static void a6l_v11_sof(struct vfe_device *vfe, unsigned int v)
{
	void __iomem *c = READ_ONCE(a6l_v10_csid);
	u32 st, unm, mm, sh, lh, lf, tot, ecc, crc;

	if (!(a6l_v11 & 1) || !c)
		return;
	st = readl_relaxed(c + 0x06c);
	unm = readl_relaxed(c + 0x070);
	mm = readl_relaxed(c + 0x074);
	sh = readl_relaxed(c + 0x078);
	lh = readl_relaxed(c + 0x07c);
	lf = readl_relaxed(c + 0x080);
	tot = readl_relaxed(c + 0x098);
	ecc = readl_relaxed(c + 0x09c);
	crc = readl_relaxed(c + 0x0a0);
	if (!a6l_v11_np++) {
		a6l_v11_tot0 = tot;
		a6l_v11_ecc0 = ecc;
		a6l_v11_crc0 = crc;
	}
	a6l_v11_totl = tot;
	a6l_v11_eccl = ecc;
	a6l_v11_crcl = crc;
	a6l_v11_hadd(a6l_v11_hl, lh);
	a6l_v11_hadd(a6l_v11_hu, unm);
	a6l_v11_hadd(a6l_v11_hm, mm);
	a6l_v11_hadd(a6l_v11_hs, sh);
	a6l_v11_hadd(a6l_v11_hf, lf);
	if ((a6l_v10_nsof[v] <= 8 || !(a6l_v10_nsof[v] % 64)) && a6l_v11_nlog < 24) {
		a6l_v11_nlog++;
		dev_info(vfe->camss->dev,
			 "A6L_V11 P sof#%u csid%u st %08x unm %08x mm %08x sh %08x lh %08x lf %08x tot %u ecc %u crc %u\n",
			 a6l_v10_nsof[v], a6l_v10_csid_id, st, unm, mm, sh, lh, lf, tot, ecc, crc);
	}
}

static void a6l_v11_hprint(struct vfe_device *vfe, const char *tag, struct a6l_v11_h *h)
{
	int i;

	for (i = 0; i < 8 && h[i].n; i++)
		dev_info(vfe->camss->dev,
			 "A6L_V11 HDR %s %08x x%u | A(di,wc,ecc): dt 0x%02x vc %u wc %u | B(stock short-pkt style): dt 0x%02x wc %u lo 0x%02x\n",
			 tag, h[i].val, h[i].n, h[i].val & 0x3f, (h[i].val >> 6) & 3, (h[i].val >> 8) & 0xffff,
			 (h[i].val >> 24) & 0x3f, (h[i].val >> 8) & 0xffff, h[i].val & 0xff);
}

static void a6l_v11_sum(struct vfe_device *vfe)
{
	char buf[320];
	int len = 0;
	unsigned int b;

	buf[0] = 0;
	for (b = 0; b < 32; b++)
		if (a6l_v11_irqbits[b])
			len += scnprintf(buf + len, sizeof(buf) - len, " b%u:%u", b, a6l_v11_irqbits[b]);
	dev_info(vfe->camss->dev,
		 "A6L_V11 SUM csid%u frames %u tot %u->%u (+%u) ecc %u->%u (+%u) crc %u->%u (+%u) v11 0x%02x settle %u ctrl7 %d dt0 0x%02x dt2 0x%02x irqs %u%s bits%s\n",
		 a6l_v10_csid_id, a6l_v11_np, a6l_v11_tot0, a6l_v11_totl, a6l_v11_totl - a6l_v11_tot0, a6l_v11_ecc0,
		 a6l_v11_eccl, a6l_v11_eccl - a6l_v11_ecc0, a6l_v11_crc0, a6l_v11_crcl, a6l_v11_crcl - a6l_v11_crc0,
		 a6l_v11, a6l_v11_settle, a6l_v11_ctrl7, a6l_v11_dt0, a6l_v11_dt2, a6l_v11_irqn,
		 a6l_v11_irqcap ? " CAPPED" : "", len ? buf : " none");
	a6l_v11_hprint(vfe, "long", a6l_v11_hl);
	a6l_v11_hprint(vfe, "unmapped", a6l_v11_hu);
	a6l_v11_hprint(vfe, "mmapped", a6l_v11_hm);
	a6l_v11_hprint(vfe, "short", a6l_v11_hs);
	a6l_v11_hprint(vfe, "ftr", a6l_v11_hf);
}

/* hard IRQ, every VFE interrupt while an RDI line streams */
''')

sub1(V48, "		a6l_v10_nsof[v]++;\n		cst = a6l_v10_poll_csid();\n",
     "		a6l_v10_nsof[v]++;\n		a6l_v11_sof(vfe, v); /* camfix11: before the clear */\n		cst = a6l_v10_poll_csid();\n")

sub1(V48, "	memset(a6l_v10_xbits, 0, sizeof(a6l_v10_xbits));\n", """	memset(a6l_v10_xbits, 0, sizeof(a6l_v10_xbits));
	/* camfix11 */
	memset(a6l_v11_hl, 0, sizeof(a6l_v11_hl));
	memset(a6l_v11_hu, 0, sizeof(a6l_v11_hu));
	memset(a6l_v11_hm, 0, sizeof(a6l_v11_hm));
	memset(a6l_v11_hs, 0, sizeof(a6l_v11_hs));
	memset(a6l_v11_hf, 0, sizeof(a6l_v11_hf));
	memset(a6l_v11_irqbits, 0, sizeof(a6l_v11_irqbits));
	a6l_v11_np = a6l_v11_nlog = a6l_v11_irqn = 0;
	a6l_v11_irqcap = false;
""")

sub1(V48, """		 a6l_v10_phy_or[9], a6l_v10_phy_or[10], a6l_v10_fsr_or, a6l_v10_fsr_n, len ? buf : " none");
}
""", """		 a6l_v10_phy_or[9], a6l_v10_phy_or[10], a6l_v10_fsr_or, a6l_v10_fsr_n, len ? buf : " none");
	if (a6l_v11 & 1)
		a6l_v11_sum(vfe); /* camfix11 */
}
""")

# ---------------- CSID ----------------
sub1(CSID, "static bool a6l_v10_tg_forced[4]; /* camfix10 bit1 */\n",
     "static bool a6l_v10_tg_forced[4]; /* camfix10 bit1 */\nstatic bool a6l_v11_irq_on[4], a6l_v11_cid1[4]; /* camfix11 */\n")
sub1(CSID, "			val = phy->lane_cnt - 1;\n			val |= phy->lane_assign << 4;\n",
"""			val = phy->lane_cnt - 1;
			val |= phy->lane_assign << 4;
			if (a6l_v11_lassign) /* camfix11: lane_assign override */
				val = (val & ~0xffff0u) | ((a6l_v11_lassign & 0xffff) << 4);
""")
sub1(CSID, "			val = phy->csiphy_id << 17;\n			val |= 0x9;\n",
"""			val = phy->csiphy_id << 17;
			val |= 0x9;
			if (a6l_v11 & 2) /* camfix11: stock msm_csid CORE_CTRL_1 low nibble 0xF */
				val |= 0xf;
""")
sub1(CSID, "		writel_relaxed(val, csid->base + CAMSS_CSID_CID_n_CFG(cid));\n",
"""		writel_relaxed(val, csid->base + CAMSS_CSID_CID_n_CFG(cid));
		/* camfix11: CID0 DT override, extra DT on CID1 (stock imx576 csid_lut: cid0 0x2b, cid1 0x36) */
		if (!tg->enabled) {
			u32 lut = readl_relaxed(csid->base + CAMSS_CSID_CID_LUT_VC_n(vc));

			if (a6l_v11_dt0 && a6l_v11_dt0 < 0x40)
				lut = (lut & ~0xffu) | a6l_v11_dt0;
			if ((a6l_v11 & (4 | 16)) && a6l_v11_dt2 && a6l_v11_dt2 < 0x40) {
				lut = (lut & ~0xff00u) | (a6l_v11_dt2 << 8);
				writel_relaxed((a6l_v11 & 16) ? val : 0, csid->base + CAMSS_CSID_CID_n_CFG(cid + 1));
				a6l_v11_cid1[csid->id & 3] = true;
			}
			writel_relaxed(lut, csid->base + CAMSS_CSID_CID_LUT_VC_n(vc));
		}
		if (a6l_v11 & 8) { /* camfix11: stock IRQ mask + 4-lane overflow, counted in csid_isr */
			a6l_v11_irq_on[csid->id & 3] = true;
			writel_relaxed(0x7f010800 | 0x00f00000, csid->base + CAMSS_CSID_IRQ_MASK);
		}
		if (a6l_v11)
			dev_info(csid->camss->dev,
				 "A6L_V11 CSID%u v11 0x%02x tg %u ctrl0 %08x ctrl1 %08x lut0 %08x cid0 %08x cid1 %08x mask %08x dt0 0x%02x dt2 0x%02x lassign 0x%x\\n",
				 csid->id, a6l_v11, tg->enabled, readl_relaxed(csid->base + CAMSS_CSID_CORE_CTRL_0),
				 readl_relaxed(csid->base + CAMSS_CSID_CORE_CTRL_1),
				 readl_relaxed(csid->base + CAMSS_CSID_CID_LUT_VC_n(0)),
				 readl_relaxed(csid->base + CAMSS_CSID_CID_n_CFG(0)),
				 readl_relaxed(csid->base + CAMSS_CSID_CID_n_CFG(1)),
				 readl_relaxed(csid->base + CAMSS_CSID_IRQ_MASK), a6l_v11_dt0, a6l_v11_dt2, a6l_v11_lassign);
""")
sub1(CSID, "		a6l_v10_csid_off(); /* camfix10 */\n", """		a6l_v10_csid_off(); /* camfix10 */
		if (a6l_v11_irq_on[csid->id & 3]) { /* camfix11: back to the mainline mask (RST_DONE) */
			a6l_v11_irq_on[csid->id & 3] = false;
			writel_relaxed(BIT(11), csid->base + CAMSS_CSID_IRQ_MASK);
		}
		if (a6l_v11_cid1[csid->id & 3]) { /* camfix11: CID1 back to unmapped */
			a6l_v11_cid1[csid->id & 3] = false;
			writel_relaxed(0, csid->base + CAMSS_CSID_CID_n_CFG(1));
			writel_relaxed(readl_relaxed(csid->base + CAMSS_CSID_CID_LUT_VC_n(0)) & ~0xff00u,
				       csid->base + CAMSS_CSID_CID_LUT_VC_n(0));
		}
""")
sub1(CSID, "	writel_relaxed(value, csid->base + CAMSS_CSID_IRQ_CLEAR_CMD);\n", """	writel_relaxed(value, csid->base + CAMSS_CSID_IRQ_CLEAR_CMD);
	if (a6l_v11_irq_on[csid->id & 3] && a6l_v11_csid_irq(csid->id, value)) { /* camfix11 storm cap */
		writel_relaxed(BIT(11), csid->base + CAMSS_CSID_IRQ_MASK);
		dev_info(csid->camss->dev, "A6L_V11 CSID%u IRQ cap (4000): mask back to RST_DONE, last status %08x\\n",
			 csid->id, value);
	}
""")

# ---------------- CSIPHY ----------------
sub1(PHY, "	settle_cnt = csiphy_settle_cnt_calc(link_freq, csiphy->timer_clk_rate);\n", """	settle_cnt = csiphy_settle_cnt_calc(link_freq, csiphy->timer_clk_rate);
	if (a6l_v11_settle && a6l_v11_settle < 256) { /* camfix11 */
		dev_info(csiphy->camss->dev, "A6L_V11 CSIPHY%u settle_cnt %u (formula %u; stock imx576 14, s5k3t1 19)\\n",
			 csiphy->id, a6l_v11_settle, settle_cnt);
		settle_cnt = a6l_v11_settle;
	}
""")
sub1(PHY, """	val = 0x02;
	writel_relaxed(val, csiphy->base +
		       CSIPHY_3PH_CMN_CSI_COMMON_CTRLn(regs->offset, 7));
""", """	val = 0x02;
	if (a6l_v11) /* camfix11: reset value of COMMON_CTRL7 (stock v3.5 D-PHY never writes it) */
		dev_info(csiphy->camss->dev, "A6L_V11 CSIPHY%u ctrl7 before %02x a6l_v11_ctrl7 %d\\n", csiphy->id,
			 readl_relaxed(csiphy->base + CSIPHY_3PH_CMN_CSI_COMMON_CTRLn(regs->offset, 7)) & 0xff,
			 a6l_v11_ctrl7);
	if (a6l_v11_ctrl7 >= 0)
		val = a6l_v11_ctrl7 & 0xff;
	if (a6l_v11_ctrl7 != -2)
		writel_relaxed(val, csiphy->base +
			       CSIPHY_3PH_CMN_CSI_COMMON_CTRLn(regs->offset, 7));
""")
sub1(PHY, "		csiphy_gen1_config_lanes(csiphy, cfg, settle_cnt);\n", """		csiphy_gen1_config_lanes(csiphy, cfg, settle_cnt);
	if (a6l_v11) /* camfix11: read back what the lanes got */
		dev_info(csiphy->camss->dev,
			 "A6L_V11 CSIPHY%u cfg3(settle) ln0 %02x ln2 %02x ln4 %02x ln6 %02x clk %02x | cfg4 clk %02x | ctrl5 %02x ctrl6 %02x ctrl7 %02x\\n",
			 csiphy->id, readl_relaxed(csiphy->base + CSIPHY_3PH_LNn_CFG3(0)) & 0xff,
			 readl_relaxed(csiphy->base + CSIPHY_3PH_LNn_CFG3(2)) & 0xff,
			 readl_relaxed(csiphy->base + CSIPHY_3PH_LNn_CFG3(4)) & 0xff,
			 readl_relaxed(csiphy->base + CSIPHY_3PH_LNn_CFG3(6)) & 0xff,
			 readl_relaxed(csiphy->base + CSIPHY_3PH_LNn_CFG3(7)) & 0xff,
			 readl_relaxed(csiphy->base + CSIPHY_3PH_LNn_CFG4(7)) & 0xff,
			 readl_relaxed(csiphy->base + CSIPHY_3PH_CMN_CSI_COMMON_CTRLn(regs->offset, 5)) & 0xff,
			 readl_relaxed(csiphy->base + CSIPHY_3PH_CMN_CSI_COMMON_CTRLn(regs->offset, 6)) & 0xff,
			 readl_relaxed(csiphy->base + CSIPHY_3PH_CMN_CSI_COMMON_CTRLn(regs->offset, 7)) & 0xff);
""")
print("CAMFIX11_PATCH_OK")
