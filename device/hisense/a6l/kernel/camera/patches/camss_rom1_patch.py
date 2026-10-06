#!/usr/bin/env python3
# camss ROM patch rom1 (29 Sep 2026): the clean sdm660 camss for the ROM.  usage: camss_rom1_patch.py <orig_camss> <camss>
# <camss> must hold the ORIGINAL baseline camss with camss-sdm660-camfix5-cumulative.patch applied (the old ROM patch).
# This script turns it into the clean ROM version:
#   - drops every camfix1-5 diagnostic (a6l_dbg register dumps, per-IRQ lines, fdump, UB experiment, IRQ-mask knob):
#     camss-csid-4-7.c, camss-csiphy-3ph-1-0.c and camss-ispif.c go back to the baseline;
#   - keeps the functional sdm660 fixes, as fixed behaviour gated on CAMSS_660 (no knobs):
#       ISPIF + CSID2/3 parent ops + vfe-mem ICC path (camss.c), nested VFE power-domain link (camss-vfe.c),
#       CSID core clock >= 310 MHz level, CSIPHY timer at its top level (269.33 MHz), VFE core clock >= 404 MHz,
#       CSIPHY clk_mux (CSIPHY2 -> CSID1 routing), WM ping/pong MAX address, CGC override, stock frame-based BUFFER_CFG;
#   - adds THE fix of 29 Sep 2026 (t35 attended PASS, both cameras): the stock CSIPHY digital clocks. For CSIPHY k,
#     cphy_csidK (-> camss_csiphyK_clk -> csiphy_clk_src) is set to 200 MHz and enabled, csiK set to 310 MHz and enabled,
#     at CSIPHY power-on; released at power-off (= camfix12 a6l_v12 bit1 without its diagnostics);
#   - keeps qcom_camss.a6l_wm (RDI write-master mode, default 6 = the mode of the t35 PASS: stock BUFFER_CFG + fixed
#     driver ping/pong copied into the queued vb2 buffer; 3 = zero-copy stock-like, 0 = upstream).
#   - lc2 (29 Sep 2026): default a6l_wm = 3 (zero-copy stock-like, needed by libcamera: vb2 buffers exported by EXPBUF are
#     written by the VFE directly, one VFE sequence step per frame). camera15 (t36): wm 3 dequeued 9/9 on imx576 AND s5k3t1
#     (ibars3/sbars3), wm 0 9/9 on imx576 (ibars0), same raw statistics as wm 6. 6 stays selectable at runtime.
import sys, re, shutil

orig, cs = sys.argv[1], sys.argv[2]

def rd(f): return open(f"{cs}/{f}").read()
def wr(f, s): open(f"{cs}/{f}", "w").write(s)

def sub1(f, old, new):
    s = rd(f)
    n = s.count(old)
    assert n == 1, f"{f}: anchor found {n}x: {old[:80]!r}"
    wr(f, s.replace(old, new))
    print(f"PATCHED {f}: {old.strip().splitlines()[0][:70]}")

def subn(f, old, new, cnt):
    s = rd(f)
    n = s.count(old)
    assert n == cnt, f"{f}: anchor found {n}x (want {cnt}): {old[:80]!r}"
    wr(f, s.replace(old, new))
    print(f"PATCHED {f} ({cnt}x): {old.strip().splitlines()[0][:70]}")

def cut(f, start, end, new, keep_end=False):
    """replace text from start (incl.) up to end (incl. unless keep_end) with new; both unique"""
    s = rd(f)
    assert s.count(start) == 1, f"{f}: start found {s.count(start)}x: {start[:80]!r}"
    i = s.index(start)
    j = s.index(end, i)
    assert s.count(end, i) >= 1
    j = j if keep_end else j + len(end)
    wr(f, s[:i] + new + s[j:])
    print(f"CUT {f}: {start.strip().splitlines()[0][:60]} .. {end.strip().splitlines()[0][:40]}")

