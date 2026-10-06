#!/usr/bin/env python3
# camfix10 (30 Sep 2026): applied AFTER camfix_patch.py .. camfix9b_patch.py.   usage: camfix10_patch.py <camss_dir>
# Splits the invariant "one RDI bus error, WM writes 9-19 lines, frame-done (almost) never" in one sweep.
# See docs/camfix10-20260930.md. New runtime param qcom_camss.a6l_v10 (default 0x01) + a6l_v10_tpg (default 1):
#   bit0 (1)   diagnostics, all read-only / IRQ-safe:
#              - A6L_V10 IOVA: per fixed slot, every 4 KB page: iommu_iova_to_phys(camss domain, iova+off) against the
#                CPU page behind the kernel mapping (vmalloc_to_page), unmapped / mismatch counts, physical segment
#                count, domain type (lens A: scattered or mis-mapped buffer)
#              - A6L_V10 F: per RDI SOF (1..8, then every 64th) and at the first bus error / done: VFE s0/s1,
#                violation 0x7c, 0xC94, ping-pong 0x338, WM0 0xA0..0xCB decoded, CSID IRQ status (polled + cleared
#                each frame, the CSID IRQ mask only has RST_DONE), CSIPHY common status 0..10 (polled + cleared,
#                sdm660 enables no CSIPHY IRQ), SMMU cb0 FSR/FSYNR0/TLBSTATUS
#              - A6L_V10 SUM at stop: per-bit frame counts of CSID status bits, OR of every CSIPHY status reg, OR of
#                SMMU FSR fault bits, extra-IRQ counts
#              - A6L_V10 HIST at stop: written-region histogram (0xffffffff / 0 / fill / other), crc32, 8 bar-centre
#                samples of lines 0..3 (lens B test 4: real colour bars or filler?)
#              - A6L_V10 SG: sg_table nents / orig_nents / first dma addr+len of the first 4 vb2 buffers
#   bit1 (2)   CSID test generator instead of the sensor (payload a6l_v10_tpg: 1 incrementing, 2 0x55/0xAA,
#              3 zeros, 4 ones, 5 random) with the sink format (2880x2156 RAW10) -> splits sensor/CSIPHY/CSID-RX
#              from ISPIF/VFE/WM/SMMU/DDR. The sensor keeps streaming (ignored by the CSID).
#   bit2 (4)   fixed ping/pong allocated with DMA_ATTR_FORCE_CONTIGUOUS (frame + 64 KB each, CMA 32 MB), fallback to
#              the normal allocation if CMA fails (logged) (lens A test 2)
#   bit3 (8)   stock IRQ_MASK_1 (0xFFFFFF7E | violation) during the stream: every error bit (UB overflow per WM,
#              violation, stats/realign) becomes visible (lens B test 2). A bit firing > 64 times is masked again.
#   bit4 (16)  fill marker 0xdeadbeef instead of 0xa5a5a5a5 + snapshot of the first 32 words of both slots at the
#              first bus error (lens A test 4)
#   bit5 (32)  (with bit1) test generator 1280x720 (1600 bytes/line), maximum blanking: small frame (lens B test 1)
#   bit6 (64)  (with bit1) test generator maximum blanking (V 255 lines, H 2047) at full size: lower line rate
# Notes: iommu_set_fault_handler() (lens A test 3) is refused by 7.2 for DMA domains (WARN_ON cookie_type !=
# IOMMU_COOKIE_NONE); arm-smmu's own context-fault print is always on, and bit0 polls cb0 FSR at every VFE IRQ instead.
# The "first word 0x4" of slot 0 (lens A detail) is the camfix6 bit7 probe: it rewrites word 0 of every written
# line with the fill value at each SOF (camss-vfe-gen1.c a6l_fix_probe), so it is not a hardware symptom.
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
CSID = f"{cs}/camss-csid-4-7.c"
PHY = f"{cs}/camss-csiphy-3ph-1-0.c"
VID = f"{cs}/camss-video.c"

# ---------------- header ----------------
sub1(H, "	dma_addr_t a6l_fix[2];\n", "	dma_addr_t a6l_fix[2];\n	unsigned long a6l_fix_attrs; /* camfix10 bit2 */\n")
sub1(H, "void a6l_stk_pp(struct vfe_device *vfe, u8 wm, int slot, u32 addr);\n",
"""void a6l_stk_pp(struct vfe_device *vfe, u8 wm, int slot, u32 addr);
/* A6L camfix10 (camss-vfe-4-8.c) */
extern uint a6l_v10, a6l_v10_tpg;
void a6l_v10_csid_on(void __iomem *base, u32 id);
void a6l_v10_csid_off(void);
void a6l_v10_phy_on(void __iomem *base, u32 offset, u32 status_offset, u32 id);
void a6l_v10_phy_off(void);
void a6l_v10_start(struct vfe_device *vfe, u8 wm, int id);
void a6l_v10_stop(struct vfe_device *vfe, int id);
""")

