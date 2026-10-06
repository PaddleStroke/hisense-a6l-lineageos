#!/usr/bin/env python3
# camfix8 (30 Sep 2026): applied AFTER camfix_patch.py .. camfix7_patch.py.   usage: camfix8_patch.py <camss_dir>
# Input: attended run t30 (camera9 = camfix7; laptop ~/A6L-usb-20260915/v75/logs/t30-cam9.tar, copy in
# logs/t30-cam9/). See docs/camfix8-20260930.md.
#   - All 10 camfix7 sweep variants fail (SMMU impl-defs, ACTLR, NoC/SMMU clocks, ICC max, half/quarter line rate);
#     the front camera s5k3t1 fails identically -> shared VFE/VBIF/MMSS-NoC path.
#   - SMMU cb0 FSR 0x00000400 is FSR.FORMAT = 0b10 (AArch64 context format, bits [10:9], read-only info), NOT a fault;
#     GFSR 0, FAR 0: the SMMU saw no fault at all.
#   - A6L_MMCC: axi_clk_src (MMCC 0xd000, RPM-owned MMSS NoC AXI) = cfg 0x203 = src 2 = MMPLL4 / 2, but MMPLL4 MODE
#     reads 0x00000000 (OUTCTRL, BYPASSNL, RESET_N all clear, no LOCK_DET) at START, BUSERR and STOP in all 142
#     dumps. clk_summary: mmpll4 enable 0 prepare 0 hw-enabled N; axi_clk_src (child of mmpll4) enable 0.
#     The branch CLK_OFF "on" readings are sampled status bits and go stale when the root stops.
#   - VBIF QoS writes (0xac, 0xd0) never stick (camfix6/7), consistent with an unclocked VBIF core (AXI domain).
# Hypothesis: the MMSS NoC AXI clock has no running source; the VFE trickles a few lines (<= 17 per frame) and the
# first AXI write that times out gives the 0xC94 bus error.
# Changes: new runtime param qcom_camss.a6l_v8 (default 0x01 = read-only diagnostics). All camfix1..7 knobs are kept.
#   bit0 (1)  diagnostics: A6L_V8 <tag> mmpll4 MODE/L/ALPHA/USER/CONFIG/TEST/STATUS + axi RCG + mmpll0 mode at
#             START/BUSERR/STOP (MMIO only, IRQ-safe); clk-framework view of axi_clk_src and its parent
#             (A6L_V8 CLK ...); VBIF write-stick probe on 0xac (A6L_V8 VBIF_STICK ... ok|LOST).
#   bit1 (2)  clk_prepare_enable(axi_clk_src) from stream-on to stream-off -> the clk core enables its current parent
#             (MMPLL4, L 0x28 = 768 MHz) and waits for LOCK_DET. Logs mode before/after.
#   bit2 (4)  with bit1: clk_set_rate(axi_clk_src, 406000000) = MMPLL0 / 2 (stock/RPM maximum, MMPLL0 locked).
#             The RCG stays on MMPLL0 until reboot (or until the RPM reprograms it).
#   bit3 (8)  with bit1: keep the axi_clk_src enable until reboot (no release at stream-off).
#   bit4 (16) raw MMIO enable of MMPLL4 (BYPASSNL, 5 us, RESET_N, wait LOCK_DET <= 1 ms, OUTCTRL), no clk framework;
#             stays on until reboot. Fallback in case the framework enable fails.
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

sub1(H, "void a6l_v7_clk_off(struct vfe_device *vfe);\n",
"""void a6l_v7_clk_off(struct vfe_device *vfe);
/* A6L camfix8 (camss-vfe-4-8.c) */
void a6l_v8_on(struct vfe_device *vfe);
void a6l_v8_off(struct vfe_device *vfe);
void a6l_v8_regs(struct vfe_device *vfe, const char *tag);
void a6l_v8_start(struct vfe_device *vfe);
""")