# ---- 1. diagnostics-only files: back to the baseline ----
for f in ("camss-csid-4-7.c", "camss-csiphy-3ph-1-0.c", "camss-ispif.c"):
    shutil.copyfile(f"{orig}/{f}", f"{cs}/{f}")
    print(f"RESTORED {f}")

# ---- 2. camss-csid.c: CSID core clock at the stock 310 MHz level ----
sub1("camss-csid.c", '''static bool a6l_csid_fast = true;
module_param(a6l_csid_fast, bool, 0644);
MODULE_PARM_DESC(a6l_csid_fast, "A6L: CSID core clock >= 310 MHz (stock csi_src rate)");


''', '''/* A6L sdm660: stock runs csiK_clk_src at 310 MHz; never pick a CSID core clock level below it */
#define A6L_CSID_MIN_HZ 200000001ULL

''')
sub1("camss-csid.c", "\t\t\tif (a6l_csid_fast && min_rate && min_rate < 200000001ULL)\n\t\t\t\tmin_rate = 200000001ULL;\n",
     "\t\t\tif (csid->camss->res->version == CAMSS_660 && min_rate < A6L_CSID_MIN_HZ)\n\t\t\t\tmin_rate = A6L_CSID_MIN_HZ;\n")
cut("camss-csid.c", '\t\t\tdev_info(dev, "A6L_CSID%u %s rate %ld', "csid->phy.lane_cnt);\n", "")

# ---- 3. camss-csiphy.c: timer level + THE fix (stock CSIPHY digital clocks) ----
sub1("camss-csiphy.c", '''static bool a6l_phy_fast = true;
module_param(a6l_phy_fast, bool, 0644);
MODULE_PARM_DESC(a6l_phy_fast, "A6L: CSIPHY timer at the highest level (stock-like, 269.33 MHz)");


''', '''/* A6L sdm660: stock runs the CSIPHY timer at its top level (269.33 MHz) */
#define A6L_CSIPHY_TIMER_MIN_HZ 200000001ULL

/*
 * A6L sdm660: stock CSIPHY digital clocks. Stock (sdm660-camera.dtsi, csiphy@c82{4,5,6}000) enables
 * MMSS_CAMSS_CSIPHYk_CLK with CSIPHY_CLK_SRC at 200 MHz, MMSS_CAMSS_CPHY_CSIDk_CLK and CSIk_CLK (CSIk_CLK_SRC
 * 310 MHz). Mainline csiphy_res_660 names none of them and never sets csiphy_clk_src, which then runs at its
 * 19.2 MHz XO rate: the CSIPHY lane logic corrupts the byte stream (every packet an ECC event, spurious frame
 * starts, one VFE bus error, no complete frame; A6L runs t30-t34). camss_cphy_csidK_clk's parent is
 * camss_csiphyK_clk -> csiphy_clk_src, so setting and enabling "cphy_csidK" of the camss node gives the stock
 * chain. Proven 29 Sep 2026 (t35): imx576 on CSIPHY0 and s5k3t1 on CSIPHY2, full frames, 0 ECC/CRC, 0 bus errors.
 */
#define A6L_CSIPHY_SRC_HZ 200000000UL
#define A6L_CSI_SRC_HZ 310000000UL

static struct clk *a6l_csiphy_clk_on(struct device *dev, const char *fmt, u8 id, unsigned long rate)
{
	struct clk *c;
	char n[16];
	int ret;

	snprintf(n, sizeof(n), fmt, id);
	c = clk_get(dev, n);
	if (IS_ERR(c)) {
		dev_warn(dev, "CSIPHY%u: no clock %s (%ld)\\n", id, n, PTR_ERR(c));
		return NULL;
	}
	if (clk_get_rate(c) != rate) {
		ret = clk_set_rate(c, rate);
		if (ret)
			dev_warn(dev, "CSIPHY%u: %s set_rate(%lu) failed: %d\\n", id, n, rate, ret);
	}
	ret = clk_prepare_enable(c);
	if (ret) {
		dev_warn(dev, "CSIPHY%u: %s enable failed: %d\\n", id, n, ret);
		clk_put(c);
		return NULL;
	}
	return c;
}

static void a6l_csiphy_clk_off(struct clk **c)
{
	if (!*c)
		return;
	clk_disable_unprepare(*c);
	clk_put(*c);
	*c = NULL;
}

static void a6l_csiphy_stock_clocks(struct csiphy_device *csiphy, bool on)
{
	struct device *dev = csiphy->camss->dev;

	if (csiphy->camss->res->version != CAMSS_660)
		return;
	if (on) {
		csiphy->a6l_phy_clk = a6l_csiphy_clk_on(dev, "cphy_csid%u", csiphy->id, A6L_CSIPHY_SRC_HZ);
		csiphy->a6l_csi_clk = a6l_csiphy_clk_on(dev, "csi%u", csiphy->id, A6L_CSI_SRC_HZ);
	} else {
		a6l_csiphy_clk_off(&csiphy->a6l_csi_clk);
		a6l_csiphy_clk_off(&csiphy->a6l_phy_clk);
	}
}

''')
sub1("camss-csiphy.c", "\t\t\tif (a6l_phy_fast && min_rate && min_rate < 200000001ULL)\n\t\t\t\tmin_rate = 200000001ULL;\n",
     "\t\t\tif (csiphy->camss->res->version == CAMSS_660 && min_rate < A6L_CSIPHY_TIMER_MIN_HZ)\n\t\t\t\tmin_rate = A6L_CSIPHY_TIMER_MIN_HZ;\n")