# ---------------- camss-vfe-4-8.c: params, state, hooks ----------------
sub1(V48, "int a6l_wmmax = 1;\n",
r'''/* ================= camfix10: split sensor/CSI from VFE/WM/SMMU, one-run diagnostics ================= */
uint a6l_v10 = 1;
module_param(a6l_v10, uint, 0644);
MODULE_PARM_DESC(a6l_v10, "A6L camfix10: bit0 diagnostics (IOVA->phys per page, per-frame CSID/CSIPHY/VFE/SMMU poll, histogram), bit1 CSID test generator instead of the sensor, bit2 fixed buffers DMA_ATTR_FORCE_CONTIGUOUS, bit3 stock IRQ_MASK_1 (all error bits), bit4 fill 0xdeadbeef + slot snapshot at the first bus error, bit5 TG 1280x720 max blanking, bit6 TG max blanking (default 1)");
uint a6l_v10_tpg = 1;
module_param(a6l_v10_tpg, uint, 0644);
MODULE_PARM_DESC(a6l_v10_tpg, "A6L camfix10: CSID test generator payload with a6l_v10 bit1: 1 incrementing, 2 0x55/0xAA, 3 zeros, 4 ones, 5 random (default 1)");
static void __iomem *a6l_v10_csid, *a6l_v10_phy;
static u32 a6l_v10_csid_id, a6l_v10_phy_id, a6l_v10_phy_st;
static void __iomem *a6l_v10_cb;
static bool a6l_v10_run[2];
static u32 a6l_v10_nsof[2], a6l_v10_nlog[2], a6l_v10_csid_bits[32], a6l_v10_csid_frames, a6l_v10_phy_or[11];
static u32 a6l_v10_phy_frames, a6l_v10_fsr_or, a6l_v10_fsr_n, a6l_v10_xbits[32], a6l_v10_mask1_old[2];
static bool a6l_v10_mask1_set[2], a6l_v10_be_snap[2], a6l_v10_done_logged[2];

void a6l_v10_csid_on(void __iomem *base, u32 id)
{
	a6l_v10_csid_id = id;
	WRITE_ONCE(a6l_v10_csid, base);
}

void a6l_v10_csid_off(void)
{
	WRITE_ONCE(a6l_v10_csid, NULL);
}

void a6l_v10_phy_on(void __iomem *base, u32 offset, u32 status_offset, u32 id)
{
	a6l_v10_phy_st = status_offset;
	a6l_v10_phy_id = id;
	WRITE_ONCE(a6l_v10_phy, base + offset);
}

void a6l_v10_phy_off(void)
{
	WRITE_ONCE(a6l_v10_phy, NULL);
}

int a6l_wmmax = 1;
''')

