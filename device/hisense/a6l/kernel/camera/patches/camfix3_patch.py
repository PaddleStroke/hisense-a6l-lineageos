#!/usr/bin/env python3
# camfix3 (25 Sep 2026): applied AFTER camfix_patch.py and camfix2_patch.py to the same camss copies.
#   usage: camfix3_patch.py <camss_dir>
# Input: attended run t25b (V74, camera3): IMX576 bars streaming, RDI0 SOF ~every frame (IRQ_STATUS_1 bit29 = 370),
# but only 2 write-master (WM0) ping-pong done IRQs (s0b8:2) in ~9 s, 3 RDI0 reg-update acks (s0b5:3) and one
# s1b4 (VFE 4.8 = BUS ERROR summary, per-WM detail in 0xC94, ignored by stock for RDI WMs) right after frame 1.
# WM0 regs at stop: ping 0xfe800000 / pong 0xfe000000 (valid IOVAs), ADDR_CFG 0x7e = frame-based + framedrop
# period 31, FRAMEDROP_PATTERN 3, UB 0/682: i.e. upstream's "toggle period 30/31 + pattern + reg-update" trick.
# That trick assumes the HW restarts its framedrop counter on every reg update; on this VFE 4.8 it evidently
# does not (no WM done in frames 1..11 although pattern 3 was active, then 2 dones and never again).
# Stock (msm_isp47/48 + msm_isp_axi_util) never uses it: period 1 (reg 0), pattern 0x1 = write EVERY frame, and a
# scratch buffer is programmed into the free ping/pong slot when no user buffer is queued.
# 1) a6l_wm (default 3): bit0 = stock-like RDI write master: period 1 / pattern 1, ping/pong always valid
#    (user buffer or a per-line scratch buffer), buffers handed over from the WM-done IRQ only, no per-frame
#    reg-update; bit1 = stock frame-based WR_BUFFER_CFG (= WR_ADDR_CFG value, as msm_vfe47_axi_cfg_wm_reg).
#    a6l_wm=0 restores upstream behaviour (runtime-writable, takes effect at the next STREAMON).
# 2) Stream-off dumps made safe: the t25b phone reset happened in the ISPIF dump right after row 0x480, i.e. when
#    reading the VFE1 interface bank (0x4a0..) whose csi_vfe1/vfe1 clocks are OFF (VFE1 unused). ISPIF dump now
#    reads only the global block and the bank of the VFE in use, and only while ispif->power_count > 0; VFE dump
#    requires vfe->power_count > 0; the CSID stop register dump is dropped (IRQ count line kept).
# 3) Diagnostics: bus-error register 0xC94 on s1 bit4, VFE dump adds 0xc80..0xc9f, first 8 WM-done events and
#    A6L_WM<n>_STOP done/user/scratch/empty counters.
import sys, re

def sub1(path, old, new):
    s = open(path).read()
    n = s.count(old)
    assert n == 1, f"{path}: anchor found {n}x: {old[:70]!r}"
    open(path, 'w').write(s.replace(old, new))
    print(f"PATCHED {path.split('/')[-1]}: {old.strip().splitlines()[0][:60]}")

def add_after_includes(path, text):
    s = open(path).read()
    last = list(re.finditer(r'^#include .*$', s, re.M))[-1]
    s = s[:last.end()] + "\n" + text + s[last.end():]
    open(path, 'w').write(s)
    print(f"PATCHED {path.split('/')[-1]}: header block")

cs = sys.argv[1]
V48 = f"{cs}/camss-vfe-4-8.c"
G1 = f"{cs}/camss-vfe-gen1.c"