sub1("camss-csiphy.c", "\t\t\treturn ret;\n\t\t}\n\n\t\tenable_irq(csiphy->irq);\n\n\t\tcsiphy->res->hw_ops->reset(csiphy);\n",
     "\t\t\treturn ret;\n\t\t}\n\n\t\ta6l_csiphy_stock_clocks(csiphy, true);\n\n\t\tenable_irq(csiphy->irq);\n\n\t\tcsiphy->res->hw_ops->reset(csiphy);\n")
sub1("camss-csiphy.c", "\t\tdisable_irq(csiphy->irq);\n\n\t\tcamss_disable_clocks(csiphy->nclocks, csiphy->clock);\n",
     "\t\tdisable_irq(csiphy->irq);\n\n\t\ta6l_csiphy_stock_clocks(csiphy, false);\n\n\t\tcamss_disable_clocks(csiphy->nclocks, csiphy->clock);\n")
sub1("camss-csiphy.h", "\tconst struct csiphy_subdev_resources *res;\n\tstruct csiphy_device_regs *regs;\n};\n",
     "\tconst struct csiphy_subdev_resources *res;\n\tstruct csiphy_device_regs *regs;\n"
     "\tstruct clk *a6l_phy_clk; /* A6L sdm660: cphy_csidK at 200 MHz (stock CSIPHY digital clock) */\n"
     "\tstruct clk *a6l_csi_clk; /* A6L sdm660: csiK at 310 MHz (stock) */\n};\n")

# ---- 4. camss-vfe.c: VFE core clock >= 404 MHz (stock levels 404/480/576) without a knob ----
sub1("camss-vfe.c", '''#include <linux/moduleparam.h>
/* camfix5: stock sdm660 never clocks the VFE below 404 MHz (DT vfe_clk_src levels 404/480/576 MHz); upstream
 * picks 120 MHz for IMX576 full-res RDI and the WM0 unified buffer overflows (bus error 0xC94) mid-frame. */
static uint a6l_vfe_min = 404000000;
module_param(a6l_vfe_min, uint, 0644);
MODULE_PARM_DESC(a6l_vfe_min, "A6L: minimum VFE core clock in Hz (default 404000000 = stock; 0 = upstream pixel-rate based)");


''', '''/* A6L sdm660: stock never clocks the VFE below 404 MHz (DT vfe_clk_src levels 404/480/576 MHz); upstream
 * picks 120 MHz for IMX576 full-res RDI and the WM0 unified buffer overflows mid-frame. */
#define A6L_VFE_MIN_HZ 404000000ULL

''')
subn("camss-vfe.c", "\t\t\tif (a6l_vfe_min && min_rate < a6l_vfe_min)\n\t\t\t\tmin_rate = a6l_vfe_min;\n",
     "\t\t\tif (vfe->camss->res->version == CAMSS_660 && min_rate < A6L_VFE_MIN_HZ)\n\t\t\t\tmin_rate = A6L_VFE_MIN_HZ;\n", 2)
