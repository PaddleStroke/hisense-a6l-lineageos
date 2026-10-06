#!/usr/bin/env python3
# camfix9 (30 Sep 2026): applied AFTER camfix_patch.py .. camfix8_patch.py.   usage: camfix9_patch.py <camss_dir>
# Input: attended run t31 (camera10 = camfix8; laptop ~/A6L-usb-20260915/v75/logs/t31-cam10.tar, copy in
# logs/t31-cam10/). See docs/camfix9-20260930.md.
#   - camfix8 REFUTED the MMPLL4 hypothesis: with MMPLL4 on+locked (c0000007) at the bus error (pll, pllclk, plliccmax,
#     hold) and with the MMSS AXI RCG moved to MMPLL0/2 = 404 MHz (post406: cfg 0x103, clk view rate 403999951,
#     parent mmpll0 hw_on 1) the stream fails the same way (1 bus error at SOF 2-6, then the WM stops).
#   - VBIF_STICK (0xac <- 0x40) reads 0 = "LOST" in EVERY variant, including post406 with a live AXI root.
#     clk_summary: camss_vfe_vbif_axi_clk and camss_vfe_vbif_ahb_clk enable 1 / hw Y; the MMCC CBCRs read on.
#     VBIF VERSION reads 0x20100000, the other registers read sane values. So "LOST" does NOT mean "unclocked".
#     Remaining explanations: (a) the bits written are not implemented on this VBIF instance (WRITE_GATHER_EN bit 6 =
#     xin6, OUT_RD_LIM xin1 field) -> RAZ/WI, the probe gave a false negative; (b) HLOS writes to the camera VBIF are
#     blocked (xPU / access control); (c) a clock-gated register domain that needs VBIF_CLKON (0x4) force-on.
#   - Stock msm_isp47/48 (camera_v2, LA.UM sdm660): VBIF_CLKON (0x4) bit0 is only forced during axi_halt and cleared at
#     the halt IRQ; the vbif-settings are written blind in init_hardware_reg; RDI write masters get bus_err_ign_mask
#     (stock IGNORES 0xC94 bus errors of RDI WMs).
#   - NOTE: vfe_gen1_enable calls set_qos (-> a6l_v8_start, the stick probe) BEFORE a6l_prepare_output (-> a6l_v8_on),
#     so in camfix8 the pll* stick probes ran before the PLL enable. posthold/post406 (persistent) are still valid.
# Changes: new runtime param qcom_camss.a6l_v9 (default 0x01 = diagnostics, every write restored). camfix1..8 knobs kept.
#   bit0 (1)  diagnostics: A6L_V9 VBIF <tag> full dump (CLKON 0x4, CLK_FORCE 0x8/0xc, QoS remap 0x20..0x2c, gather,
#             in/out limits 0xb0..0xdc, rrqos, amemtype, pnd/src err, halt 0x200/0x204) at PRE/START/BUSERR/STOP
#             (MMIO only, IRQ-safe); A6L_V9 CLK: enable state + rate of the camss VFE clocks (vfe_axi = vbif axi,
#             vfe_ahb = vbif ahb, throttle_axi); A6L_V9 STICK: per-register write-readback-restore tests on fields
#             that exist on every VBIF (0xd0 xin0 byte, 0x124, 0x160 xin0 nibble) + all-ones masks of 0xac / 0xd0 /
#             0x4 / 0x8 -> A6L_V9 VBIF_WR WRITABLE|READONLY (the corrected "vbif ok").
#   bit1 (2)  VBIF_CLKON (0x4) |= 1 (stock "vbif clk force on", used by msm_vfe47_axi_halt) from set_qos to stream-off.
#   bit2 (4)  VBIF_CLK_FORCE_CTRL0/1 (0x8/0xc) = 0xffffffff (force every xin clock on) from set_qos to stream-off.
#   bit3 (8)  power VFE1 (vfe_get: camss_vfe1 GDSC + VFE1 clocks) for the stream: the VBIF is shared with VFE1
#             (stock DT: both vfe nodes map vfe_vbif 0xca40000); xin1 never acks halt with VFE1 collapsed.
#   bit4 (16) re-apply the stock vbif-settings (0x124=3, 0xac=0x40, 0xd0=0x1010) AFTER bits 1..3, keep them, and log
#             the readback (A6L_V9 VBIF_APPLY).
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
G1 = f"{cs}/camss-vfe-gen1.c"