# ISR helper + start/stop, placed before vfe_isr (after the camfix9b helpers, all macros known)
sub1(V48, "static irqreturn_t vfe_isr(int irq, void *dev)\n{\n",
r'''/* camfix10: CSID status (poll + clear, like csid_isr), CSIPHY common status 0..10 (poll + clear, like csiphy_isr) */
static u32 a6l_v10_poll_csid(void)
{
	void __iomem *c = READ_ONCE(a6l_v10_csid);
	u32 st;

	if (!c)
		return 0xdead0000;
	st = readl_relaxed(c + 0x06c);
	if (st)
		writel_relaxed(st, c + 0x064);
	return st;
}

static bool a6l_v10_poll_phy(u8 *s)
{
	void __iomem *p = READ_ONCE(a6l_v10_phy);
	bool any = false;
	int i;

	if (!p)
		return false;
	for (i = 0; i < 11; i++) {
		s[i] = readl_relaxed(p + a6l_v10_phy_st + 4 * i) & 0xff;
		any |= s[i] != 0;
	}
	if (!any)
		return false;
	for (i = 0; i < 11; i++)
		writel_relaxed(s[i], p + 4 * (22 + i));
	writel_relaxed(1, p + 4 * 10);
	writel_relaxed(0, p + 4 * 10);
	for (i = 22; i < 33; i++)
		writel_relaxed(0, p + 4 * i);
	return true;
}

/* hard IRQ, every VFE interrupt while an RDI line streams */
static void a6l_v10_isr(struct vfe_device *vfe, u32 s0, u32 s1)
{
	unsigned int v = vfe->id & 1, b;
	bool sof = s1 & BIT(29), be = s1 & BIT(4), done = s0 & GENMASK(14, 8), dolog;
	u32 fsr = 0, fsynr = 0, tlb = 0, xs;
	u8 phy[11] = { 0 };

	if (!a6l_v10 || !a6l_v10_run[v])
		return;
	if (a6l_v10_cb) {
		fsr = readl_relaxed(a6l_v10_cb + 0x58);
		if (fsr & ~(3u << 9)) {
			fsynr = readl_relaxed(a6l_v10_cb + 0x68);
			if (!a6l_v10_fsr_n++)
				dev_info(vfe->camss->dev, "A6L_V10 SMMU_FAULT cb0 fsr %08x fsynr0 %08x far %08x%08x sof#%u\n", fsr,
					 fsynr, readl_relaxed(a6l_v10_cb + 0x64), readl_relaxed(a6l_v10_cb + 0x60), a6l_v10_nsof[v]);
			a6l_v10_fsr_or |= fsr & ~(3u << 9);
		}
	}
	/* bit3: bits we enabled on top of the mainline mask */
	xs = s1 & ~a6l_v10_mask1_old[v] & ~(BIT(4) | GENMASK(31, 29));
	if (a6l_v10_mask1_set[v] && xs) {
		for (b = 0; b < 32; b++)
			if (xs & BIT(b)) {
				if (!a6l_v10_xbits[b])
					dev_info(vfe->camss->dev, "A6L_V10 XIRQ s1 bit%u first at sof#%u s1 %08x viol(7c) %08x c94 %08x\n",
						 b, a6l_v10_nsof[v], s1, readl_relaxed(vfe->base + 0x07c),
						 readl_relaxed(vfe->base + 0xc94));
				if (++a6l_v10_xbits[b] == 64) {
					vfe_reg_clr(vfe, VFE_0_IRQ_MASK_1, BIT(b));
					dev_info(vfe->camss->dev, "A6L_V10 XIRQ s1 bit%u masked again after 64\n", b);
				}
			}
	}
	if (sof) {
		u32 cst;
		bool phy_ev;
		int i;

		a6l_v10_nsof[v]++;
		cst = a6l_v10_poll_csid();
		if (cst && cst != 0xdead0000) {
			a6l_v10_csid_frames++;
			for (b = 0; b < 32; b++)
				if (cst & BIT(b))
					a6l_v10_csid_bits[b]++;
		}
		phy_ev = a6l_v10_poll_phy(phy);
		if (phy_ev) {
			a6l_v10_phy_frames++;
			for (i = 0; i < 11; i++)
				a6l_v10_phy_or[i] |= phy[i];
		}
		if (a6l_v10_cb)
			tlb = readl_relaxed(a6l_v10_cb + 0x7f4);
		dolog = a6l_v10_nsof[v] <= 8 || !(a6l_v10_nsof[v] % 64);
		if (dolog && a6l_v10_nlog[v] < 24) {
			u32 w = 0xa0; /* WM0 (RDI0) */

			a6l_v10_nlog[v]++;
			dev_info(vfe->camss->dev,
				 "A6L_V10 F sof#%u s0 %08x s1 %08x viol %08x c94 %08x pp %08x | wm0 addrcfg %08x ping %08x pong %08x max %08x/%08x size %08x bufcfg %08x pat %08x sub %08x ub %08x | csid%u %08x | phy%u %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x | fsr %08x tlb %08x\n",
				 a6l_v10_nsof[v], s0, s1, readl_relaxed(vfe->base + 0x07c), readl_relaxed(vfe->base + 0xc94),
				 readl_relaxed(vfe->base + 0x338), readl_relaxed(vfe->base + w + 0x14),
				 readl_relaxed(vfe->base + w + 0x4), readl_relaxed(vfe->base + w + 0xc),
				 readl_relaxed(vfe->base + w + 0x8), readl_relaxed(vfe->base + w + 0x10),
				 readl_relaxed(vfe->base + w + 0x1c), readl_relaxed(vfe->base + w + 0x20),
				 readl_relaxed(vfe->base + w + 0x24), readl_relaxed(vfe->base + w + 0x28),
				 readl_relaxed(vfe->base + w + 0x18), a6l_v10_csid_id, cst, a6l_v10_phy_id,
				 phy[0], phy[1], phy[2], phy[3], phy[4], phy[5], phy[6], phy[7], phy[8], phy[9], phy[10], fsr, tlb);
		}
	}
	if (done && !a6l_v10_done_logged[v]) {
		a6l_v10_done_logged[v] = true;
		dev_info(vfe->camss->dev, "A6L_V10 DONE first at sof#%u s0 %08x pp %08x c94 %08x\n", a6l_v10_nsof[v], s0,
			 readl_relaxed(vfe->base + 0x338), readl_relaxed(vfe->base + 0xc94));
	}
	if (be && !a6l_v10_be_snap[v]) {
		struct vfe_output *o = &vfe->line[VFE_LINE_RDI0].output;
		unsigned int s;

		a6l_v10_be_snap[v] = true;
		dev_info(vfe->camss->dev, "A6L_V10 BUSERR sof#%u s0 %08x s1 %08x c94 %08x pp %08x wm0 ping %08x pong %08x ub %08x fsr %08x\n",
			 a6l_v10_nsof[v], s0, s1, readl_relaxed(vfe->base + 0xc94), readl_relaxed(vfe->base + 0x338),
			 readl_relaxed(vfe->base + 0xa4), readl_relaxed(vfe->base + 0xac), readl_relaxed(vfe->base + 0xb8), fsr);
		for (s = 0; (a6l_v10 & 16) && s < 2 && o->a6l_mode == 2; s++) {
			const u32 *p = o->a6l_fix_cpu[s];

			if (!p)
				continue;
			dev_info(vfe->camss->dev, "A6L_V10 SNAP slot %u +00 %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x\n",
				 s, p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7], p[8], p[9], p[10], p[11], p[12], p[13], p[14], p[15]);
			dev_info(vfe->camss->dev, "A6L_V10 SNAP slot %u +40 %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x\n",
				 s, p[16], p[17], p[18], p[19], p[20], p[21], p[22], p[23], p[24], p[25], p[26], p[27], p[28], p[29], p[30], p[31]);
		}
	}
}

/* camfix10: from vfe_enable_output (output_lock held, process context below the lock is not needed) */
void a6l_v10_start(struct vfe_device *vfe, u8 wm, int id)
{
	unsigned int v = vfe->id & 1;

	if (id != VFE_LINE_RDI0)
		return;
	a6l_v10_nsof[v] = a6l_v10_nlog[v] = 0;
	a6l_v10_csid_frames = a6l_v10_phy_frames = a6l_v10_fsr_or = a6l_v10_fsr_n = 0;
	memset(a6l_v10_csid_bits, 0, sizeof(a6l_v10_csid_bits));
	memset(a6l_v10_phy_or, 0, sizeof(a6l_v10_phy_or));
	memset(a6l_v10_xbits, 0, sizeof(a6l_v10_xbits));
	a6l_v10_be_snap[v] = a6l_v10_done_logged[v] = false;
	if (!a6l_v10)
		return;
	a6l_v10_cb = NULL;
	if (a6l_smmu_ok()) {
		u32 cbs[4];

		if (a6l_smmu_cam_cbs(cbs, ARRAY_SIZE(cbs), vfe, false))
			a6l_v10_cb = a6l_smmu_cb(cbs[0]);
	}
	a6l_v10_mask1_old[v] = readl_relaxed(vfe->base + VFE_0_IRQ_MASK_1);
	if (a6l_v10 & 8) {
		vfe_reg_set(vfe, VFE_0_IRQ_MASK_1, 0xFFFFFF7E | BIT(7));
		a6l_v10_mask1_set[v] = true;
	}
	/* clear stale CSID / CSIPHY status so that frame 1 shows only its own events */
	a6l_v10_poll_csid();
	{
		u8 phy[11];

		a6l_v10_poll_phy(phy);
	}
	a6l_v10_run[v] = true;
	wmb();
	dev_info(vfe->camss->dev, "A6L_V10 START vfe%u wm %u v10 0x%02x tpg %u mask1 %08x -> %08x csid%u %s phy%u %s smmu_cb %s\n",
		 vfe->id, wm, a6l_v10, a6l_v10_tpg, a6l_v10_mask1_old[v], readl_relaxed(vfe->base + VFE_0_IRQ_MASK_1),
		 a6l_v10_csid_id, a6l_v10_csid ? "polled" : "none", a6l_v10_phy_id, a6l_v10_phy ? "polled" : "none",
		 a6l_v10_cb ? "polled" : "none");
}

/* camfix10: from vfe_disable_output (output_lock held) */
void a6l_v10_stop(struct vfe_device *vfe, int id)
{
	unsigned int v = vfe->id & 1, b;
	char buf[320];
	int len = 0;

	if (id != VFE_LINE_RDI0 || !a6l_v10_run[v])
		return;
	a6l_v10_run[v] = false;
	if (a6l_v10_mask1_set[v]) {
		writel_relaxed(a6l_v10_mask1_old[v], vfe->base + VFE_0_IRQ_MASK_1);
		a6l_v10_mask1_set[v] = false;
	}
	buf[0] = 0;
	for (b = 0; b < 32; b++)
		if (a6l_v10_csid_bits[b])
			len += scnprintf(buf + len, sizeof(buf) - len, " b%u:%u", b, a6l_v10_csid_bits[b]);
	dev_info(vfe->camss->dev, "A6L_V10 SUM sof %u csid_frames %u csid_bits%s\n", a6l_v10_nsof[v], a6l_v10_csid_frames,
		 len ? buf : " none");
	len = 0;
	buf[0] = 0;
	for (b = 0; b < 32; b++)
		if (a6l_v10_xbits[b])
			len += scnprintf(buf + len, sizeof(buf) - len, " s1b%u:%u", b, a6l_v10_xbits[b]);
	dev_info(vfe->camss->dev, "A6L_V10 SUM phy_frames %u phy_or %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x fsr_or %08x fsr_irqs %u xirq%s\n",
		 a6l_v10_phy_frames, a6l_v10_phy_or[0], a6l_v10_phy_or[1], a6l_v10_phy_or[2], a6l_v10_phy_or[3],
		 a6l_v10_phy_or[4], a6l_v10_phy_or[5], a6l_v10_phy_or[6], a6l_v10_phy_or[7], a6l_v10_phy_or[8],
		 a6l_v10_phy_or[9], a6l_v10_phy_or[10], a6l_v10_fsr_or, a6l_v10_fsr_n, len ? buf : " none");
}

static irqreturn_t vfe_isr(int irq, void *dev)
{
''')