sub1("camss-vfe.c", 'dev_warn(vfe->camss->dev, "A6L_VFE%u pm_domain_off without link', 'dev_warn(vfe->camss->dev, "VFE%u: pm_domain_off without link')
cut("camss-vfe.c", '\t\t\tdev_info(dev, "A6L_VFE%u %s rate %ld (min %llu)', "(unsigned long long)min_rate);\n", "")

# ---- 5. camss-vfe.h: only the fields the kept WM modes use ----
cut("camss-vfe.h", "\t/* A6L camfix3: stock-like RDI write master", "\tstruct vfe_device *a6l_vfe;\n", '''	/* A6L sdm660 RDI write master (qcom_camss.a6l_wm): a6l_mode 1 = stock-like every frame + scratch buffer,
	 * 2 = fixed driver ping/pong copied into the queued user buffer */
	int a6l_mode;
	dma_addr_t a6l_scratch;
	void *a6l_scratch_cpu;
	size_t a6l_scratch_sz;
	void *a6l_fix_cpu[2];
	dma_addr_t a6l_fix[2];
	size_t a6l_fix_sz, a6l_frame_sz;
	u32 a6l_copy_seq;
	u64 a6l_copy_ts;
	int a6l_copy_slot, a6l_busy, a6l_stop, a6l_work_ok;
	struct work_struct a6l_work;
	struct vfe_device *a6l_vfe;
''')

sub1("camss-vfe.h", "struct camss_subdev_resources;\n", "struct camss_subdev_resources;\n\n"
     "/* A6L sdm660 RDI write master (camss-vfe-gen1.c / camss-vfe-4-8.c) */\n"
     "extern int a6l_wm;\n"
     "void a6l_wm_set_size(struct vfe_device *vfe, u8 wm, u32 size);\n")

# ---- 6. camss-vfe-4-8.c: WM MAX address + CGC override + BUFFER_CFG; no diagnostics ----
cut("camss-vfe-4-8.c", "#include <linux/moduleparam.h>\n#include <linux/ktime.h>\nextern int a6l_wm;", "#define VFE_0_GLOBAL_RESET_CMD\t\t0x018\n", '''/* A6L sdm660 (stock msm_vfe47_update_ping_pong_addr): WR_PING/PONG_MAX_ADDR = (addr + size) & ~0x1f.
 * Left at 0 every burst is out of range: bus error (0xC94), no frame done. */
static u32 a6l_wm_sz[2][8];
void a6l_wm_set_size(struct vfe_device *vfe, u8 wm, u32 size)
{
	if (wm < 8)
		a6l_wm_sz[vfe->id & 1][wm] = size;
}

static u32 a6l_wm_max(struct vfe_device *vfe, u8 wm, u32 addr)
{
	u64 end;

	if (!addr || wm >= 8 || !a6l_wm_sz[vfe->id & 1][wm])
		return 0;
	end = (u64)addr + a6l_wm_sz[vfe->id & 1][wm];
	if (end > 0xffffffffULL)
		end = 0xffffffffULL;
	return (u32)end & ~0x1fU;
}

''', keep_end=True)
cut("camss-vfe-4-8.c", "\t/* camfix5 experiment: give WM0 a deeper UB", "\t}\n", "")
sub1("camss-vfe-4-8.c", "\t\t/* A6L: stock msm_vfe47_axi_cfg_wm_reg writes", "\t\t/* A6L (a6l_wm bit1): stock msm_vfe47_axi_cfg_wm_reg writes")
sub1("camss-vfe-4-8.c", "\t/* camfix4: WR_PING_MAX_ADDR (stock), 0 = every burst out of range -> bus error, no done */\n",
     "\t/* A6L: WR_PING_MAX_ADDR (stock) */\n")
