#!/usr/bin/env python3
# camfix7 (29 Sep 2026): applied AFTER camfix_patch.py .. camfix6_patch.py.   usage: camfix7_patch.py <camss_dir>
# Input: attended run t29 (camera7/camera8 = camfix6; laptop ~/A6L-usb-20260915/v75/logs/t29/cam29.tgz, cam29b.tgz;
# phone-side summaries in .relay/outbox/t29-46-bars.out, t29-48-sw2.out, t29-80-cam.out). See docs/camfix7-20260929.md.
#   - Every camfix6 variant (VBIF, UB 1904/192, ICC 2/4 and 6/8 GB/s, recovery, burst3, line-based, fix6) fails the
#     same way: ONE WM0 bus error (s1 bit4, 0xC94 bit0) at SOF 2..5, then the WM limps (<= 17 lines per frame, 40..100
#     lines in total) and almost never completes a frame (best: iccmax 3 dones, icc 2, burst3 1).
#   - s1 bits 9..15 (image-master UB overflow) are NEVER raised: 0xC94 is a separate per-WM *bus error* (AXI side),
#     not a UB overflow. Stock msm_vfe48_get_bus_err_mask reads 0xC94, masks the software ignore mask (scratch WMs only)
#     and clears s1 bit4 when nothing is left; s1 bit4 is in the stock overflow mask 0x09fffe7e -> halt + recovery.
#   - VBIF: the camfix6 writes did NOT stick (START readback wgather(ac) 0, outrd(d0) 0x10 with v6 bit0 set);
#     VBIF pnderr/srcerr stay 0.
#   - A6L_AXI "~0 kHz" was a decode bug: cfg 0x203 = source 2 = MMPLL4 (768 MHz, L=0x28), divider 2 -> 384 MHz.
#   - No "Unhandled context fault" in any log, but nobody has read the SMMU fault registers themselves.
# Stock comparison (firmware/extracted/device-trees/stock-00.dts):
#   a) arm,smmu-mmss@cd00000 has attach-impl-defs (24 implementation-defined registers 0x6000..0x6b84: micro-TLB,
#      prefetch, arbitration). msm-4.4 qsmmuv2_device_reset writes them under a local halt (0x6000 bit2, wait bit3).
#      Mainline arm-smmu-qcom (qcom,sdm630-smmu-v2) never writes them. msm-4.4 also sets CB ACTLR ISH|OSH|NSH.
#   b) qcom,vfe0 clocks include mmssnoc_axi, mnoc_ahb, bimc_smmu_ahb, bimc_smmu_axi (upstream camss holds none of
#      them; the SMMU driver owns the three mmcc ones) and a 100 GB/s initial msm-bus vote.
# Changes: new runtime param qcom_camss.a6l_v7 (default 0x01 = read-only diagnostics). All camfix1..6 knobs are kept.
#   bit0 (1)  diagnostics: A6L_MMCC (CBCR CLK_OFF of the camera/NoC/SMMU branches, GDSC PWR_ON, MMPLL0/4/7/10 lock,
#             AXI/AHB/VFE0 RCG), A6L_SMMU (sCR0, GFSR, SMR/S2CR of SIDs 0xc00..0xc03, their context bank
#             SCTLR/ACTLR/TCR/FSR/FAR/FSYNR/CBAR/CBFRSYNRA, impl-def readback) at START / first BUSERR / STOP. SMMU reads
#             only while the BIMC SMMU GDSC reports PWR_ON and bimc_smmu_ahb runs (no unclocked access).
#   bit1 (2)  program the stock mmss SMMU attach-impl-defs under a local halt, at stream-on (process context).
#             They stay programmed until reboot.
#   bit2 (4)  hold mnoc_ahb, bimc_smmu_ahb, bimc_smmu_axi, camss_micro_ahb (of_clk_get_from_provider on mmcc) for
#             the duration of the stream, like the stock vfe0 clock list.
#   bit3 (8)  stock msm-4.4 CB ACTLR (ISH|OSH|NSH = 0x70000000) on the context banks of SIDs 0xc00..0xc03.
#   The A6L_AXI line now decodes MMPLL4/7/10 correctly.
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

