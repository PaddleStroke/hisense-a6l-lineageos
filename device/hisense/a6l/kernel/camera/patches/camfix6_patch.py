#!/usr/bin/env python3
# camfix6 (28 Sep 2026): applied AFTER camfix_patch.py .. camfix5_patch.py.   usage: camfix6_patch.py <camss_dir>
# Input: attended run t28b (camera6 = camfix5, MODE=clkloop FDUMP=24), laptop ~/A6L-usb-20260915/v75/logs/t28b/
# c6-clk.log + dmesg-cam6.txt (repo copy .relay/outbox/cam28/).
#   - Actual VFE clock 200 / 404 / 480 / 540 MHz: every run has buserr (0xC94 = 1) and 0-1 WM done in 225-393 SOFs.
#     So the VFE core clock is not the cause (camfix5 hypothesis ruled out).
#   - The bus error always hits in the FIRST frame after the RDI0 reg-update ack (s0 bit5 at SOF n -> s1 bit4 +
#     0xC94 bit0 at SOF n+1; n = 1, 3, 5, 4 in the 4 runs). That is the first frame the WM writes.
#     0xC94 then stays 1 (sticky), s1 bit4 is never raised again, and ping-pong status 0x338 never toggles.
#   - No SMMU fault, no ISPIF/CSID error, the sensor streams. WM registers equal stock (ping/pong + MAX, ADDR_CFG 2,
#     BUFFER_CFG 2, CGC, 0xCEC enable, XBAR 0xc00, RDI_CFG 7). t27 wm4 scans: the WM writes only 5..93 of 2156 lines.
# Stock comparison (Hisense stock kernel firmware/extracted/stock-symbolized.elf + stock-00.dts qcom,vfe0@ca10000):
#   a) msm_vfe47_init_hardware_reg programs qos-* and ds-* into the VFE (upstream does too, values identical) AND
#      vbif-regs <0x124 0xac 0xd0> = <0x3 0x40 0x1010> into the camera VBIF 0xca40000 (DT reg "vfe_vbif").
#      Upstream sdm660 has no VBIF region and never writes it (VBIF_ROUND_ROBIN_QOS_ARB / WRITE_GATHER_EN /
#      OUT_RD_LIM_CONF0 stay at reset).
#   b) msm_vfe48_get_ub_size = 1904 words for image WMs (stats UB at 1904..2047). msm_vfe47_cfg_axi_ub_equal_default
#      gives an RDI WM offset rdi*192, size 2*min_wm_ub = 192 (msm_vfe48_axi_hw_info.min_wm_ub = 0x60), value (size-1).
#   c) msm_isp_request_axi_stream: for stream_src > VFE_PIX (= RDI) it calls ops->set_bus_err_ign_mask(wm, 1)
#      (the ops slot +0x1c8 = msm_vfe48_set_bus_err_ign_mask), so stock IGNORES 0xC94 for RDI WMs.
#   d) msm_vfe47_axi_cfg_wm_reg: frame-based -> BUFFER_CFG = ADDR_CFG value; line-based -> IMAGE_SIZE =
#      ((wpl+3)/4-1)<<16 | (h-1), BUFFER_CFG = 3 (burst) | (h-1)<<2 | ((stride_wpl+1)/2)<<16.
#   e) stock bus vote: msm-bus MASTER_VFE(29)->EBI(512) with dynamic ab/ib; upstream-A6L votes 1 GB/s / 2 GB/s.
# Reading: the WM is starved on the AXI side (clock independent, first written frame, a few lines only). The
#   candidates that differ from stock are VBIF QoS (a), UB depth (b), bandwidth vote (e) and error recovery (c);
#   line-based vs frame-based (d) and the burst field are the remaining WM-programming differences.
# Changes (all runtime-switchable through qcom_camss.a6l_v6, default 0x0f):
#   bit0 (1)   apply the stock VBIF settings (ioremap 0x0ca40000; 0x124=3, 0xac=0x40, 0xd0=0x1010) at first stream-on
#   bit1 (2)   WM0 UB = whole stock image UB (offset 0, 1904 words; VFE1 capped at 1392) when it is the only stream
#              and belongs to an RDI line (never for the multi-WM PIX path)
#   bit2 (4)   interconnect vote a6l_icc_avg / a6l_icc_peak kBps (default 2000000 / 4000000) at stream-on
#   bit3 (8)   bus-error recovery: at RDI SOF, if 0xC94 has a NEW bit, write it back (W1C try) + reload that WM
#   bit4 (16)  RDI WM line-based like stock (IMAGE_SIZE + BUFFER_CFG burst 3, stride), instead of frame-based
#   bit5 (32)  frame-based BUFFER_CFG burst field 3 instead of the stock ADDR_CFG copy (2)
#   bit6 (64)  stock RDI UB (offset 0, 192 words = 0xBF): control for the UB theory
#   bit7 (128) (a6l_wm=4 only) per-SOF write progress probe: A6L_WM4_P lines = lines written in the last frame
# Diagnostics (read-only): A6L_AXI (MMSS AXI RCG 0xc8cd000 cmd/cfg -> source/divider), A6L_VBIF (VBIF QoS + error
#   registers, only while the VFE is powered), A6L_ICC, A6L_VFE<n>_RECOVER, A6L_VFE<n>_V6 summary at stream-off.
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