sub1(V48, "	a6l_stk_isr(vfe, value0, value1); /* camfix9b */\n",
     "	a6l_stk_isr(vfe, value0, value1); /* camfix9b */\n	a6l_v10_isr(vfe, value0, value1); /* camfix10 */\n")

# ---------------- gen1: fill, contiguous alloc, IOVA check, histogram, hooks ----------------
sub1(G1, "#define A6L_FILL 0xa5a5a5a5U\n",
"""static u32 a6l_fill = 0xa5a5a5a5U; /* camfix10 bit4: 0xdeadbeef */
#define A6L_FILL a6l_fill
""")
sub1(G1, "#include <media/videobuf2-dma-sg.h>\n",
"""#include <media/videobuf2-dma-sg.h>
#include <linux/iommu.h>
#include <linux/vmalloc.h>
#include <linux/mm.h>
#include <linux/crc32.h>
#include <linux/sizes.h>
""")

OLD_PREP = """	size_t sz = PAGE_ALIGN(2 * frame);
	unsigned int s;

	if (output->a6l_fix_cpu[0] && output->a6l_fix_sz < sz) {
		for (s = 0; s < 2; s++)
			if (output->a6l_fix_cpu[s])
				dma_free_attrs(vfe->camss->dev, output->a6l_fix_sz, output->a6l_fix_cpu[s],
					       output->a6l_fix[s], 0);
		output->a6l_fix_cpu[0] = output->a6l_fix_cpu[1] = NULL;
	}
	if (!output->a6l_fix_cpu[0]) {
		for (s = 0; s < 2; s++) {
			output->a6l_fix_cpu[s] = dma_alloc_attrs(vfe->camss->dev, sz, &output->a6l_fix[s],
								 GFP_KERNEL, 0);
			if (!output->a6l_fix_cpu[s]) {
				dev_err(vfe->camss->dev, "A6L_WM4 fixed buffer %u (%zu bytes) alloc failed\\n", s, sz);
				if (s) {
					dma_free_attrs(vfe->camss->dev, sz, output->a6l_fix_cpu[0], output->a6l_fix[0], 0);
					output->a6l_fix_cpu[0] = NULL;
				}
				return -ENOMEM;
			}
		}
		output->a6l_fix_sz = sz;
	}
"""
NEW_PREP = """	/* camfix10 bit2: physically contiguous slots (frame + 64 KB each, CMA is 32 MB) */
	unsigned long attrs = (a6l_v10 & 4) ? DMA_ATTR_FORCE_CONTIGUOUS : 0;
	size_t sz = attrs ? PAGE_ALIGN(frame + SZ_64K) : PAGE_ALIGN(2 * frame);
	unsigned int s;

retry:
	if (output->a6l_fix_cpu[0] && (output->a6l_fix_sz < sz || output->a6l_fix_attrs != attrs)) {
		for (s = 0; s < 2; s++)
			if (output->a6l_fix_cpu[s])
				dma_free_attrs(vfe->camss->dev, output->a6l_fix_sz, output->a6l_fix_cpu[s],
					       output->a6l_fix[s], output->a6l_fix_attrs);
		output->a6l_fix_cpu[0] = output->a6l_fix_cpu[1] = NULL;
	}
	if (!output->a6l_fix_cpu[0]) {
		for (s = 0; s < 2; s++) {
			output->a6l_fix_cpu[s] = dma_alloc_attrs(vfe->camss->dev, sz, &output->a6l_fix[s],
								 GFP_KERNEL, attrs);
			if (!output->a6l_fix_cpu[s]) {
				dev_err(vfe->camss->dev, "A6L_WM4 fixed buffer %u (%zu bytes, attrs 0x%lx) alloc failed\\n",
					s, sz, attrs);
				if (s) {
					dma_free_attrs(vfe->camss->dev, sz, output->a6l_fix_cpu[0], output->a6l_fix[0], attrs);
					output->a6l_fix_cpu[0] = NULL;
				}
				if (attrs) {
					dev_info(vfe->camss->dev, "A6L_V10 CONTIG alloc failed: fallback to the normal allocation\\n");
					attrs = 0;
					sz = PAGE_ALIGN(2 * frame);
					goto retry;
				}
				return -ENOMEM;
			}
		}
		output->a6l_fix_sz = sz;
		output->a6l_fix_attrs = attrs;
	}
	a6l_fill = (a6l_v10 & 16) ? 0xdeadbeefU : 0xa5a5a5a5U;
"""
sub1(G1, OLD_PREP, NEW_PREP)