# ---- header: prototypes (no -Wmissing-prototypes) -----------------------------------------------------------------
sub1(H, "#endif /* QC_MSM_CAMSS_VFE_H */",
r'''/* A6L camfix7 (camss-vfe-4-8.c) */
void a6l_v7_clk_on(struct vfe_device *vfe);
void a6l_v7_clk_off(struct vfe_device *vfe);

#endif /* QC_MSM_CAMSS_VFE_H */''')

# ---- 4-8: includes ------------------------------------------------------------------------------------------------
sub1(V48, "#include <linux/iopoll.h>\n",
"#include <linux/iopoll.h>\n#include <linux/clk.h>\n#include <linux/clk-provider.h>\n#include <linux/of.h>\n"
"#include <dt-bindings/clock/qcom,mmcc-sdm660.h>\n")

# ---- 4-8: AXI decode fix (upstream mmcc parent map: xo, mmpll0, mmpll4, mmpll7, mmpll10, gpll0, gpll0_div) ----------
sub1(V48, "\tstatic const u32 src_khz[8] = { 19200, 808000, 0, 0, 0, 600000, 300000, 0 };\n",
     "\tstatic const u32 src_khz[8] = { 19200, 808000, 768000, 960000, 576000, 600000, 300000, 0 }; /* camfix7 */\n")