# ---- header: bytes per line for the progress probe -------------------------------------------------------------
sub1(H, "\tstruct vfe_device *a6l_vfe;\n",
     "\tstruct vfe_device *a6l_vfe;\n\tu32 a6l_bpl; /* camfix6: bytes per line (progress probe) */\n")

# ---- 4-8: params, VBIF / AXI diagnostics ------------------------------------------------------------------------
sub1(V48, "static ktime_t a6l_f_t0[2];\n",
r'''static ktime_t a6l_f_t0[2];
/* camfix6: stock-parity switches, see camfix6_patch.py header */
uint a6l_v6 = 0x0f;
module_param(a6l_v6, uint, 0644);
MODULE_PARM_DESC(a6l_v6, "A6L camfix6: bit0 stock VBIF QoS, bit1 WM0 UB 1904, bit2 ICC vote, bit3 bus-error recovery, bit4 RDI line-based WM, bit5 frame-based burst 3, bit6 stock RDI UB 192, bit7 wm4 progress probe (default 0x0f)");
static uint a6l_pdump = 40;
module_param(a6l_pdump, uint, 0644);
MODULE_PARM_DESC(a6l_pdump, "A6L camfix6: number of SOFs the a6l_v6 bit7 progress probe logs per stream");
static u32 a6l_f_rec[2], a6l_f_probe[2], a6l_be_last[2];
static void __iomem *a6l_vbif_base, *a6l_axi_rcg;
void a6l_fix_probe(struct vfe_device *vfe, u32 sof); /* camss-vfe-gen1.c */
void a6l_wm_rdi_line_based(struct vfe_device *vfe, u8 wm, u32 bpl, u32 height);

#define A6L_VBIF_PHYS	0x0ca40000 /* stock DT qcom,vfe0 reg "vfe_vbif" <0xca40000 0x3000> */
#define A6L_AXI_RCG	0x0c8cd000 /* mmcc axi_clk_src CMD_RCGR (RPM owned MMSS NoC AXI) */

static void a6l_v6_map(void)
{
	if (!a6l_vbif_base)
		a6l_vbif_base = ioremap(A6L_VBIF_PHYS, 0x3000);
	if (!a6l_axi_rcg)
		a6l_axi_rcg = ioremap(A6L_AXI_RCG, 0x10);
}

/* read-only: MMSS AXI clock source and camera VBIF state (VBIF only while the VFE clocks are on) */
static void a6l_v6_regs(struct vfe_device *vfe, const char *tag)
{
	static const u32 src_khz[8] = { 19200, 808000, 0, 0, 0, 600000, 300000, 0 };

	a6l_v6_map();
	if (a6l_axi_rcg) {
		u32 cmd = readl_relaxed(a6l_axi_rcg), cfg = readl_relaxed(a6l_axi_rcg + 4);
		u32 src = (cfg >> 8) & 7, hid = cfg & 0x1f;

		dev_info(vfe->camss->dev, "A6L_AXI %s rcg cmd %08x cfg %08x src %u hid %u ~%u kHz%s\n", tag, cmd, cfg,
			 src, hid, src_khz[src] ? src_khz[src] * 2 / (hid + 1) : 0, (cmd & BIT(31)) ? " ROOT_OFF" : "");
	}
	if (a6l_vbif_base && vfe->power_count > 0)
		dev_info(vfe->camss->dev,
			 "A6L_VBIF %s ver %08x clkforce %08x wgather(ac) %08x inrd(b0) %08x inwr(c0) %08x outrd(d0) %08x outwr(d4) %08x rrqos(124) %08x amem(160) %08x pnderr(190) %08x srcerr(194) %08x halt %08x %08x\n",
			 tag, readl_relaxed(a6l_vbif_base), readl_relaxed(a6l_vbif_base + 0x8),
			 readl_relaxed(a6l_vbif_base + 0xac), readl_relaxed(a6l_vbif_base + 0xb0),
			 readl_relaxed(a6l_vbif_base + 0xc0), readl_relaxed(a6l_vbif_base + 0xd0),
			 readl_relaxed(a6l_vbif_base + 0xd4), readl_relaxed(a6l_vbif_base + 0x124),
			 readl_relaxed(a6l_vbif_base + 0x160), readl_relaxed(a6l_vbif_base + 0x190),
			 readl_relaxed(a6l_vbif_base + 0x194), readl_relaxed(a6l_vbif_base + 0x200),
			 readl_relaxed(a6l_vbif_base + 0x204));
}

/* stock msm_vfe47_init_hardware_reg: vbif-regs <0x124 0xac 0xd0> = vbif-settings <0x3 0x40 0x1010> */
static void a6l_vbif_apply(struct vfe_device *vfe)
{
	a6l_v6_map();
	if (!a6l_vbif_base || vfe->power_count <= 0)
		return;
	writel_relaxed(0x3, a6l_vbif_base + 0x124);
	writel_relaxed(0x40, a6l_vbif_base + 0xac);
	writel_relaxed(0x1010, a6l_vbif_base + 0xd0);
	wmb();
}
''')

