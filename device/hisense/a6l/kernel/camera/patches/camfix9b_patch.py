#!/usr/bin/env python3
# camfix9b (30 Sep 2026): applied AFTER camfix_patch.py .. camfix9_patch.py.   usage: camfix9b_patch.py <camss_dir>
# Stock RDI parity. Source: msm camera_v2 msm_isp47.c / msm_isp48.c / msm_isp_axi_util.c / msm_isp_util.c (LA.UM sdm660,
# WSL /home/a6l/references/msm-camera-v2-sdm660/). See docs/camfix9-20260930.md "Stock RDI comparison".
# Differences between stock (vfe48 on sdm660) and mainline camss vfe-4-8 + camfix for one RDI raw stream:
#   1. Stock requests an RDI REG_UPDATE EVERY FRAME: msm_vfe47_process_reg_update(), case VFE_RAW_x:
#      "Reg Update is pseudo SOF for RDI, so request every frame" -> reg_update(vfe, RAW_x) at each RDI RUP irq.
#      Mainline issues it at stream start and on buffer/frame-drop changes only; in the camfix fixed ping/pong mode
#      (a6l_wm bit2) it is issued exactly once (one s0 0x20 irq per stream in every t29..t31 log).
#   2. Stock bus-error handling (vfe48 only): msm_vfe48_get_bus_err_mask() reads 0xC94, drops the bits of RDI WMs
#      (bus_err_ign_mask, set in msm_isp_axi_reserve_wm for stream_src >= RDI_INTF_0) and, when nothing is left,
#      clears irq_status1 bit4, so msm_isp_process_overflow_irq() never halts/recovers. 0xC94 is never written or
#      cleared, no WM reload, no dump. IRQ_MASK_1 = 0xFFFFFF7E (bit4 enabled).
#   3. Stock start order (msm_isp_start_axi_stream): framedrop (pattern 1, period-1 = 0), ping+pong, WM irq mask,
#      enable WM, reg_update(src), THEN reload_wm, THEN msm_isp_input_enable -> cfg_ub: RDI WM UB = 2 * min_wm_ub
#      = 192 words at offset (rdi - RAW_0) * 192 (UB_CFG = off << 16 | 191). Mainline: enable, reload, reg_update, and
#      UB 682 words (ub_size/3).
#   4. Stock re-programs the finished ping/pong address (+ max = (addr + size) & ~0x1f) at every buf done
#      (msm_isp_process_axi_irq_stream -> msm_isp_cfg_ping_pong_address). camfix fixed mode never rewrites it.
#   Same in both (no change): frame-based ADDR_CFG 0x2 / BUFFER_CFG = ADDR_CFG value, IRQ_SUBSAMPLE 0xffffffff,
#   XBAR 0xC00 for RDI0, RDI_CFG (rdi*3)<<28 | cid<<4 | 1<<2, WM enable via 0xCEC (vfe48), BUS_CFG 0x101, VBIF values,
#   irq clear sequence (0x64/0x68 + 0x58), CGC override per WM.
# New runtime param qcom_camss.a6l_stk (default 0 = unchanged behaviour; counters are always logged):
#   bit0 (1)  RDI reg-update every frame, re-requested at each RDI REG_UPDATE irq (stock chain)
#   bit1 (2)  also request it at each RDI SOF when none is pending (keeps the chain alive if an RUP is lost)
#   bit2 (4)  stock bus-error handling for RDI WMs: IRQ_MASK_1 bit4 on, RDI 0xC94 bits ignored: no BUSERR dumps
#             (v7/v8/v9), no v6 bit3 / a6l_wm bit3 clear+reload recovery, no state change
#   bit3 (8)  stock start order: WM reload after the reg-update request, then stock RDI UB (192 words at rdi*192)
#   bit4 (16) re-program the finished ping/pong address at every WM done (fixed mode, a6l_wm bit2)
#   Logs: A6L_STK START ... / A6L_STK BUSERR ignored ... / A6L_STK STOP ... rup_irq rup_req sof_req be_ign pp
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