sub1(G1, "static int a6l_prepare_fixed(struct vfe_device *vfe, struct vfe_output *output, size_t frame)\n{\n",
r'''/* camfix10 bit0 (process context): does every IOVA page of a fixed slot translate to the page the CPU sees? */
static void a6l_v10_iova(struct vfe_device *vfe, struct vfe_output *o, const char *tag)
{
	struct iommu_domain *dom = iommu_get_domain_for_dev(vfe->camss->dev);
	unsigned int s;

	if (!(a6l_v10 & 1))
		return;
	for (s = 0; s < 2; s++) {
		size_t off, pages = 0, unmapped = 0, mism = 0, segs = 0, first_bad = SIZE_MAX;
		phys_addr_t prev = 0, p0 = 0;
		char *va = o->a6l_fix_cpu[s];

		if (!va)
			continue;
		for (off = 0; off < o->a6l_fix_sz; off += PAGE_SIZE) {
			phys_addr_t ip = dom ? iommu_iova_to_phys(dom, o->a6l_fix[s] + off) : 0;
			struct page *pg = is_vmalloc_addr(va + off) ? vmalloc_to_page(va + off) : virt_to_page(va + off);
			phys_addr_t cp = pg ? page_to_phys(pg) : 0;

			pages++;
			if (!off)
				p0 = ip;
			if (!ip) {
				unmapped++;
			} else if (ip != cp) {
				mism++;
				if (first_bad == SIZE_MAX)
					first_bad = off;
			}
			if (!off || ip != prev + PAGE_SIZE)
				segs++;
			prev = ip;
		}
		dev_info(vfe->camss->dev,
			 "A6L_V10 IOVA %s slot %u iova %pad dom_type 0x%x pages %zu unmapped %zu mismatch %zu first_bad 0x%zx phys_segs %zu phys0 %pa attrs 0x%lx%s\n",
			 tag, s, &o->a6l_fix[s], dom ? dom->type : 0, pages, unmapped, mism,
			 first_bad == SIZE_MAX ? 0 : first_bad, segs, &p0, o->a6l_fix_attrs,
			 (!unmapped && !mism) ? " OK" : " BAD");
	}
}

/* camfix10 bit0 (stream-off): what did the WM write? histogram, crc32, colour-bar samples of lines 0..3 */
static void a6l_v10_hist(struct vfe_device *vfe, struct vfe_output *o, unsigned int s, size_t end)
{
	const u32 *p = o->a6l_fix_cpu[s];
	size_t i, nw = end / 4, ff = 0, zero = 0, fill = 0, other = 0, bpl = o->a6l_bpl, l;
	u32 odd[4] = { 0 }, oddoff[4] = { 0 }, no = 0;
	char buf[300];

	if (!(a6l_v10 & 1) || !p || !end)
		return;
	for (i = 0; i < nw; i++) {
		if (p[i] == 0xffffffff)
			ff++;
		else if (!p[i])
			zero++;
		else if (p[i] == A6L_FILL)
			fill++;
		else {
			other++;
			if (no < 4) {
				odd[no] = p[i];
				oddoff[no++] = i * 4;
			}
		}
	}
	dev_info(vfe->camss->dev,
		 "A6L_V10 HIST slot %u end 0x%zx words %zu ff %zu zero %zu fill %zu other %zu crc32 %08x first_other %x:%08x %x:%08x %x:%08x %x:%08x\n",
		 s, end, nw, ff, zero, fill, other, crc32_le(0, (const u8 *)p, end), oddoff[0], odd[0], oddoff[1], odd[1],
		 oddoff[2], odd[2], oddoff[3], odd[3]);
	if (!bpl || (bpl & 3))
		return;
	for (l = 0; l < 4 && (l + 1) * bpl <= end; l++) {
		int len = 0, k;

		for (k = 0; k < 8; k++) { /* 8 colour bars: centre of bar k at byte (k * bpl / 8 + bpl / 16) */
			size_t w = (l * bpl + k * bpl / 8 + bpl / 16) / 4;

			len += scnprintf(buf + len, sizeof(buf) - len, " %08x", p[w]);
		}
		dev_info(vfe->camss->dev, "A6L_V10 BARS slot %u line %zu%s\n", s, l, buf);
	}
}

static int a6l_prepare_fixed(struct vfe_device *vfe, struct vfe_output *output, size_t frame)
{
''')