# ---- 4-8: camfix7 code, inserted before the camfix4 WM MAX block (i.e. before a6l_vfe_count / vfe_set_qos) --------
sub1(V48, "/* camfix4: stock msm_vfe47_update_ping_pong_addr writes WR_PING/PONG_MAX_ADDR = (addr + size) & ~0x1f */\n",
r'''/* ================= camfix7: MMCC / SMMU diagnostics, stock SMMU impl-defs, stock NoC/SMMU clocks ================= */
static uint a6l_v7 = 0x01;
module_param(a6l_v7, uint, 0644);
MODULE_PARM_DESC(a6l_v7, "A6L camfix7: bit0 MMCC/SMMU register diagnostics (read-only), bit1 stock mmss SMMU attach-impl-defs, bit2 stock NoC/SMMU clocks (mnoc_ahb, bimc_smmu_ahb/axi, camss_micro_ahb), bit3 stock CB ACTLR 0x70000000 (default 0x01)");

#define A6L_MMCC_PHYS	0x0c8c0000
#define A6L_SMMU_PHYS	0x0cd00000
#define A6L_SMMU_SIZE	0x40000
static void __iomem *a6l_mmcc, *a6l_smmu;
static bool a6l_v7_be_logged[2];

/* stock-00.dts arm,smmu-mmss@cd00000 attach-impl-defs (offset, value), written by msm-4.4 qsmmuv2_device_reset */
static const u32 a6l_smmu_impl[][2] = {
	{ 0x6000, 0x2378 }, { 0x6060, 0x1055 }, { 0x678c, 0x28 }, { 0x6794, 0xe0 }, { 0x6800, 0x06 }, { 0x6900, 0x3ff },
	{ 0x6924, 0x204 }, { 0x6928, 0x11002 }, { 0x6930, 0x800 }, { 0x6960, 0xffffffff }, { 0x6964, 0xffffffff },
	{ 0x6968, 0xffffffff }, { 0x696c, 0xffffffff }, { 0x6b48, 0x330330 }, { 0x6b4c, 0x81 }, { 0x6b50, 0x3333 },
	{ 0x6b54, 0x3333 }, { 0x6b64, 0x1a5555 }, { 0x6b68, 0xbaaa892a }, { 0x6b70, 0x10100202 }, { 0x6b74, 0x10100202 },
	{ 0x6b78, 0x10100000 }, { 0x6b80, 0x20042004 }, { 0x6b84, 0x20042004 },
};

/* process context only (ioremap may sleep) */
static void a6l_v7_map(void)
{
	if (!a6l_mmcc)
		a6l_mmcc = ioremap(A6L_MMCC_PHYS, 0x10000);
	if (!a6l_smmu)
		a6l_smmu = ioremap(A6L_SMMU_PHYS, A6L_SMMU_SIZE);
}

/* the SMMU registers are only touched while its GDSC is on and its AHB clock runs (no unclocked access) */
static bool a6l_smmu_ok(void)
{
	if (!a6l_mmcc || !a6l_smmu)
		return false;
	return (readl_relaxed(a6l_mmcc + 0xe020) & BIT(31)) && !(readl_relaxed(a6l_mmcc + 0xe004) & BIT(31));
}

static u32 a6l_smmu_pgsz(void)
{
	return (readl_relaxed(a6l_smmu + 0x24) & BIT(31)) ? 0x10000 : 0x1000;
}

static void __iomem *a6l_smmu_cb(u32 cb)
{
	u32 idr1 = readl_relaxed(a6l_smmu + 0x24), pg = a6l_smmu_pgsz();
	u32 npage = 1U << (((idr1 >> 28) & 7) + 1);

	if ((npage + cb + 1) * pg > A6L_SMMU_SIZE)
		return NULL;
	return a6l_smmu + npage * pg + cb * pg;
}

static bool a6l_smr_is_cam(u32 id, u32 mask)
{
	u32 s;

	for (s = 0xc00; s <= 0xc03; s++)
		if (!((s ^ id) & ~mask & 0x7fff))
			return true;
	return false;
}

static void a6l_cb_add(u32 *cbs, unsigned int *k, unsigned int max, u32 s2cr)
{
	unsigned int j;

	if (((s2cr >> 16) & 3) != 0) /* not a translation context */
		return;
	for (j = 0; j < *k; j++)
		if (cbs[j] == (s2cr & 0xff))
			return;
	if (*k < max)
		cbs[(*k)++] = s2cr & 0xff;
}

/* context banks that SIDs 0xc00..0xc03 translate through (stream matching or stream indexing) */
static unsigned int a6l_smmu_cam_cbs(u32 *cbs, unsigned int max, struct vfe_device *vfe, bool log)
{
	u32 idr0 = readl_relaxed(a6l_smmu + 0x20), n = idr0 & 0xff, i;
	unsigned int k = 0;

	if (!(idr0 & BIT(27))) { /* stream indexing: S2CR[SID] */
		for (i = 0xc00; i <= 0xc03; i++) {
			u32 s2cr = readl_relaxed(a6l_smmu + 0xc00 + 4 * i);

			if (log)
				dev_info(vfe->camss->dev, "A6L_SMMU S2CR[sid %03x] %08x\n", i, s2cr);
			a6l_cb_add(cbs, &k, max, s2cr);
		}
		return k;
	}
	for (i = 0; i < n && i < 128; i++) {
		u32 smr = readl_relaxed(a6l_smmu + 0x800 + 4 * i), s2cr, id, mask;

		if (!(smr & BIT(31)))
			continue;
		id = smr & 0x7fff;
		mask = (smr >> 16) & 0x7fff;
		if (!a6l_smr_is_cam(id, mask))
			continue;
		s2cr = readl_relaxed(a6l_smmu + 0xc00 + 4 * i);
		if (log)
			dev_info(vfe->camss->dev, "A6L_SMMU SMR[%u] %08x (id %03x mask %03x) S2CR %08x type %u cb %u\n",
				 i, smr, id, mask, s2cr, (s2cr >> 16) & 3, s2cr & 0xff);
		a6l_cb_add(cbs, &k, max, s2cr);
	}
	return k;
}

static void a6l_smmu_regs(struct vfe_device *vfe, const char *tag)
{
	u32 cbs[4], i;
	unsigned int ncb;

	if (!a6l_smmu_ok()) {
		dev_info(vfe->camss->dev, "A6L_SMMU %s skipped: gdsc %08x ahb %08x\n", tag,
			 a6l_mmcc ? readl_relaxed(a6l_mmcc + 0xe020) : 0, a6l_mmcc ? readl_relaxed(a6l_mmcc + 0xe004) : 0);
		return;
	}
	dev_info(vfe->camss->dev, "A6L_SMMU %s scr0 %08x idr0 %08x idr1 %08x gfsr %08x gfsynr0 %08x gfsynr1 %08x\n", tag,
		 readl_relaxed(a6l_smmu), readl_relaxed(a6l_smmu + 0x20), readl_relaxed(a6l_smmu + 0x24),
		 readl_relaxed(a6l_smmu + 0x48), readl_relaxed(a6l_smmu + 0x50), readl_relaxed(a6l_smmu + 0x54));
	ncb = a6l_smmu_cam_cbs(cbs, ARRAY_SIZE(cbs), vfe, !strcmp(tag, "START"));
	if (!ncb)
		dev_info(vfe->camss->dev, "A6L_SMMU %s no translating context bank found for SIDs 0xc00..0xc03\n", tag);
	for (i = 0; i < ncb; i++) {
		void __iomem *cb = a6l_smmu_cb(cbs[i]);
		u32 pg = a6l_smmu_pgsz();

		if (!cb)
			continue;
		dev_info(vfe->camss->dev,
			 "A6L_SMMU %s cb%u sctlr %08x actlr %08x tcr %08x fsr %08x far %08x%08x fsynr0 %08x fsynr1 %08x cbar %08x cbfrsynra %08x\n",
			 tag, cbs[i], readl_relaxed(cb), readl_relaxed(cb + 0x4), readl_relaxed(cb + 0x30),
			 readl_relaxed(cb + 0x58), readl_relaxed(cb + 0x64), readl_relaxed(cb + 0x60),
			 readl_relaxed(cb + 0x68), readl_relaxed(cb + 0x6c), readl_relaxed(a6l_smmu + pg + 4 * cbs[i]),
			 readl_relaxed(a6l_smmu + pg + 0x400 + 4 * cbs[i]));
	}
	if (!strcmp(tag, "START") || !strcmp(tag, "STOP")) {
		char buf[400];
		int len = 0;

		for (i = 0; i < ARRAY_SIZE(a6l_smmu_impl); i++)
			len += scnprintf(buf + len, sizeof(buf) - len, " %x:%x", a6l_smmu_impl[i][0],
					 readl_relaxed(a6l_smmu + a6l_smmu_impl[i][0]));
		dev_info(vfe->camss->dev, "A6L_SMMU %s impl%s\n", tag, buf);
	}
}

/* bit1: msm-4.4 qsmmuv2_halt -> impl-defs -> qsmmuv2_resume (IMPL_DEF1 page = base + 6 * pagesize) */
static void a6l_smmu_impl_apply(struct vfe_device *vfe)
{
	void __iomem *ctl;
	u32 c, st = 0, i, changed = 0;
	int ret;

	if (!a6l_smmu_ok() || a6l_smmu_pgsz() != 0x1000) {
		dev_info(vfe->camss->dev, "A6L_SMMU IMPL skipped (smmu not powered or not 4K pages)\n");
		return;
	}
	ctl = a6l_smmu + 0x6000;
	c = readl_relaxed(ctl);
	writel_relaxed(c | BIT(2), ctl);
	ret = readl_poll_timeout_atomic(ctl, st, st & BIT(3), 1, 30000);
	if (ret) {
		writel_relaxed(readl_relaxed(ctl) & ~BIT(2), ctl);
		dev_info(vfe->camss->dev, "A6L_SMMU IMPL halt timeout (ctl %08x -> %08x): not applied\n", c, st);
		return;
	}
	for (i = 0; i < ARRAY_SIZE(a6l_smmu_impl); i++) {
		u32 old = readl_relaxed(a6l_smmu + a6l_smmu_impl[i][0]);

		writel_relaxed(a6l_smmu_impl[i][1], a6l_smmu + a6l_smmu_impl[i][0]);
		if (old != a6l_smmu_impl[i][1])
			changed++;
	}
	wmb();
	writel_relaxed(readl_relaxed(ctl) & ~BIT(2), ctl);
	wmb();
	dev_info(vfe->camss->dev, "A6L_SMMU IMPL applied %zu regs (%u changed), ctl %08x -> %08x\n",
		 ARRAY_SIZE(a6l_smmu_impl), changed, c, readl_relaxed(ctl));
}

/* bit3: msm-4.4 arm_smmu_init_context_bank (QCOM_SMMUV2): ACTLR = ISH | OSH | NSH */
static void a6l_smmu_actlr(struct vfe_device *vfe)
{
	u32 cbs[4], i;
	unsigned int ncb;

	if (!a6l_smmu_ok())
		return;
	ncb = a6l_smmu_cam_cbs(cbs, ARRAY_SIZE(cbs), vfe, false);
	for (i = 0; i < ncb; i++) {
		void __iomem *cb = a6l_smmu_cb(cbs[i]);
		u32 old;

		if (!cb)
			continue;
		old = readl_relaxed(cb + 0x4);
		writel_relaxed(old | 0x70000000, cb + 0x4);
		dev_info(vfe->camss->dev, "A6L_SMMU ACTLR cb%u %08x -> %08x\n", cbs[i], old, readl_relaxed(cb + 0x4));
	}
}

static void a6l_mmcc_regs(struct vfe_device *vfe, const char *tag)
{
	static const struct { const char *n; u16 o; } br[] = {
		{ "throttle_axi", 0x3c3c }, { "vbif_axi", 0x36bc }, { "vbif_ahb", 0x36b8 }, { "vfe0", 0x36a8 },
		{ "vfe0_ahb", 0x3668 }, { "vfe0_stream", 0x3720 }, { "csi_vfe0", 0x3704 }, { "camss_ahb", 0x348c },
		{ "top_ahb", 0x3484 }, { "micro_ahb", 0x3494 }, { "mnoc_ahb", 0x5024 }, { "smmu_ahb", 0xe004 },
		{ "smmu_axi", 0xe008 }, { "csi0rdi", 0x30d4 },
	};
	static const struct { const char *n; u16 o; } pll[] = {
		{ "mmpll0", 0xc000 }, { "mmpll4", 0x50 }, { "mmpll7", 0x140 }, { "mmpll10", 0x190 },
	};
	char buf[400];
	int len = 0;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(br); i++) {
		u32 v = readl_relaxed(a6l_mmcc + br[i].o);

		len += scnprintf(buf + len, sizeof(buf) - len, " %s:%s%s", br[i].n, (v & BIT(31)) ? "OFF" : "on",
				 (v & BIT(0)) ? "" : "(noen)");
	}
	dev_info(vfe->camss->dev, "A6L_MMCC %s cbcr%s\n", tag, buf);
	len = 0;
	for (i = 0; i < ARRAY_SIZE(pll); i++) {
		u32 m = readl_relaxed(a6l_mmcc + pll[i].o), l = readl_relaxed(a6l_mmcc + pll[i].o + 4);

		len += scnprintf(buf + len, sizeof(buf) - len, " %s:mode %08x L %u (%u MHz)%s", pll[i].n, m, l & 0xffff,
				 (l & 0xffff) * 192 / 10, (m & BIT(31)) ? " LOCK" : " nolock");
	}
	dev_info(vfe->camss->dev, "A6L_MMCC %s pll%s\n", tag, buf);
	dev_info(vfe->camss->dev,
		 "A6L_MMCC %s gdsc camss_top %08x vfe0 %08x vfe1 %08x bimc_smmu %08x rcg axi %08x/%08x ahb %08x/%08x vfe0 %08x/%08x\n",
		 tag, readl_relaxed(a6l_mmcc + 0x34a0), readl_relaxed(a6l_mmcc + 0x3664), readl_relaxed(a6l_mmcc + 0x3674),
		 readl_relaxed(a6l_mmcc + 0xe020), readl_relaxed(a6l_mmcc + 0xd000), readl_relaxed(a6l_mmcc + 0xd004),
		 readl_relaxed(a6l_mmcc + 0x5000), readl_relaxed(a6l_mmcc + 0x5004), readl_relaxed(a6l_mmcc + 0x3600),
		 readl_relaxed(a6l_mmcc + 0x3604));
}

/* may run in IRQ context (first bus error): never maps, only uses what a6l_v7_start mapped */
static void a6l_v7_regs(struct vfe_device *vfe, const char *tag)
{
	if (!(a6l_v7 & 1) || !a6l_mmcc)
		return;
	a6l_mmcc_regs(vfe, tag);
	a6l_smmu_regs(vfe, tag);
}

/* stream-on, process context (vfe_set_qos under stream_lock) */
static void a6l_v7_start(struct vfe_device *vfe)
{
	a6l_v7_map();
	a6l_v7_be_logged[vfe->id & 1] = false;
	if (a6l_v7 & 2)
		a6l_smmu_impl_apply(vfe);
	if (a6l_v7 & 8)
		a6l_smmu_actlr(vfe);
	a6l_v7_regs(vfe, "START");
}

/* bit2: stock vfe0 clock list members that upstream camss never holds */
static struct clk *a6l_v7_clks[4];
static const int a6l_v7_clk_ids[4] = { MNOC_AHB_CLK, BIMC_SMMU_AHB_CLK, BIMC_SMMU_AXI_CLK, CAMSS_MICRO_AHB_CLK };
static const char * const a6l_v7_clk_names[4] = { "mnoc_ahb", "bimc_smmu_ahb", "bimc_smmu_axi", "camss_micro_ahb" };
static bool a6l_v7_clk_held;

void a6l_v7_clk_on(struct vfe_device *vfe)
{
	struct of_phandle_args args = { .args_count = 1 };
	unsigned int i;

	if (!(a6l_v7 & 4) || a6l_v7_clk_held)
		return;
	args.np = of_find_compatible_node(NULL, NULL, "qcom,mmcc-sdm660");
	if (!args.np) {
		dev_info(vfe->camss->dev, "A6L_V7CLK no mmcc node\n");
		return;
	}
	for (i = 0; i < ARRAY_SIZE(a6l_v7_clks); i++) {
		struct clk *c;
		bool was;
		int r;

		args.args[0] = a6l_v7_clk_ids[i];
		c = of_clk_get_from_provider(&args);
		if (IS_ERR(c)) {
			dev_info(vfe->camss->dev, "A6L_V7CLK %s get %ld\n", a6l_v7_clk_names[i], PTR_ERR(c));
			continue;
		}
		was = __clk_is_enabled(c);
		r = clk_prepare_enable(c);
		dev_info(vfe->camss->dev, "A6L_V7CLK %s was %s -> enable %d rate %lu\n", a6l_v7_clk_names[i],
			 was ? "on" : "off", r, clk_get_rate(c));
		if (r)
			clk_put(c);
		else
			a6l_v7_clks[i] = c;
	}
	of_node_put(args.np);
	a6l_v7_clk_held = true;
}

void a6l_v7_clk_off(struct vfe_device *vfe)
{
	unsigned int i;

	if (!a6l_v7_clk_held)
		return;
	for (i = 0; i < ARRAY_SIZE(a6l_v7_clks); i++) {
		if (!a6l_v7_clks[i])
			continue;
		clk_disable_unprepare(a6l_v7_clks[i]);
		clk_put(a6l_v7_clks[i]);
		a6l_v7_clks[i] = NULL;
	}
	a6l_v7_clk_held = false;
	dev_info(vfe->camss->dev, "A6L_V7CLK released\n");
}

/* camfix4: stock msm_vfe47_update_ping_pong_addr writes WR_PING/PONG_MAX_ADDR = (addr + size) & ~0x1f */
''')