# ---- vfe_output: scratch buffer + counters ----------------------------------------------------------------
sub1(f"{cs}/camss-vfe.h", "\tint wait_reg_update;\n\tstruct completion sof;\n",
     "\tint wait_reg_update;\n"
     "\t/* A6L camfix3: stock-like RDI write master (every frame, scratch when no user buffer) */\n"
     "\tint a6l_mode;\n\tdma_addr_t a6l_scratch;\n\tvoid *a6l_scratch_cpu;\n\tsize_t a6l_scratch_sz;\n"
     "\tu32 a6l_done, a6l_user, a6l_scr, a6l_empty;\n"
     "\tstruct completion sof;\n")

# ---- gen1: param, helpers --------------------------------------------------------------------------------
add_after_includes(G1, r'''
#include <linux/dma-mapping.h>
#include <linux/moduleparam.h>

extern int a6l_dbg; /* camss-vfe-4-8.c */
int a6l_wm = 3;
module_param(a6l_wm, int, 0644);
MODULE_PARM_DESC(a6l_wm, "A6L RDI WM: bit0 stock-like every-frame + scratch buffer, bit1 stock frame-based BUFFER_CFG; 0 = upstream");

static u32 a6l_slot_addr(struct vfe_output *output, int slot, unsigned int plane)
{
	return output->buf[slot] ? output->buf[slot]->addr[plane] : (u32)output->a6l_scratch;
}

/* before vfe_get_output/vfe_enable_output: sleeping allocation, outside output_lock */
static void a6l_prepare_output(struct vfe_device *vfe, struct vfe_line *line)
{
	struct vfe_output *output = &line->output;
	struct v4l2_pix_format_mplane *pix = &line->video_out.active_fmt.fmt.pix_mp;
	size_t sz = 0;
	unsigned int i;

	output->a6l_mode = 0;
	output->a6l_done = output->a6l_user = output->a6l_scr = output->a6l_empty = 0;
	if (!(a6l_wm & 1) || line->id == VFE_LINE_PIX)
		return;
	for (i = 0; i < pix->num_planes && i < ARRAY_SIZE(pix->plane_fmt); i++)
		sz = max_t(size_t, sz, pix->plane_fmt[i].sizeimage);
	sz = PAGE_ALIGN(sz);
	if (!sz)
		return;
	if (output->a6l_scratch_cpu && output->a6l_scratch_sz < sz) {
		dma_free_attrs(vfe->camss->dev, output->a6l_scratch_sz, output->a6l_scratch_cpu,
			       output->a6l_scratch, DMA_ATTR_NO_KERNEL_MAPPING);
		output->a6l_scratch_cpu = NULL;
	}
	if (!output->a6l_scratch_cpu) {
		output->a6l_scratch_cpu = dma_alloc_attrs(vfe->camss->dev, sz, &output->a6l_scratch,
							  GFP_KERNEL, DMA_ATTR_NO_KERNEL_MAPPING);
		if (!output->a6l_scratch_cpu) {
			dev_err(vfe->camss->dev, "A6L_WM scratch alloc %zu failed: upstream WM mode\n", sz);
			return;
		}
		output->a6l_scratch_sz = sz;
	}
	output->a6l_mode = 1;
	dev_info(vfe->camss->dev, "A6L_WM vfe%u line %d stock-like mode, scratch %zu bytes iova %pad (a6l_wm %d)\n",
		 vfe->id, line->id, output->a6l_scratch_sz, &output->a6l_scratch, a6l_wm);
}
''')

# enable: prepare before get_output
sub1(G1, "\tvfe->stream_count++;\n\n\tmutex_unlock(&vfe->stream_lock);\n\n\tret = vfe_get_output(line);\n",
     "\tvfe->stream_count++;\n\n\tmutex_unlock(&vfe->stream_lock);\n\n\ta6l_prepare_output(vfe, line);\n\n\tret = vfe_get_output(line);\n")