sub1("camss-vfe-4-8.c", "\t/* camfix4: WR_PONG_MAX_ADDR (stock) */\n", "\t/* A6L: WR_PONG_MAX_ADDR (stock) */\n")
sub1("camss-vfe-4-8.c", "\t/* camfix4: stock msm_vfe47_axi_update_cgc_override (called for every stream at START/STOP) */\n\tif (!a6l_cgc && enable)\n\t\treturn;\n",
     "\t/* A6L: stock msm_vfe47_axi_update_cgc_override (called for every stream at START/STOP) */\n")
sub1("camss-vfe-4-8.c", "\ta6l_vfe_count(vfe, value0, value1);\n", "")

# ---- 7. camss-vfe-gen1.c: WM modes without diagnostics ----
cut("camss-vfe-gen1.c", "#include <linux/dma-mapping.h>\n", "void a6l_vfe_dump(struct vfe_device *vfe, const char *tag); /* camss-vfe-4-8.c */\n", r'''#include <linux/dma-mapping.h>
#include <linux/moduleparam.h>
#include <linux/workqueue.h>
#include <media/videobuf2-dma-sg.h>

/*
 * A6L sdm660 RDI write-master mode:
 *   bit0 (1) stock-like: the WM writes every frame, a driver scratch buffer takes frames no user buffer is queued for
 *   bit1 (2) stock frame-based BUFFER_CFG (= ADDR_CFG, msm_vfe47_axi_cfg_wm_reg)
 *   bit2 (4) fixed ping/pong: two driver buffers, each finished frame is copied into the next queued user buffer
 *   0 = upstream (frame-drop toggling, user buffers only)
 * Default 6 = the mode of the 29 Sep 2026 attended PASS (t35). 3 = zero-copy (confirm run camera15 first).
 */
int a6l_wm = 3;
module_param(a6l_wm, int, 0644);
MODULE_PARM_DESC(a6l_wm, "A6L RDI write master: bit0 every frame + scratch buffer, bit1 stock frame-based BUFFER_CFG, bit2 fixed ping/pong + copy; 0 = upstream (default 3, zero-copy)");

static u32 a6l_slot_addr(struct vfe_output *output, int slot, unsigned int plane)
{
	if (output->a6l_mode == 2)
		return (u32)output->a6l_fix[slot & 1];
	return output->buf[slot] ? output->buf[slot]->addr[plane] : (u32)output->a6l_scratch;
}

/* fixed mode: copy the slot the WM just finished into the next queued user buffer (process context) */
static void a6l_fix_work(struct work_struct *work)
{
	struct vfe_output *output = container_of(work, struct vfe_output, a6l_work);
	struct vfe_device *vfe = output->a6l_vfe;
	struct camss_buffer *b = NULL;
	struct sg_table *sgt;
	unsigned long flags;
	void *dst = NULL;
	size_t n;
	u32 seq;
	u64 ts;
	int slot;

	spin_lock_irqsave(&vfe->output_lock, flags);
	slot = output->a6l_copy_slot & 1;
	seq = output->a6l_copy_seq;
	ts = output->a6l_copy_ts;
	if (!output->a6l_stop)
		b = vfe_buf_get_pending(output);
	spin_unlock_irqrestore(&vfe->output_lock, flags);
	if (b) {
		n = min_t(size_t, output->a6l_frame_sz, vb2_plane_size(&b->vb.vb2_buf, 0));
		dst = vb2_plane_vaddr(&b->vb.vb2_buf, 0);
		if (dst) {
			memcpy(dst, output->a6l_fix_cpu[slot], n);
			/* written through the CPU cache: clean it before vb2 invalidates for the CPU */
			sgt = vb2_dma_sg_plane_desc(&b->vb.vb2_buf, 0);
			if (sgt)
				dma_sync_sgtable_for_device(vfe->camss->dev, sgt, DMA_TO_DEVICE);
		}
		b->vb.vb2_buf.timestamp = ts;
		b->vb.sequence = seq;
		vb2_buffer_done(&b->vb.vb2_buf, dst ? VB2_BUF_STATE_DONE : VB2_BUF_STATE_ERROR);
	}
	WRITE_ONCE(output->a6l_busy, 0);
}

static int a6l_prepare_fixed(struct vfe_device *vfe, struct vfe_output *output, size_t frame)
{
	size_t sz = PAGE_ALIGN(2 * frame);
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
				dev_err(vfe->camss->dev, "VFE%u: fixed WM buffer %u (%zu bytes) alloc failed\n",
					vfe->id, s, sz);
				if (s) {
					dma_free_attrs(vfe->camss->dev, sz, output->a6l_fix_cpu[0], output->a6l_fix[0], 0);
					output->a6l_fix_cpu[0] = NULL;
				}
				return -ENOMEM;
			}
		}
		output->a6l_fix_sz = sz;
	}
	if (!output->a6l_work_ok) {
		INIT_WORK(&output->a6l_work, a6l_fix_work);
		output->a6l_work_ok = 1;
	}
	output->a6l_vfe = vfe;
	output->a6l_frame_sz = frame;
	output->a6l_busy = 0;
	output->a6l_stop = 0;
	output->a6l_mode = 2;
	return 0;
}

/* fixed mode stream-off: stop the copies */
static void a6l_fixed_stop(struct vfe_device *vfe, struct vfe_output *output)
{
	unsigned long flags;

	spin_lock_irqsave(&vfe->output_lock, flags);
	output->a6l_stop = 1;
	spin_unlock_irqrestore(&vfe->output_lock, flags);
	if (output->a6l_work_ok)
		cancel_work_sync(&output->a6l_work);
}

/* before vfe_get_output/vfe_enable_output: sleeping allocation, outside output_lock */
static void a6l_prepare_output(struct vfe_device *vfe, struct vfe_line *line)
{
	struct vfe_output *output = &line->output;
	struct v4l2_pix_format_mplane *pix = &line->video_out.active_fmt.fmt.pix_mp;
	size_t sz = 0;
	unsigned int i;

	output->a6l_mode = 0;
	if (line->id == VFE_LINE_PIX || vfe->camss->res->version != CAMSS_660)
		return;
	for (i = 0; i < pix->num_planes && i < ARRAY_SIZE(pix->plane_fmt); i++)
		sz = max_t(size_t, sz, pix->plane_fmt[i].sizeimage);
	if (!sz)
		return;
	if ((a6l_wm & 4) && !a6l_prepare_fixed(vfe, output, sz))
		return;
	if (!(a6l_wm & 1))
		return;
	sz = PAGE_ALIGN(sz);
	if (output->a6l_scratch_cpu && output->a6l_scratch_sz < sz) {
		dma_free_attrs(vfe->camss->dev, output->a6l_scratch_sz, output->a6l_scratch_cpu,
			       output->a6l_scratch, DMA_ATTR_NO_KERNEL_MAPPING);
		output->a6l_scratch_cpu = NULL;
	}
	if (!output->a6l_scratch_cpu) {
		output->a6l_scratch_cpu = dma_alloc_attrs(vfe->camss->dev, sz, &output->a6l_scratch,
							  GFP_KERNEL, DMA_ATTR_NO_KERNEL_MAPPING);
		if (!output->a6l_scratch_cpu) {
			dev_err(vfe->camss->dev, "VFE%u: WM scratch alloc %zu failed: upstream WM mode\n", vfe->id, sz);
			return;
		}
		output->a6l_scratch_sz = sz;
	}
	output->a6l_mode = 1;
}
''')
cut("camss-vfe-gen1.c", "\tif (line->output.a6l_mode)\n\t\tdev_info(vfe->camss->dev, \"A6L_WM%u_STOP", "line->output.sequence);\n", "")
sub1("camss-vfe-gen1.c", "\ta6l_vfe_dump(vfe, \"STOP\");\n", "")
sub1("camss-vfe-gen1.c", "\t/* camfix4: size used for WR_PING/PONG_MAX_ADDR (RDI frame-based WMs only) */\n",
     "\t/* A6L: size used for WR_PING/PONG_MAX_ADDR (RDI frame-based WMs only) */\n")