sub1(H, "void a6l_v9_start(struct vfe_device *vfe);\n",
"""void a6l_v9_start(struct vfe_device *vfe);
/* A6L camfix9b: stock RDI behaviour (camss-vfe-4-8.c) */
extern uint a6l_stk;
void a6l_stk_start(struct vfe_device *vfe, u8 wm, int id);
void a6l_stk_stop(struct vfe_device *vfe, int id);
void a6l_stk_pp(struct vfe_device *vfe, u8 wm, int slot, u32 addr);
""")

# ---- param + state (before a6l_vfe_count, which uses them) ----
sub1(V48, "int a6l_wmmax = 1;\n",
r'''/* ================= camfix9b: stock msm_isp47/48 RDI behaviour ================= */
uint a6l_stk;
module_param(a6l_stk, uint, 0644);
MODULE_PARM_DESC(a6l_stk, "A6L camfix9b stock RDI: bit0 RDI reg-update every frame (re-requested at each RDI REG_UPDATE irq, msm_vfe47_process_reg_update), bit1 also request it at each RDI SOF when none is pending, bit2 stock bus-error handling (RDI WM 0xC94 bits ignored: no dump/clear/reload/recovery, IRQ_MASK_1 bit4 on), bit3 stock start order (reg-update, WM reload, then stock RDI UB 192 words), bit4 re-program the finished ping/pong address at every WM done (default 0)");
static u32 a6l_stk_ign[2];
static bool a6l_stk_run[2][3];
static u32 a6l_stk_nrup[2], a6l_stk_nreq[2], a6l_stk_nsof[2], a6l_stk_nbe[2], a6l_stk_npp[2];
static u32 a6l_stk_mask1_old[2];
static bool a6l_stk_mask1_set[2];

int a6l_wmmax = 1;
''')

sub1(V48, """	unsigned int v = vfe->id & 1, b;
	u32 n;

	if (!a6l_dbg)
		return;
	n = ++a6l_vfe_nirq[v];
""", """	unsigned int v = vfe->id & 1, b;
	bool stk_ign = false;
	u32 n;

	if (!a6l_dbg)
		return;
	n = ++a6l_vfe_nirq[v];
	/* camfix9b bit2: stock msm_vfe48_get_bus_err_mask - bus error only from RDI WMs -> ignored, nothing else done */
	if ((s1 & BIT(4)) && (a6l_stk & 4) && a6l_stk_ign[v] &&
	    !(readl_relaxed(vfe->base + 0xc94) & 0x7f & ~a6l_stk_ign[v]))
		stk_ign = true;
""")

sub1(V48, "		if (!a6l_v7_be_logged[v]) { /* camfix7: SMMU/MMCC state at the first bus error */\n",
"""		if (stk_ign && !a6l_stk_nbe[v]++)
			dev_info(vfe->camss->dev, "A6L_STK BUSERR ignored (stock: RDI WM mask 0x%02x) 0xc94 0x%08x sof#%u\\n",
				 a6l_stk_ign[v], readl_relaxed(vfe->base + 0xc94), a6l_f_sof[v]);
		if (!stk_ign && !a6l_v7_be_logged[v]) { /* camfix7: SMMU/MMCC state at the first bus error */
""")

sub1(V48, "	if ((s1 & BIT(29)) && (a6l_v6 & 8)) {\n",
     "	if ((s1 & BIT(29)) && (a6l_v6 & 8) && !(a6l_stk & 4)) { /* camfix9b bit2: stock never clears/reloads */\n")
sub1(V48, "	if ((s1 & BIT(4)) && (a6l_wm & 8)) {\n",
     "	if ((s1 & BIT(4)) && (a6l_wm & 8) && !stk_ign) {\n")