# disable: stats line before the (existing) register dump
sub1(G1, "\ta6l_vfe_dump(vfe, \"STOP\");\n\tvfe_disable_output(line);\n",
     "\tif (line->output.a6l_mode)\n"
     "\t\tdev_info(vfe->camss->dev, \"A6L_WM%u_STOP line %d done %u user %u scratch %u empty %u seq %u\\n\",\n"
     "\t\t\t line->output.wm_idx[0], line->id, line->output.a6l_done, line->output.a6l_user,\n"
     "\t\t\t line->output.a6l_scr, line->output.a6l_empty, line->output.sequence);\n"
     "\ta6l_vfe_dump(vfe, \"STOP\");\n\tvfe_disable_output(line);\n")

# frame drop: period 1 (reg 0), pattern 1 -> every frame
sub1(G1, "\t/* We need to toggle update period to be valid on next frame */\n\toutput->drop_update_idx++;\n",
     "\tif (output->a6l_mode) {\n"
     "\t\tfor (i = 0; i < output->wm_num; i++) {\n"
     "\t\t\tvfe->ops_gen1->wm_set_framedrop_period(vfe, output->wm_idx[i], 0);\n"
     "\t\t\tvfe->ops_gen1->wm_set_framedrop_pattern(vfe, output->wm_idx[i], 1);\n"
     "\t\t}\n"
     "\t\tvfe->res->hw_ops->reg_update(vfe, container_of(output, struct vfe_line, output)->id);\n"
     "\t\treturn;\n\t}\n\n"
     "\t/* We need to toggle update period to be valid on next frame */\n\toutput->drop_update_idx++;\n")

# init addrs: ping/pong always valid
sub1(G1, "\toutput->gen1.active_buf = 0;\n\n\tfor (i = 0; i < output->wm_num; i++) {\n\t\tif (output->buf[0])\n\t\t\tping_addr = output->buf[0]->addr[i];\n",
     "\toutput->gen1.active_buf = 0;\n\n"
     "\tif (output->a6l_mode) {\n"
     "\t\tfor (i = 0; i < output->wm_num; i++) {\n"
     "\t\t\tvfe->ops_gen1->wm_set_ping_addr(vfe, output->wm_idx[i], a6l_slot_addr(output, 0, i));\n"
     "\t\t\tvfe->ops_gen1->wm_set_pong_addr(vfe, output->wm_idx[i], a6l_slot_addr(output, 1, i));\n"
     "\t\t\tif (sync)\n\t\t\t\tvfe->ops_gen1->bus_reload_wm(vfe, output->wm_idx[i]);\n"
     "\t\t}\n\t\treturn;\n\t}\n\n"
     "\tfor (i = 0; i < output->wm_num; i++) {\n\t\tif (output->buf[0])\n\t\t\tping_addr = output->buf[0]->addr[i];\n")

# wm done: stock-like hand-over
sub1(G1, "\toutput = &vfe->line[vfe->wm_output_map[wm]].output;\n\n\tif (output->gen1.active_buf == active_index && 0) {",
     "\toutput = &vfe->line[vfe->wm_output_map[wm]].output;\n\n"
     "\tif (output->a6l_mode) {\n"
     "\t\t/* status bit = slot the WM writes now; the other slot just completed (= stock pingpong_bit) */\n"
     "\t\tint slot = !active_index;\n"
     "\t\tstruct camss_buffer *nb;\n\n"
     "\t\toutput->gen1.active_buf = active_index;\n"
     "\t\tready_buf = output->buf[slot];\n"
     "\t\toutput->a6l_done++;\n"
     "\t\tif (ready_buf) {\n"
     "\t\t\tready_buf->vb.vb2_buf.timestamp = ts;\n"
     "\t\t\tready_buf->vb.sequence = output->sequence;\n"
     "\t\t\toutput->a6l_user++;\n"
     "\t\t} else {\n\t\t\toutput->a6l_scr++;\n\t\t}\n"
     "\t\toutput->sequence++;\n"
     "\t\tnb = vfe_buf_get_pending(output);\n"
     "\t\toutput->buf[slot] = nb;\n"
     "\t\tif (!nb)\n\t\t\toutput->a6l_empty++;\n"
     "\t\tfor (i = 0; i < output->wm_num; i++) {\n"
     "\t\t\tif (slot)\n"
     "\t\t\t\tvfe->ops_gen1->wm_set_pong_addr(vfe, output->wm_idx[i], a6l_slot_addr(output, slot, i));\n"
     "\t\t\telse\n"
     "\t\t\t\tvfe->ops_gen1->wm_set_ping_addr(vfe, output->wm_idx[i], a6l_slot_addr(output, slot, i));\n"
     "\t\t}\n"
     "\t\tspin_unlock_irqrestore(&vfe->output_lock, flags);\n"
     "\t\tif (a6l_dbg && output->a6l_done <= 8)\n"
     "\t\t\tdev_info(vfe->camss->dev, \"A6L_WM%u done#%u slot %d %s next %s\\n\", wm, output->a6l_done,\n"
     "\t\t\t\t slot, ready_buf ? \"user\" : \"scratch\", nb ? \"user\" : \"scratch\");\n"
     "\t\tif (ready_buf)\n"
     "\t\t\tvb2_buffer_done(&ready_buf->vb.vb2_buf, VB2_BUF_STATE_DONE);\n"
     "\t\treturn;\n\t}\n\n"
     "\tif (output->gen1.active_buf == active_index && 0) {")