sub1("camss-vfe-gen1.c", "(output->a6l_mode == 2 && !(a6l_wm & 16)) ?", "output->a6l_mode == 2 ?")
cut("camss-vfe-gen1.c", "\tif (a6l_dbg && line->id != VFE_LINE_PIX) {\n", "readl_relaxed(vfe->base + b + 0x28));\n\t}\n\n", "")
cut("camss-vfe-gen1.c", "\tif (output->a6l_mode == 2) {\n\t\tint slot = !active_index;\n",
    "\t\tif (ready_buf)\n\t\t\tvb2_buffer_done(&ready_buf->vb.vb2_buf, VB2_BUF_STATE_DONE);\n\t\treturn;\n\t}\n", r'''	if (output->a6l_mode == 2) {
		int slot = !active_index;
		int q = 0;

		output->sequence++;
		if (!output->a6l_stop && !output->a6l_busy && !list_empty(&output->pending_bufs)) {
			output->a6l_busy = 1;
			output->a6l_copy_slot = slot;
			output->a6l_copy_seq = output->sequence - 1;
			output->a6l_copy_ts = ts;
			q = 1;
		}
		spin_unlock_irqrestore(&vfe->output_lock, flags);
		if (q)
			queue_work(system_highpri_wq, &output->a6l_work);
		return;
	}

	if (output->a6l_mode) {
		/* status bit = slot the WM writes now; the other slot just completed (= stock pingpong_bit) */
		int slot = !active_index;

		output->gen1.active_buf = active_index;
		ready_buf = output->buf[slot];
		if (ready_buf) {
			ready_buf->vb.vb2_buf.timestamp = ts;
			ready_buf->vb.sequence = output->sequence;
		}
		output->sequence++;
		output->buf[slot] = vfe_buf_get_pending(output);
		for (i = 0; i < output->wm_num; i++) {
			if (slot)
				vfe->ops_gen1->wm_set_pong_addr(vfe, output->wm_idx[i], a6l_slot_addr(output, slot, i));
			else
				vfe->ops_gen1->wm_set_ping_addr(vfe, output->wm_idx[i], a6l_slot_addr(output, slot, i));
		}
		spin_unlock_irqrestore(&vfe->output_lock, flags);
		if (ready_buf)
			vb2_buffer_done(&ready_buf->vb.vb2_buf, VB2_BUF_STATE_DONE);
		return;
	}
''')