# ---- helpers + ISR hook (after all register macros, before vfe_isr) ----
sub1(V48, "static irqreturn_t vfe_isr(int irq, void *dev)\n{\n",
r'''/* camfix9b: from vfe_enable_output (output_lock held), after the line's first reg-update request */
void a6l_stk_start(struct vfe_device *vfe, u8 wm, int id)
{
	unsigned int v = vfe->id & 1;

	if (id < VFE_LINE_RDI0 || id > VFE_LINE_RDI2)
		return;
	a6l_stk_nrup[v] = a6l_stk_nreq[v] = a6l_stk_nsof[v] = a6l_stk_nbe[v] = a6l_stk_npp[v] = 0;
	a6l_stk_run[v][id] = false;
	if (!a6l_stk)
		return;
	if (a6l_stk & 8) {
		/* stock msm_isp_start_axi_stream: reg_update(src) -> reload_wm -> msm_isp_input_enable -> cfg_ub
		 * (msm_vfe47_cfg_axi_ub_equal_default, RDI: 2 * min_wm_ub = 192 words at (rdi - RAW_0) * 192) */
		vfe_bus_reload_wm(vfe, wm);
		writel_relaxed(((u32)(id - VFE_LINE_RDI0) * 192) << VFE_0_BUS_IMAGE_MASTER_n_WR_UB_CFG_OFFSET_SHIFT | 191,
			       vfe->base + VFE_0_BUS_IMAGE_MASTER_n_WR_UB_CFG(wm));
	}
	if (a6l_stk & 4) {
		/* stock msm_isp_axi_reserve_wm: "setup var to ignore bus error from RDI wm"; IRQ_MASK_1 0xFFFFFF7E */
		a6l_stk_ign[v] |= BIT(wm);
		if (!a6l_stk_mask1_set[v]) {
			a6l_stk_mask1_old[v] = readl_relaxed(vfe->base + VFE_0_IRQ_MASK_1) & BIT(4);
			a6l_stk_mask1_set[v] = true;
		}
		vfe_reg_set(vfe, VFE_0_IRQ_MASK_1, BIT(4));
	}
	a6l_stk_run[v][id] = !!(a6l_stk & 3);
	wmb();
	dev_info(vfe->camss->dev, "A6L_STK START vfe%u line %d wm %u stk 0x%02x ub %08x addrcfg %08x ign 0x%02x mask0 %08x mask1 %08x rup_pending %08x\n",
		 vfe->id, id, wm, a6l_stk, readl_relaxed(vfe->base + VFE_0_BUS_IMAGE_MASTER_n_WR_UB_CFG(wm)),
		 readl_relaxed(vfe->base + VFE_0_BUS_IMAGE_MASTER_n_WR_ADDR_CFG(wm)), a6l_stk_ign[v],
		 readl_relaxed(vfe->base + VFE_0_IRQ_MASK_0), readl_relaxed(vfe->base + VFE_0_IRQ_MASK_1), vfe->reg_update);
}

/* camfix9b: from vfe_disable_output (output_lock held), before the stop sequence */
void a6l_stk_stop(struct vfe_device *vfe, int id)
{
	unsigned int v = vfe->id & 1;

	if (id < VFE_LINE_RDI0 || id > VFE_LINE_RDI2)
		return;
	a6l_stk_run[v][id] = false;
	if (a6l_stk_mask1_set[v]) {
		if (!a6l_stk_mask1_old[v])
			vfe_reg_clr(vfe, VFE_0_IRQ_MASK_1, BIT(4));
		a6l_stk_mask1_set[v] = false;
	}
	dev_info(vfe->camss->dev, "A6L_STK STOP vfe%u line %d stk 0x%02x rup_irq %u rup_req %u sof_req %u be_ign %u pp %u c94 %08x ign 0x%02x\n",
		 vfe->id, id, a6l_stk, a6l_stk_nrup[v], a6l_stk_nreq[v], a6l_stk_nsof[v], a6l_stk_nbe[v], a6l_stk_npp[v],
		 readl_relaxed(vfe->base + 0xc94), a6l_stk_ign[v]);
	a6l_stk_ign[v] = 0;
}

/* camfix9b bit4: stock msm_isp_cfg_ping_pong_address for the buffer that just completed (output_lock held) */
void a6l_stk_pp(struct vfe_device *vfe, u8 wm, int slot, u32 addr)
{
	if (slot)
		vfe_wm_set_pong_addr(vfe, wm, addr);
	else
		vfe_wm_set_ping_addr(vfe, wm, addr);
	a6l_stk_npp[vfe->id & 1]++;
}

/* camfix9b: hard IRQ, after the reg-update callbacks (which clear the pending bit) */
static void a6l_stk_isr(struct vfe_device *vfe, u32 value0, u32 value1)
{
	unsigned int v = vfe->id & 1;
	int i;

	for (i = VFE_LINE_RDI0; i <= VFE_LINE_RDI2 && i < vfe->res->line_num; i++) {
		bool rup = value0 & VFE_0_IRQ_STATUS_0_line_n_REG_UPDATE(i);

		if (rup)
			a6l_stk_nrup[v]++;
		if (!a6l_stk_run[v][i])
			continue;
		if (rup && (a6l_stk & 1)) {
			/* stock msm_vfe47_process_reg_update: "Reg Update is pseudo SOF for RDI, so request every frame" */
			spin_lock(&vfe->output_lock);
			vfe_reg_update(vfe, i);
			spin_unlock(&vfe->output_lock);
			a6l_stk_nreq[v]++;
		} else if ((a6l_stk & 2) && (value1 & VFE_0_IRQ_STATUS_1_RDIn_SOF(i)) &&
			   !(vfe->reg_update & VFE_0_REG_UPDATE_line_n(i))) {
			spin_lock(&vfe->output_lock);
			vfe_reg_update(vfe, i);
			spin_unlock(&vfe->output_lock);
			a6l_stk_nsof[v]++;
		}
	}
}

static irqreturn_t vfe_isr(int irq, void *dev)
{
''')