sub1(H, "void a6l_v8_start(struct vfe_device *vfe);\n",
"""void a6l_v8_start(struct vfe_device *vfe);
/* A6L camfix9 (camss-vfe-4-8.c) */
void a6l_v9_off(struct vfe_device *vfe);
void a6l_v9_regs(struct vfe_device *vfe, const char *tag);
void a6l_v9_start(struct vfe_device *vfe);
""")

sub1(V48, "\t\t\t rd == 0x40 ? \"ok\" : \"LOST\");\n\t}\n}\n",
r'''			 rd == 0x40 ? "ok" : "LOST");
	}
}

/* ================= camfix9: camera VBIF (0xca40000) writability / clock force / VFE1 domain ================= */
static uint a6l_v9 = 0x01;
module_param(a6l_v9, uint, 0644);
MODULE_PARM_DESC(a6l_v9, "A6L camfix9: bit0 VBIF full dump + per-register write-stick tests + VBIF clock view, bit1 VBIF_CLKON(0x4) bit0 force-on during the stream, bit2 VBIF_CLK_FORCE_CTRL0/1 = all ones during the stream, bit3 power VFE1 (shared VBIF) during the stream, bit4 re-apply the stock VBIF settings after bits 1-3 (default 0x01)");

static u32 a6l_v9_clkon_old, a6l_v9_fc0_old, a6l_v9_fc1_old;
static bool a6l_v9_clkon_set, a6l_v9_fc_set, a6l_v9_vfe1_on;

/* MMIO only: safe in the VFE IRQ handler (first bus error) */
void a6l_v9_regs(struct vfe_device *vfe, const char *tag)
{
	void __iomem *b = a6l_vbif_base;

	if (!(a6l_v9 & 1) || !b || vfe->power_count <= 0)
		return;
	dev_info(vfe->camss->dev,
		 "A6L_V9 VBIF %s ver %08x clkon(4) %08x fc0(8) %08x fc1(c) %08x qrm(20..2c) %08x %08x %08x %08x wg(ac) %08x inrd(b0..bc) %08x %08x %08x %08x inwr(c0..cc) %08x %08x %08x %08x outrd(d0/d4) %08x %08x outwr(d8/dc) %08x %08x rrqos %08x amem(160/164) %08x %08x pnd %08x src %08x halt %08x %08x\n",
		 tag, readl_relaxed(b), readl_relaxed(b + 0x4), readl_relaxed(b + 0x8), readl_relaxed(b + 0xc),
		 readl_relaxed(b + 0x20), readl_relaxed(b + 0x24), readl_relaxed(b + 0x28), readl_relaxed(b + 0x2c),
		 readl_relaxed(b + 0xac), readl_relaxed(b + 0xb0), readl_relaxed(b + 0xb4), readl_relaxed(b + 0xb8),
		 readl_relaxed(b + 0xbc), readl_relaxed(b + 0xc0), readl_relaxed(b + 0xc4), readl_relaxed(b + 0xc8),
		 readl_relaxed(b + 0xcc), readl_relaxed(b + 0xd0), readl_relaxed(b + 0xd4), readl_relaxed(b + 0xd8),
		 readl_relaxed(b + 0xdc), readl_relaxed(b + 0x124), readl_relaxed(b + 0x160), readl_relaxed(b + 0x164),
		 readl_relaxed(b + 0x190), readl_relaxed(b + 0x194), readl_relaxed(b + 0x200), readl_relaxed(b + 0x204));
}

/* write val, read back, restore the old value; returns the readback */
static u32 a6l_v9_try(void __iomem *r, u32 val, u32 *oldp)
{
	u32 old = readl_relaxed(r), rd;

	writel_relaxed(val, r);
	wmb();
	udelay(2);
	rd = readl_relaxed(r);
	writel_relaxed(old, r);
	wmb();
	if (oldp)
		*oldp = old;
	return rd;
}

static void a6l_v9_stick(struct vfe_device *vfe)
{
	void __iomem *b = a6l_vbif_base;
	u32 o1, o2, o3, o4, o5, o6, r1, r2, r3, m_ac, m_d0, m_4, m_8, w1, w2, w3;
	bool ok1, ok2, ok3;

	/* fields that exist on every VBIF: xin0 byte of OUT_RD_LIM_CONF0, RR QoS arb, xin0 nibble of AMEMTYPE */
	o1 = readl_relaxed(b + 0xd0);
	w1 = (o1 & ~0xffu) | ((o1 & 0xff) ^ 0x01);
	r1 = a6l_v9_try(b + 0xd0, w1, NULL);
	o2 = readl_relaxed(b + 0x124);
	w2 = o2 ^ 0x1;
	r2 = a6l_v9_try(b + 0x124, w2, NULL);
	o3 = readl_relaxed(b + 0x160);
	w3 = o3 ^ 0x1;
	r3 = a6l_v9_try(b + 0x160, w3, NULL);
	/* implemented-bit masks (all ones, restored at once; before the WMs are enabled) */
	m_ac = a6l_v9_try(b + 0xac, 0xffffffff, &o4);
	m_d0 = a6l_v9_try(b + 0xd0, 0xffffffff, NULL);
	m_4 = a6l_v9_try(b + 0x4, (a6l_v9_clkon_set ? readl_relaxed(b + 0x4) : 0) | 0x1, &o5);
	m_8 = a6l_v9_try(b + 0x8, 0xffffffff, &o6);
	ok1 = r1 == w1;
	ok2 = r2 == w2;
	ok3 = r3 == w3;
	dev_info(vfe->camss->dev,
		 "A6L_V9 STICK d0 old %08x wrote %08x read %08x %s | 124 old %08x wrote %08x read %08x %s | 160 old %08x wrote %08x read %08x %s\n",
		 o1, w1, r1, ok1 ? "ok" : "LOST", o2, w2, r2, ok2 ? "ok" : "LOST", o3, w3, r3, ok3 ? "ok" : "LOST");
	dev_info(vfe->camss->dev,
		 "A6L_V9 MASK ac old %08x ones-> %08x | d0 ones-> %08x | clkon old %08x bit0-> %08x | fc0 old %08x ones-> %08x | now ac %08x d0 %08x clkon %08x fc0 %08x\n",
		 o4, m_ac, m_d0, o5, m_4, o6, m_8, readl_relaxed(b + 0xac), readl_relaxed(b + 0xd0),
		 readl_relaxed(b + 0x4), readl_relaxed(b + 0x8));
	dev_info(vfe->camss->dev, "A6L_V9 VBIF_WR %s (%u/3 known fields stick; gather-bit6 %s, outrd-xin1 %s, clkon %s)\n",
		 (ok1 || ok2 || ok3) ? "WRITABLE" : "READONLY", ok1 + ok2 + ok3,
		 (m_ac & 0x40) ? "impl" : "RAZ", (m_d0 & 0xff00) ? "impl" : "RAZ", (m_4 & 1) ? "impl" : "RAZ");
}

static void a6l_v9_clkview(struct vfe_device *vfe)
{
	unsigned int i;

	for (i = 0; i < vfe->nclocks; i++) {
		struct camss_clock *c = &vfe->clock[i];

		if (!c->clk || !c->name || !(strstr(c->name, "axi") || strstr(c->name, "vfe_ahb")))
			continue;
		dev_info(vfe->camss->dev, "A6L_V9 CLK %s (%s) enabled %d prepared %d rate %lu\n", c->name,
			 __clk_get_name(c->clk), __clk_is_enabled(c->clk), clk_hw_is_prepared(__clk_get_hw(c->clk)),
			 clk_get_rate(c->clk));
	}
}

/* vfe_set_qos: stream-on, process context, VFE0 powered, runs BEFORE a6l_prepare_output and the WM enable */
void a6l_v9_start(struct vfe_device *vfe)
{
	void __iomem *b;
	struct camss *camss = vfe->camss;

	if (!a6l_v9)
		return;
	a6l_v6_map();
	b = a6l_vbif_base;
	if (!b || vfe->power_count <= 0)
		return;
	a6l_v9_regs(vfe, "PRE");
	if ((a6l_v9 & 8) && !a6l_v9_vfe1_on && vfe->id == 0 && camss->res->vfe_num > 1) {
		int r = vfe_get(&camss->vfe[1]);

		a6l_v9_vfe1_on = !r;
		dev_info(camss->dev, "A6L_V9 VFE1 power on %d (power_count %d)\n", r, camss->vfe[1].power_count);
	}
	if ((a6l_v9 & 2) && !a6l_v9_clkon_set) {
		a6l_v9_clkon_old = readl_relaxed(b + 0x4);
		writel_relaxed(a6l_v9_clkon_old | 0x1, b + 0x4);
		wmb();
		a6l_v9_clkon_set = true;
		dev_info(camss->dev, "A6L_V9 CLKON %08x -> %08x\n", a6l_v9_clkon_old, readl_relaxed(b + 0x4));
	}
	if ((a6l_v9 & 4) && !a6l_v9_fc_set) {
		a6l_v9_fc0_old = readl_relaxed(b + 0x8);
		a6l_v9_fc1_old = readl_relaxed(b + 0xc);
		writel_relaxed(0xffffffff, b + 0x8);
		writel_relaxed(0xffffffff, b + 0xc);
		wmb();
		a6l_v9_fc_set = true;
		dev_info(camss->dev, "A6L_V9 CLKFORCE fc0 %08x -> %08x fc1 %08x -> %08x\n", a6l_v9_fc0_old,
			 readl_relaxed(b + 0x8), a6l_v9_fc1_old, readl_relaxed(b + 0xc));
	}
	if (a6l_v9 & 1) {
		a6l_v9_clkview(vfe);
		a6l_v9_stick(vfe);
	}
	if (a6l_v9 & 16) {
		writel_relaxed(0x3, b + 0x124);
		writel_relaxed(0x40, b + 0xac);
		writel_relaxed(0x1010, b + 0xd0);
		wmb();
		dev_info(camss->dev, "A6L_V9 VBIF_APPLY rrqos %08x wgather %08x outrd %08x (want 3/40/1010)\n",
			 readl_relaxed(b + 0x124), readl_relaxed(b + 0xac), readl_relaxed(b + 0xd0));
	}
	a6l_v9_regs(vfe, "START");
}

/* stream-off (process context), after the VFE halt */
void a6l_v9_off(struct vfe_device *vfe)
{
	void __iomem *b = a6l_vbif_base;

	if (b && vfe->power_count > 0) {
		if (a6l_v9_clkon_set)
			writel_relaxed(a6l_v9_clkon_old, b + 0x4);
		if (a6l_v9_fc_set) {
			writel_relaxed(a6l_v9_fc0_old, b + 0x8);
			writel_relaxed(a6l_v9_fc1_old, b + 0xc);
		}
		wmb();
		if (a6l_v9_clkon_set || a6l_v9_fc_set)
			dev_info(vfe->camss->dev, "A6L_V9 restored clkon %08x fc0 %08x fc1 %08x\n",
				 readl_relaxed(b + 0x4), readl_relaxed(b + 0x8), readl_relaxed(b + 0xc));
	}
	a6l_v9_clkon_set = false;
	a6l_v9_fc_set = false;
	if (a6l_v9_vfe1_on) {
		vfe_put(&vfe->camss->vfe[1]);
		a6l_v9_vfe1_on = false;
		dev_info(vfe->camss->dev, "A6L_V9 VFE1 power released\n");
	}
}
''')

sub1(V48, "\t\t\ta6l_v8_regs(vfe, \"BUSERR\"); /* camfix8 */\n",
     "\t\t\ta6l_v8_regs(vfe, \"BUSERR\"); /* camfix8 */\n\t\t\ta6l_v9_regs(vfe, \"BUSERR\"); /* camfix9 */\n")
sub1(V48, "\ta6l_v8_regs(vfe, tag); /* camfix8 */\n",
     "\ta6l_v8_regs(vfe, tag); /* camfix8 */\n\ta6l_v9_regs(vfe, tag); /* camfix9 */\n")
sub1(V48, "\ta6l_v8_start(vfe); /* camfix8 */\n",
     "\ta6l_v8_start(vfe); /* camfix8 */\n\ta6l_v9_start(vfe); /* camfix9 */\n")
sub1(G1, "\ta6l_v8_off(vfe); /* camfix8 */\n",
     "\ta6l_v8_off(vfe); /* camfix8 */\n\ta6l_v9_off(vfe); /* camfix9 */\n")
print("CAMFIX9_PATCH_OK")