sub1(V48, "\ta6l_v7_clk_held = false;\n\tdev_info(vfe->camss->dev, \"A6L_V7CLK released\\n\");\n}\n",
r'''	a6l_v7_clk_held = false;
	dev_info(vfe->camss->dev, "A6L_V7CLK released\n");
}

/* ================= camfix8: MMSS NoC AXI clock source (axi_clk_src -> MMPLL4) ================= */
static uint a6l_v8 = 0x01;
module_param(a6l_v8, uint, 0644);
MODULE_PARM_DESC(a6l_v8, "A6L camfix8: bit0 MMSS AXI/MMPLL4 diagnostics + VBIF write-stick probe, bit1 enable axi_clk_src (and so its PLL) for the stream, bit2 +set axi_clk_src 406 MHz (MMPLL0/2, persists), bit3 keep the bit1 enable until reboot, bit4 raw MMIO enable of MMPLL4 (persists) (default 0x01)");

#define A6L_MMPLL4	0x50
static struct clk *a6l_v8_axi;
static bool a6l_v8_keep;

/* MMIO only: safe in the VFE IRQ handler (first bus error) */
void a6l_v8_regs(struct vfe_device *vfe, const char *tag)
{
	void __iomem *p;
	u32 m;

	if (!(a6l_v8 & 1) || !a6l_mmcc)
		return;
	p = a6l_mmcc + A6L_MMPLL4;
	m = readl_relaxed(p);
	dev_info(vfe->camss->dev,
		 "A6L_V8 %s mmpll4 mode %08x (out %u bypassnl %u reset_n %u fsm %u lock %u) L %08x alpha %08x user %08x config %08x test %08x status %08x | axi rcg %08x/%08x | mmpll0 mode %08x\n",
		 tag, m, m & 1, !!(m & BIT(1)), !!(m & BIT(2)), !!(m & BIT(20)), !!(m & BIT(31)),
		 readl_relaxed(p + 0x4), readl_relaxed(p + 0x8), readl_relaxed(p + 0x10), readl_relaxed(p + 0x18),
		 readl_relaxed(p + 0x1c), readl_relaxed(p + 0x24), readl_relaxed(a6l_mmcc + 0xd000),
		 readl_relaxed(a6l_mmcc + 0xd004), readl_relaxed(a6l_mmcc + 0xc000));
}

static void a6l_v8_clkview(struct vfe_device *vfe, const char *tag, struct clk *c)
{
	struct clk *p = clk_get_parent(c);

	dev_info(vfe->camss->dev, "A6L_V8 CLK %s %s hw_on %d rate %lu parent %s hw_on %d rate %lu | mmpll4 mode %08x rcg %08x/%08x\n",
		 tag, __clk_get_name(c), __clk_is_enabled(c), clk_get_rate(c), p ? __clk_get_name(p) : "-",
		 p ? __clk_is_enabled(p) : -1, p ? clk_get_rate(p) : 0, readl_relaxed(a6l_mmcc + A6L_MMPLL4),
		 readl_relaxed(a6l_mmcc + 0xd000), readl_relaxed(a6l_mmcc + 0xd004));
}

/* bit4: clk_alpha_pll_enable() sequence by hand (DEFAULT alpha PLL: MODE +0, LOCK_DET bit31) */
static void a6l_v8_raw_pll(struct vfe_device *vfe)
{
	void __iomem *p = a6l_mmcc + A6L_MMPLL4;
	u32 m0 = readl_relaxed(p), m;
	int ret;

	if ((m0 & 7) == 7 && (m0 & BIT(31))) {
		dev_info(vfe->camss->dev, "A6L_V8 RAWPLL already on (mode %08x)\n", m0);
		return;
	}
	writel_relaxed(m0 | BIT(1), p);			/* BYPASSNL */
	mb();
	udelay(5);
	writel_relaxed(readl_relaxed(p) | BIT(2), p);	/* RESET_N */
	ret = readl_poll_timeout_atomic(p, m, m & BIT(31), 5, 1000);
	if (!ret)
		writel_relaxed(readl_relaxed(p) | BIT(0), p);	/* OUTCTRL */
	mb();
	dev_info(vfe->camss->dev, "A6L_V8 RAWPLL mmpll4 mode %08x -> %08x (%s)\n", m0, readl_relaxed(p),
		 ret ? "NO LOCK" : "locked");
}

static struct clk *a6l_v8_get_axi(struct vfe_device *vfe)
{
	struct of_phandle_args args = { .args_count = 1 };
	struct clk *c;

	args.np = of_find_compatible_node(NULL, NULL, "qcom,mmcc-sdm660");
	if (!args.np) {
		dev_info(vfe->camss->dev, "A6L_V8 no mmcc node\n");
		return NULL;
	}
	args.args[0] = AXI_CLK_SRC;
	c = of_clk_get_from_provider(&args);
	of_node_put(args.np);
	if (IS_ERR(c)) {
		dev_info(vfe->camss->dev, "A6L_V8 axi_clk_src get %ld\n", PTR_ERR(c));
		return NULL;
	}
	return c;
}

/* stream-on, process context (a6l_prepare_output), before vfe_set_qos / the VBIF writes */
void a6l_v8_on(struct vfe_device *vfe)
{
	struct clk *c;
	int r;

	if (!a6l_v8)
		return;
	a6l_v7_map();
	if (!a6l_mmcc)
		return;
	if (a6l_v8 & 16)
		a6l_v8_raw_pll(vfe);
	if ((a6l_v8 & 1) && !(a6l_v8 & 2)) {
		c = a6l_v8_get_axi(vfe);
		if (c) {
			a6l_v8_clkview(vfe, "PRE", c);
			clk_put(c);
		}
	}
	if (!(a6l_v8 & 2))
		return;
	if (!a6l_v8_axi) {
		c = a6l_v8_get_axi(vfe);
		if (!c)
			return;
		a6l_v8_clkview(vfe, "PRE", c);
		r = clk_prepare_enable(c);
		dev_info(vfe->camss->dev, "A6L_V8 AXI enable %d\n", r);
		if (r) {
			clk_put(c);
			return;
		}
		a6l_v8_axi = c;
		a6l_v8_clkview(vfe, "ON", c);
	}
	if (a6l_v8 & 4) {
		r = clk_set_rate(a6l_v8_axi, 406000000);
		dev_info(vfe->camss->dev, "A6L_V8 AXI set_rate 406000000 -> %d\n", r);
		a6l_v8_clkview(vfe, "RATE", a6l_v8_axi);
	}
	if (a6l_v8 & 8)
		a6l_v8_keep = true;
}

void a6l_v8_off(struct vfe_device *vfe)
{
	if (!a6l_v8_axi)
		return;
	if (a6l_v8_keep) {
		dev_info(vfe->camss->dev, "A6L_V8 AXI kept on (until reboot), mmpll4 mode %08x\n",
			 readl_relaxed(a6l_mmcc + A6L_MMPLL4));
		return;
	}
	clk_disable_unprepare(a6l_v8_axi);
	a6l_v8_clkview(vfe, "OFF", a6l_v8_axi);
	clk_put(a6l_v8_axi);
	a6l_v8_axi = NULL;
}

/* vfe_set_qos (stream-on, process context, VFE clocks on): START dump + VBIF write-stick probe */
void a6l_v8_start(struct vfe_device *vfe)
{
	a6l_v7_map();
	a6l_v8_regs(vfe, "START");
	if ((a6l_v8 & 1) && a6l_vbif_base && vfe->power_count > 0) {
		u32 old = readl_relaxed(a6l_vbif_base + 0xac), rd;

		writel_relaxed(0x40, a6l_vbif_base + 0xac);	/* stock vbif-settings value */
		wmb();
		udelay(2);
		rd = readl_relaxed(a6l_vbif_base + 0xac);
		if (!(a6l_v6 & 1))
			writel_relaxed(old, a6l_vbif_base + 0xac);
		dev_info(vfe->camss->dev, "A6L_V8 VBIF_STICK wgather old %08x wrote 00000040 read %08x -> %s\n", old, rd,
			 rd == 0x40 ? "ok" : "LOST");
	}
}
''')