sub1(V48, "\twritel_relaxed(val7, vfe->base + VFE_0_BUS_BDG_QOS_CFG_7);\n}\n",
     "\twritel_relaxed(val7, vfe->base + VFE_0_BUS_BDG_QOS_CFG_7);\n"
     "\t/* camfix6: stock also programs the camera VBIF here (msm_vfe47_init_hardware_reg) */\n"
     "\ta6l_v6_regs(vfe, \"PRE\");\n"
     "\tif (a6l_v6 & 1)\n\t\ta6l_vbif_apply(vfe);\n"
     "\ta6l_v6_regs(vfe, \"START\");\n}\n")

# ---- 4-8: recovery + progress probe in the IRQ accounting ------------------------------------------------------
sub1(V48, "\tif (a6l_fdump && a6l_f_logged[v] < a6l_fdump && ((s1 & (BIT(29) | BIT(4))) || (s0 & GENMASK(14, 8)))) {\n",
r'''	/* camfix6 bit3: stock ignores RDI bus errors; we try to clear 0xC94 and reload the WM once per NEW error bit */
	if ((s1 & BIT(29)) && (a6l_v6 & 8)) {
		u32 be = readl_relaxed(vfe->base + 0xc94) & 0x7f;

		if (be & ~a6l_be_last[v]) {
			u32 be2;
			unsigned int w;

			writel_relaxed(be, vfe->base + 0xc94);
			for (w = 0; w < 7; w++)
				if (be & BIT(w))
					vfe_bus_reload_wm(vfe, w);
			be2 = readl_relaxed(vfe->base + 0xc94) & 0x7f;
			a6l_f_rec[v]++;
			if (a6l_f_rec[v] <= 4)
				dev_info(vfe->camss->dev, "A6L_VFE%u_RECOVER#%u sof#%u be 0x%02x -> 0x%02x (reload)\n",
					 vfe->id, a6l_f_rec[v], a6l_f_sof[v], be, be2);
			a6l_be_last[v] = be2;
		} else {
			a6l_be_last[v] = be;
		}
	}
	/* camfix6 bit7: per-frame write progress of the fixed (a6l_wm=4) slots */
	if ((s1 & BIT(29)) && (a6l_v6 & 128) && a6l_f_probe[v] < a6l_pdump) {
		a6l_f_probe[v]++;
		a6l_fix_probe(vfe, a6l_f_sof[v]);
	}
	if (a6l_fdump && a6l_f_logged[v] < a6l_fdump && ((s1 & (BIT(29) | BIT(4))) || (s0 & GENMASK(14, 8)))) {
''')