# ---- 8. nothing diagnostic may remain ----
import glob, os
bad = 0
for p in sorted(glob.glob(f"{cs}/*.[ch]")):
    s = open(p).read()
    for pat in ("a6l_dbg", "A6L_", "a6l_v1", "a6l_v6", "a6l_v7", "a6l_v8", "a6l_v9", "a6l_stk", "a6l_cgc", "a6l_wmmax", "a6l_fdump", "a6l_ub", "a6l_csid_fast", "a6l_phy_fast", "a6l_vfe_min", "a6l_done", "a6l_user", "a6l_scr ", "a6l_empty", "a6l_copied", "a6l_slot_done", "a6l_fill", "A6L_FILL"):
        for m in re.finditer(re.escape(pat), s):
            line = s[s.rfind("\n", 0, m.start()) + 1: s.find("\n", m.start())]
            if pat == "A6L_" and re.search(r"A6L_(CSID_MIN_HZ|CSIPHY_TIMER_MIN_HZ|CSIPHY_SRC_HZ|CSI_SRC_HZ|VFE_MIN_HZ)", line):
                continue
            print(f"LEFTOVER {os.path.basename(p)}: {line.strip()[:100]}")
            bad += 1
print("ROM1_STRIP", "PASS" if not bad else f"FAIL {bad}")
sys.exit(1 if bad else 0)