# queue: only pending in a6l mode (the WM-done IRQ hands buffers to the HW, race-free)
sub1(G1, "\tspin_lock_irqsave(&vfe->output_lock, flags);\n\n\tvfe_buf_update_wm_on_new(vfe, output, buf, line);\n",
     "\tspin_lock_irqsave(&vfe->output_lock, flags);\n\n"
     "\tif (output->a6l_mode && output->state != VFE_OUTPUT_OFF) {\n"
     "\t\tvfe_buf_add_pending(output, buf);\n"
     "\t\tspin_unlock_irqrestore(&vfe->output_lock, flags);\n\t\treturn 0;\n\t}\n\n"
     "\tvfe_buf_update_wm_on_new(vfe, output, buf, line);\n")

# ---- vfe 4.8: stock frame-based BUFFER_CFG, bus error, safe dump ------------------------------------------
sub1(V48, "\tif (enable)\n\t\tvfe_reg_set(vfe, VFE_0_BUS_IMAGE_MASTER_n_WR_ADDR_CFG(wm),\n\t\t\t    1 << VFE_0_BUS_IMAGE_MASTER_n_WR_ADDR_CFG_FRM_BASED_SHIFT);\n\telse",
     "\tif (enable) {\n\t\tvfe_reg_set(vfe, VFE_0_BUS_IMAGE_MASTER_n_WR_ADDR_CFG(wm),\n\t\t\t    1 << VFE_0_BUS_IMAGE_MASTER_n_WR_ADDR_CFG_FRM_BASED_SHIFT);\n"
     "\t\t/* A6L: stock msm_vfe47_axi_cfg_wm_reg writes the ADDR_CFG value into BUFFER_CFG for frame-based WMs */\n"
     "\t\tif (a6l_wm & 2)\n"
     "\t\t\twritel_relaxed(readl_relaxed(vfe->base + VFE_0_BUS_IMAGE_MASTER_n_WR_ADDR_CFG(wm)),\n"
     "\t\t\t\t       vfe->base + VFE_0_BUS_IMAGE_MASTER_n_WR_BUFFER_CFG(wm));\n"
     "\t} else")
sub1(V48, "int a6l_dbg = 1;\n", "extern int a6l_wm; /* camss-vfe-gen1.c */\nint a6l_dbg = 1;\n")
sub1(V48, "\t/* s1: bit0 camif error, bit7 violation, bits 9..15 image master bus overflow */\n",
     "\t/* VFE 4.8 s1 bit4 = bus error summary, per-WM bits in 0xC94 (stock ignores it for RDI WMs) */\n"
     "\tif ((s1 & BIT(4)) && a6l_vfe_bits[v][36] <= 4)\n"
     "\t\tdev_info(vfe->camss->dev, \"A6L_VFE%u BUSERR#%u 0xc94 0x%08x (irq#%u)\\n\", vfe->id,\n"
     "\t\t\t a6l_vfe_bits[v][36], readl_relaxed(vfe->base + 0xc94), n);\n"
     "\t/* s1: bit0 camif error, bit7 violation, bits 9..15 image master bus overflow */\n")