sub1(V48, "\t\t\ta6l_v7_regs(vfe, \"BUSERR\");\n",
     "\t\t\ta6l_v7_regs(vfe, \"BUSERR\");\n\t\t\ta6l_v8_regs(vfe, \"BUSERR\"); /* camfix8 */\n")
sub1(V48, "\ta6l_v7_regs(vfe, tag); /* camfix7 */\n",
     "\ta6l_v7_regs(vfe, tag); /* camfix7 */\n\ta6l_v8_regs(vfe, tag); /* camfix8 */\n")
sub1(V48, "\ta6l_v7_start(vfe); /* camfix7 */\n",
     "\ta6l_v7_start(vfe); /* camfix7 */\n\ta6l_v8_start(vfe); /* camfix8 */\n")
sub1(G1, "\ta6l_v7_clk_on(vfe); /* camfix7 bit2 */\n",
     "\ta6l_v8_on(vfe); /* camfix8: MMSS AXI source */\n\ta6l_v7_clk_on(vfe); /* camfix7 bit2 */\n")
sub1(G1, "\ta6l_v7_clk_off(vfe); /* camfix7 bit2 */\n",
     "\ta6l_v7_clk_off(vfe); /* camfix7 bit2 */\n\ta6l_v8_off(vfe); /* camfix8 */\n")
print("CAMFIX8_PATCH_OK")