sub1(G1, """	output->a6l_mode = 2;
	dev_info(vfe->camss->dev, "A6L_WM vfe%u fixed mode (a6l_wm %d): ping %pad pong %pad %zu bytes each, frame %zu\\n",
		 vfe->id, a6l_wm, &output->a6l_fix[0], &output->a6l_fix[1], output->a6l_fix_sz, frame);
""", """	output->a6l_mode = 2;
	dev_info(vfe->camss->dev, "A6L_WM vfe%u fixed mode (a6l_wm %d): ping %pad pong %pad %zu bytes each, frame %zu\\n",
		 vfe->id, a6l_wm, &output->a6l_fix[0], &output->a6l_fix[1], output->a6l_fix_sz, frame);
	a6l_v10_iova(vfe, output, "START"); /* camfix10 */
""")

sub1(G1, """			 output->a6l_frame_sz, output->a6l_fix_sz, p[0], p[1], p[2], p[3]);
	}
""", """			 output->a6l_frame_sz, output->a6l_fix_sz, p[0], p[1], p[2], p[3]);
		a6l_v10_hist(vfe, output, s, cnt ? (last + 1) * 4 : 0); /* camfix10 */
	}
	a6l_v10_iova(vfe, output, "STOP"); /* camfix10: mapping unchanged after the stream? */
""")