sub1(V48, "\ta6l_f_sof[v] = 0;\n\ta6l_f_done[v] = 0;\n",
     "\tdev_info(vfe->camss->dev, \"A6L_VFE%u_V6 %s v6 0x%02x rec %u probe %u be 0x%08x ub0 %08x imgsize0 %08x bufcfg0 %08x\\n\",\n"
     "\t\t vfe->id, tag, a6l_v6, a6l_f_rec[v], a6l_f_probe[v], readl_relaxed(vfe->base + 0xc94),\n"
     "\t\t readl_relaxed(vfe->base + 0xb8), readl_relaxed(vfe->base + 0xbc), readl_relaxed(vfe->base + 0xc0));\n"
     "\ta6l_v6_regs(vfe, tag);\n"
     "\ta6l_f_rec[v] = 0;\n\ta6l_f_probe[v] = 0;\n\ta6l_be_last[v] = 0;\n"
     "\ta6l_f_sof[v] = 0;\n\ta6l_f_done[v] = 0;\n")

# ---- 4-8: frame-based burst / clean IMAGE_SIZE, line-based RDI --------------------------------------------------
sub1(V48, "\t\t/* A6L: stock msm_vfe47_axi_cfg_wm_reg writes the ADDR_CFG value into BUFFER_CFG for frame-based WMs */\n"
          "\t\tif (a6l_wm & 2)\n"
          "\t\t\twritel_relaxed(readl_relaxed(vfe->base + VFE_0_BUS_IMAGE_MASTER_n_WR_ADDR_CFG(wm)),\n"
          "\t\t\t\t       vfe->base + VFE_0_BUS_IMAGE_MASTER_n_WR_BUFFER_CFG(wm));\n"
          "\t} else\n"
          "\t\tvfe_reg_clr(vfe, VFE_0_BUS_IMAGE_MASTER_n_WR_ADDR_CFG(wm),\n"
          "\t\t\t    1 << VFE_0_BUS_IMAGE_MASTER_n_WR_ADDR_CFG_FRM_BASED_SHIFT);\n}\n",
r'''		/* camfix6: a previous line-based run must not leave IMAGE_SIZE behind */
		writel_relaxed(0, vfe->base + VFE_0_BUS_IMAGE_MASTER_n_WR_IMAGE_SIZE(wm));
		/* A6L: stock msm_vfe47_axi_cfg_wm_reg writes the ADDR_CFG value into BUFFER_CFG for frame-based WMs */
		if (a6l_wm & 2) {
			u32 bc = readl_relaxed(vfe->base + VFE_0_BUS_IMAGE_MASTER_n_WR_ADDR_CFG(wm));

			if (a6l_v6 & 32) /* camfix6 bit5: burst field 3 (stock line-based VFE47_BURST_LEN) */
				bc = (bc & ~0x3U) | 0x3;
			writel_relaxed(bc, vfe->base + VFE_0_BUS_IMAGE_MASTER_n_WR_BUFFER_CFG(wm));
		}
	} else {
		vfe_reg_clr(vfe, VFE_0_BUS_IMAGE_MASTER_n_WR_ADDR_CFG(wm),
			    1 << VFE_0_BUS_IMAGE_MASTER_n_WR_ADDR_CFG_FRM_BASED_SHIFT);
		/* camfix6: also undo a line-based RDI set-up */
		writel_relaxed(0, vfe->base + VFE_0_BUS_IMAGE_MASTER_n_WR_IMAGE_SIZE(wm));
		writel_relaxed(0, vfe->base + VFE_0_BUS_IMAGE_MASTER_n_WR_BUFFER_CFG(wm));
	}
}

/* camfix6 bit4: stock line-based WM for RDI (msm_vfe47_axi_cfg_wm_reg with frame_based = 0) */
void a6l_wm_rdi_line_based(struct vfe_device *vfe, u8 wm, u32 bpl, u32 height)
{
	u32 wpl = DIV_ROUND_UP(bpl, 8), reg;

	if (!bpl || !height)
		return;
	vfe_reg_clr(vfe, VFE_0_BUS_IMAGE_MASTER_n_WR_ADDR_CFG(wm),
		    1 << VFE_0_BUS_IMAGE_MASTER_n_WR_ADDR_CFG_FRM_BASED_SHIFT);
	reg = (height - 1) | ((((wpl + 3) / 4) - 1) << 16);
	writel_relaxed(reg, vfe->base + VFE_0_BUS_IMAGE_MASTER_n_WR_IMAGE_SIZE(wm));
	reg = 0x3 | ((height - 1) << 2) | (((wpl + 1) / 2) << 16);
	writel_relaxed(reg, vfe->base + VFE_0_BUS_IMAGE_MASTER_n_WR_BUFFER_CFG(wm));
}
''')