# ---- 4-8: hooks -----------------------------------------------------------------------------------------------------
sub1(V48, "\tif (s1 & BIT(4)) {\n\t\tif (!a6l_f_be[v])\n\t\t\ta6l_f_be_first[v] = a6l_f_sof[v];\n",
     "\tif (s1 & BIT(4)) {\n\t\tif (!a6l_f_be[v])\n\t\t\ta6l_f_be_first[v] = a6l_f_sof[v];\n"
     "\t\tif (!a6l_v7_be_logged[v]) { /* camfix7: SMMU/MMCC state at the first bus error */\n"
     "\t\t\ta6l_v7_be_logged[v] = true;\n\t\t\ta6l_v7_regs(vfe, \"BUSERR\");\n\t\t}\n")
sub1(V48, "\ta6l_v6_regs(vfe, tag);\n\ta6l_f_rec[v] = 0;\n",
     "\ta6l_v6_regs(vfe, tag);\n\ta6l_v7_regs(vfe, tag); /* camfix7 */\n\ta6l_f_rec[v] = 0;\n")
sub1(V48, "\ta6l_v6_regs(vfe, \"START\");\n}\n",
     "\ta6l_v6_regs(vfe, \"START\");\n\ta6l_v7_start(vfe); /* camfix7 */\n}\n")

# ---- gen1: clocks around the RDI stream (process context) -------------------------------------------------------------
sub1(G1, "\tif (a6l_wm & 4) { /* camfix4: simple fixed ping/pong */\n",
     "\ta6l_v7_clk_on(vfe); /* camfix7 bit2 */\n\tif (a6l_wm & 4) { /* camfix4: simple fixed ping/pong */\n")
sub1(G1, "\ta6l_vfe_dump(vfe, \"STOP\");\n\tvfe_disable_output(line);\n",
     "\ta6l_vfe_dump(vfe, \"STOP\");\n\tvfe_disable_output(line);\n\ta6l_v7_clk_off(vfe); /* camfix7 bit2 */\n")
print("CAMFIX7_PATCH_OK")