sub1(G1, "	a6l_stk_stop(vfe, line->id); /* camfix9b: stop the per-frame reg-update chain first */\n",
     "	a6l_stk_stop(vfe, line->id); /* camfix9b: stop the per-frame reg-update chain first */\n	a6l_v10_stop(vfe, line->id); /* camfix10 */\n")
sub1(G1, "		a6l_stk_start(vfe, output->wm_idx[0], line->id); /* camfix9b */\n",
     "		a6l_stk_start(vfe, output->wm_idx[0], line->id); /* camfix9b */\n	if (line->id != VFE_LINE_PIX)\n		a6l_v10_start(vfe, output->wm_idx[0], line->id); /* camfix10 */\n")

# ---------------- CSID: forced test generator + poll hooks ----------------
sub1(CSID, "static u32 a6l_csid_nirq[4];\n",
"""static u32 a6l_csid_nirq[4];
static bool a6l_v10_tg_forced[4]; /* camfix10 bit1 */
""")
sub1(CSID, """	if (enable) {
		struct v4l2_mbus_framefmt *input_format;
		const struct csid_format_info *format;
		u8 vc = 0; /* Virtual Channel 0 */
		u8 cid = vc * 4; /* id of Virtual Channel and Data Type set */
		u8 dt_shift;

		if (tg->enabled) {
""", """	if (enable) {
		struct v4l2_mbus_framefmt *input_format;
		const struct csid_format_info *format;
		u8 vc = 0; /* Virtual Channel 0 */
		u8 cid = vc * 4; /* id of Virtual Channel and Data Type set */
		u8 dt_shift;

		/* camfix10 bit1: CSID test generator instead of the CSIPHY input (sensor keeps streaming, ignored) */
		if ((a6l_v10 & 2) && !tg->enabled) {
			tg->enabled = 1;
			tg->mode = (a6l_v10_tpg >= 1 && a6l_v10_tpg <= 5) ? a6l_v10_tpg : 1;
			a6l_v10_tg_forced[csid->id & 3] = true;
		}
		if (tg->enabled) {
""")
sub1(CSID, """			num_bytes_per_line = input_format->width * format->bpp * format->spp / 8;
			num_lines = input_format->height;

			/* 31:24 V blank, 23:13 H blank, 3:2 num of active DT */
			/* 1:0 VC */
			val = ((CAMSS_CSID_TG_VC_CFG_V_BLANKING & 0xff) << 24) |
				  ((CAMSS_CSID_TG_VC_CFG_H_BLANKING & 0x7ff) << 13);
			writel_relaxed(val, csid->base + CAMSS_CSID_TG_VC_CFG);
""", """			num_bytes_per_line = input_format->width * format->bpp * format->spp / 8;
			num_lines = input_format->height;
			if (a6l_v10 & 32) { /* camfix10: small frame 1280x720 RAW10 */
				num_bytes_per_line = 1600;
				num_lines = 720;
			}

			/* 31:24 V blank, 23:13 H blank, 3:2 num of active DT */
			/* 1:0 VC */
			val = ((CAMSS_CSID_TG_VC_CFG_V_BLANKING & 0xff) << 24) |
				  ((CAMSS_CSID_TG_VC_CFG_H_BLANKING & 0x7ff) << 13);
			if (a6l_v10 & (32 | 64)) /* camfix10: maximum blanking */
				val = (0xffu << 24) | (0x7ffu << 13);
			writel_relaxed(val, csid->base + CAMSS_CSID_TG_VC_CFG);
			dev_info(csid->camss->dev, "A6L_V10 TPG csid%u forced %u mode %u (%s) bytes/line %u lines %u vc_cfg %08x dt 0x%02x\\n",
				 csid->id, a6l_v10_tg_forced[csid->id & 3], tg->mode,
				 tg->mode < 7 ? csid_testgen_modes[tg->mode] : "?", num_bytes_per_line, num_lines, val,
				 format->data_type);
""")
sub1(CSID, """		if (a6l_dbg)
			a6l_csid_dump(csid, "START");
""", """		if (a6l_dbg)
			a6l_csid_dump(csid, "START");
		a6l_v10_csid_on(csid->base, csid->id); /* camfix10: per-frame status poll from the VFE IRQ */
""")
sub1(CSID, """		if (tg->enabled) {
			val = CAMSS_CSID_TG_CTRL_DISABLE;
			writel_relaxed(val, csid->base + CAMSS_CSID_TG_CTRL);
		}
""", """		a6l_v10_csid_off(); /* camfix10 */
		if (tg->enabled) {
			val = CAMSS_CSID_TG_CTRL_DISABLE;
			writel_relaxed(val, csid->base + CAMSS_CSID_TG_CTRL);
		}
		if (a6l_v10_tg_forced[csid->id & 3]) { /* camfix10: back to the control's state */
			tg->enabled = 0;
			a6l_v10_tg_forced[csid->id & 3] = false;
		}
""")