# ---- 4-8: UB variants ------------------------------------------------------------------------------------------
sub1(V48, "\t/* camfix5 experiment: give WM0 a deeper UB (single RDI stream only) */\n",
r'''	/* camfix6: stock vfe48 image UB = 1904 words (bit1), or the stock RDI share 192 words (bit6); only for WM0
	 * while it is the only stream on this VFE, so no other WM can overlap. Stock UB_CFG value = size - 1. */
	if (wm == 0 && vfe->stream_count <= 1 && vfe->wm_output_map[0] != VFE_LINE_PIX && (a6l_v6 & 0x42)) {
		offset = 0;
		depth = (a6l_v6 & 0x40) ? 191 : (vfe->id ? 1391 : 1903);
	}
	/* camfix5 experiment: give WM0 a deeper UB (single RDI stream only) */
''')

# ---- gen1: ICC vote, bytes per line, line-based hook, progress probe --------------------------------------------
sub1(G1, "#include <linux/workqueue.h>\n", "#include <linux/workqueue.h>\n#include <linux/interconnect.h>\n")
sub1(G1, "extern int a6l_dbg; /* camss-vfe-4-8.c */\nint a6l_wm = 3;\n",
r'''extern int a6l_dbg; /* camss-vfe-4-8.c */
extern uint a6l_v6; /* camss-vfe-4-8.c */
void a6l_wm_rdi_line_based(struct vfe_device *vfe, u8 wm, u32 bpl, u32 height); /* camss-vfe-4-8.c */
void a6l_fix_probe(struct vfe_device *vfe, u32 sof);
/* camfix6 bit2: VFE -> DDR interconnect vote (kBps); upstream-A6L default is 1000000 / 2000000 */
static uint a6l_icc_avg = 2000000;
module_param(a6l_icc_avg, uint, 0644);
MODULE_PARM_DESC(a6l_icc_avg, "A6L camfix6: vfe-mem average bandwidth vote in kBps when a6l_v6 bit2 is set");
static uint a6l_icc_peak = 4000000;
module_param(a6l_icc_peak, uint, 0644);
MODULE_PARM_DESC(a6l_icc_peak, "A6L camfix6: vfe-mem peak bandwidth vote in kBps when a6l_v6 bit2 is set");
int a6l_wm = 3;
''')