sub1(V48, "\tstatic const u16 rng[][2] = { { 0x000, 0x160 }, { 0x300, 0x360 }, { 0x400, 0x4c0 } };\n",
     "\tstatic const u16 rng[][2] = { { 0x000, 0x160 }, { 0x300, 0x360 }, { 0x400, 0x4c0 }, { 0xc80, 0xca0 } };\n")
sub1(V48, "\tif (!a6l_dbg)\n\t\treturn;\n\tbuf[0] = 0;\n",
     "\tif (!a6l_dbg)\n\t\treturn;\n"
     "\tif (vfe->power_count <= 0) { /* A6L camfix3: never touch an unclocked block */\n"
     "\t\tdev_info(vfe->camss->dev, \"A6L_VFE%u_%s skipped (power_count %d)\\n\", vfe->id, tag, vfe->power_count);\n"
     "\t\treturn;\n\t}\n\tbuf[0] = 0;\n")

# ---- ISPIF: only global block + bank of the VFE in use, only while powered --------------------------------
sub1(f"{cs}/camss-ispif.c", "static void a6l_ispif_dump(struct ispif_device *ispif, const char *tag)\n{\n\tu32 o, w[8], nz;\n\tunsigned int i;\n\n\tif (!a6l_dbg)\n\t\treturn;\n\tdev_info(ispif->camss->dev, \"A6L_ISPIF_%s irqs %u\\n\", tag, a6l_ispif_nirq);\n\tfor (o = 0; o < 0x500; o += 32) {\n",
     "static void a6l_ispif_dump(struct ispif_device *ispif, const char *tag, u8 vfe)\n{\n\tu32 o, w[8], nz;\n\tunsigned int i;\n"
     "\t/* camfix3: the VFE1 bank (0x400..) needs csi_vfe1/vfe1 clocks that are off when VFE1 is unused;\n"
     "\t * reading it reset the phone in run t25b. Dump only the global block and the bank of our VFE. */\n"
     "\tu32 lo = 0x200 + 0x200 * (vfe & 1);\n\n"
     "\tif (!a6l_dbg)\n\t\treturn;\n"
     "\tdev_info(ispif->camss->dev, \"A6L_ISPIF_%s irqs %u vfe%u power %d\\n\", tag, a6l_ispif_nirq, vfe, ispif->power_count);\n"
     "\tif (ispif->power_count <= 0)\n\t\treturn;\n"
     "\tfor (o = 0; o < lo + 0x100; o += 32) {\n"
     "\t\tif (o == 0x80)\n\t\t\to = lo;\n")
sub1(f"{cs}/camss-ispif.c", "\t\ta6l_ispif_dump(ispif, \"STOP\");\n", "\t\ta6l_ispif_dump(ispif, \"STOP\", vfe);\n")

# ---- CSID: no register reads at stream-off, just the IRQ count --------------------------------------------
sub1(f"{cs}/camss-csid-4-7.c", "\t\ta6l_csid_dump(csid, \"STOP\");\n",
     "\t\tif (a6l_dbg) { /* camfix3: no CSID register dump at stream-off */\n"
     "\t\t\tdev_info(csid->camss->dev, \"A6L_CSID%u_STOP irqs %u\\n\", csid->id, a6l_csid_nirq[csid->id & 3]);\n"
     "\t\t\ta6l_csid_nirq[csid->id & 3] = 0;\n\t\t}\n")
print("CAMFIX3_PATCH_PASS")