# ---------------- CSIPHY: poll hooks ----------------
sub1(PHY, """	dev_info(csiphy->camss->dev, "A6L_CSIPHY%d link_freq %lld timer %u settle_cnt %u lanes %u mask 0x%x csid %u\\n",
		 csiphy->id, (long long)link_freq, csiphy->timer_clk_rate, settle_cnt, c->num_data, lane_mask, cfg->csid_id);
""", """	dev_info(csiphy->camss->dev, "A6L_CSIPHY%d link_freq %lld timer %u settle_cnt %u lanes %u mask 0x%x csid %u\\n",
		 csiphy->id, (long long)link_freq, csiphy->timer_clk_rate, settle_cnt, c->num_data, lane_mask, cfg->csid_id);
	a6l_v10_phy_on(csiphy->base, regs->offset, regs->common_status_offset, csiphy->id); /* camfix10 */
""")
sub1(PHY, """	if (a6l_dbg) {
		a6l_phy_status(csiphy, "STOP");
		a6l_phy_nirq[csiphy->id % 3] = 0;
	}
""", """	a6l_v10_phy_off(); /* camfix10 */
	if (a6l_dbg) {
		a6l_phy_status(csiphy, "STOP");
		a6l_phy_nirq[csiphy->id % 3] = 0;
	}
""")

# ---------------- video: sg_table per vb2 buffer ----------------
sub1(VID, """		buffer->addr[i] = sg_dma_address(sgt->sgl);
	}
""", """		buffer->addr[i] = sg_dma_address(sgt->sgl);
		if ((a6l_v10 & 1) && vb->index < 4) { /* camfix10: lens A, how is the vb2 buffer mapped? */
			struct iommu_domain *dom = iommu_get_domain_for_dev(video->camss->dev);
			struct scatterlist *sg1 = sgt->nents > 1 ? sg_next(sgt->sgl) : NULL;
			dma_addr_t d0 = sg_dma_address(sgt->sgl), d1 = sg1 ? sg_dma_address(sg1) : 0;
			phys_addr_t p0 = dom ? iommu_iova_to_phys(dom, d0) : 0;

			dev_info(video->camss->dev,
				 "A6L_V10 SG buf %u plane %u nents %u orig_nents %u dma0 %pad len0 %u dma1 %pad len1 %u size %lu dom_type 0x%x phys0 %pa\\n",
				 vb->index, i, sgt->nents, sgt->orig_nents, &d0, sg_dma_len(sgt->sgl), &d1,
				 sg1 ? sg_dma_len(sg1) : 0, vb2_plane_size(vb, i), dom ? dom->type : 0, &p0);
		}
	}
""")
sub1(VID, "static int video_buf_init(struct vb2_buffer *vb)\n",
"""#include <linux/iommu.h>
static int video_buf_init(struct vb2_buffer *vb)
""")
print("CAMFIX10_PATCH_OK")