sub1(G1, "\toutput->a6l_mode = 0;\n\toutput->a6l_done = output->a6l_user = output->a6l_scr = output->a6l_empty = 0;\n"
         "\tif (line->id == VFE_LINE_PIX)\n\t\treturn;\n",
r'''	output->a6l_mode = 0;
	output->a6l_done = output->a6l_user = output->a6l_scr = output->a6l_empty = 0;
	if (line->id == VFE_LINE_PIX)
		return;
	output->a6l_bpl = pix->plane_fmt[0].bytesperline;
	/* camfix6 bit2: process context; camss_runtime_suspend drops the vote again */
	if ((a6l_v6 & 4) && vfe->camss->res->icc_path_num && vfe->camss->icc_path[0]) {
		int r = icc_set_bw(vfe->camss->icc_path[0], a6l_icc_avg, a6l_icc_peak);

		dev_info(vfe->camss->dev, "A6L_ICC vfe-mem avg %u peak %u kBps ret %d (v6 0x%02x)\n",
			 a6l_icc_avg, a6l_icc_peak, r, a6l_v6);
	}
''')

sub1(G1, "\t\tvfe->ops_gen1->wm_frame_based(vfe, output->wm_idx[0], 1);\n",
r'''		if ((a6l_v6 & 16) && vfe->camss->res->version == CAMSS_660)
			a6l_wm_rdi_line_based(vfe, output->wm_idx[0],
					      line->video_out.active_fmt.fmt.pix_mp.plane_fmt[0].bytesperline,
					      line->video_out.active_fmt.fmt.pix_mp.height);
		else
			vfe->ops_gen1->wm_frame_based(vfe, output->wm_idx[0], 1);
''')

sub1(G1, "\tdev_info(vfe->camss->dev, \"A6L_WM4_STOP done %u copied %u\\n\", output->a6l_done, output->a6l_copied);\n}\n",
r'''	dev_info(vfe->camss->dev, "A6L_WM4_STOP done %u copied %u\n", output->a6l_done, output->a6l_copied);
}

/*
 * camfix6 bit7 (IRQ context, at RDI SOF, fixed mode only): how many lines did the WM write since the last SOF?
 * Word 0 of every line that holds data is reset to the fill pattern after each probe, so the binary search over
 * "word 0 != fill" measures the last frame only. head = line 0 was (re)written, i.e. the WM restarted the slot.
 */
void a6l_fix_probe(struct vfe_device *vfe, u32 sof)
{
	unsigned int l, s;

	for (l = VFE_LINE_RDI0; l <= VFE_LINE_RDI2 && l < vfe->res->line_num; l++) {
		struct vfe_output *o = &vfe->line[l].output;
		u32 n[2] = { 0, 0 }, head[2] = { 0, 0 }, bpl = o->a6l_bpl, nl;

		if (o->a6l_mode != 2 || o->a6l_stop || !bpl || (bpl & 3))
			continue;
		nl = o->a6l_fix_sz / bpl;
		for (s = 0; s < 2; s++) {
			u32 *p = o->a6l_fix_cpu[s], lo = 0, hi = nl, i;

			if (!p || !nl)
				continue;
			head[s] = p[0] != A6L_FILL;
			while (lo < hi) {
				u32 m = lo + (hi - lo) / 2;

				if (p[(size_t)m * (bpl / 4)] != A6L_FILL)
					lo = m + 1;
				else
					hi = m;
			}
			n[s] = lo;
			for (i = 0; i < lo; i++)
				p[(size_t)i * (bpl / 4)] = A6L_FILL;
		}
		dev_info(vfe->camss->dev, "A6L_WM4_P sof#%u line %u slot0 head %u lines %u slot1 head %u lines %u frame_lines %zu done %u\n",
			 sof, l, head[0], n[0], head[1], n[1], o->a6l_frame_sz / bpl, o->a6l_done);
	}
}
''')
print("CAMFIX6_PATCH_PASS")