sub1(V48, """		if (value0 & VFE_0_IRQ_STATUS_0_line_n_REG_UPDATE(i))
			vfe->isr_ops.reg_update(vfe, i);
""", """		if (value0 & VFE_0_IRQ_STATUS_0_line_n_REG_UPDATE(i))
			vfe->isr_ops.reg_update(vfe, i);
	a6l_stk_isr(vfe, value0, value1); /* camfix9b */
""")

# ---- gen1: start order, start/stop hooks, ping/pong re-program ----
sub1(G1, """		vfe->ops_gen1->wm_enable(vfe, output->wm_idx[0], 1);
		vfe->ops_gen1->bus_reload_wm(vfe, output->wm_idx[0]);
	} else {
""", """		vfe->ops_gen1->wm_enable(vfe, output->wm_idx[0], 1);
		if (!(a6l_stk & 8)) /* camfix9b bit3: stock reloads after the reg-update request (a6l_stk_start) */
			vfe->ops_gen1->bus_reload_wm(vfe, output->wm_idx[0]);
	} else {
""")
sub1(G1, """	ops->reg_update(vfe, line->id);

	if (a6l_dbg && line->id != VFE_LINE_PIX) {
""", """	ops->reg_update(vfe, line->id);
	if (line->id != VFE_LINE_PIX)
		a6l_stk_start(vfe, output->wm_idx[0], line->id); /* camfix9b */

	if (a6l_dbg && line->id != VFE_LINE_PIX) {
""")
sub1(G1, """	spin_lock_irqsave(&vfe->output_lock, flags);

	output->gen1.wait_sof = 1;
""", """	spin_lock_irqsave(&vfe->output_lock, flags);
	a6l_stk_stop(vfe, line->id); /* camfix9b: stop the per-frame reg-update chain first */

	output->gen1.wait_sof = 1;
""")
sub1(G1, "		output->a6l_slot_done[slot]++;\n",
"""		output->a6l_slot_done[slot]++;
		if (a6l_stk & 16) /* camfix9b bit4: stock re-programs the finished buffer address at buf done */
			a6l_stk_pp(vfe, wm, slot, (u32)output->a6l_fix[slot]);
""")
print("CAMFIX9B_PATCH_OK")
