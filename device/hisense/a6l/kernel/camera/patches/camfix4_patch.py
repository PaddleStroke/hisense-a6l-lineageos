#!/usr/bin/env python3
# camfix4 (25 Sep 2026 evening): applied AFTER camfix_patch.py, camfix2_patch.py and camfix3_patch.py.
#   usage: camfix4_patch.py <camss_dir>
# Input: attended run t25c (V74, camera4 = camfix3, a6l_wm=3): IMX576 bars streamed (sensor frame counter moving,
# RDI0 SOF every frame, 238 SOF in 5.5 s), stream-off safe, but NO write-master done at all (s0b8 never set),
# one bus error (s1 bit4, 0xC94 = 0x1 = WM0) on the 2nd SOF, exactly as in t25b (camfix2, which got 2 dones).
# Root cause (found by disassembling the Hisense stock kernel, firmware/extracted/stock-symbolized.elf):
#   msm_vfe47_update_ping_pong_addr() of the STOCK kernel (used by msm_vfe48) writes TWO registers per slot:
#     WR_PING_ADDR (0xA4 + 0x2C*wm) = addr, WR_PING_MAX_ADDR (0xA8 + 0x2C*wm) = (addr + buf_size) & ~0x1f
#     WR_PONG_ADDR (0xAC + 0x2C*wm) = addr, WR_PONG_MAX_ADDR (0xB0 + 0x2C*wm) = (addr + buf_size) & ~0x1f
#   (buf_size = word_per_line*8*scan_lines = frame bytes). Upstream camss-vfe-4-8.c never writes the MAX
#   registers: they stay 0 after the VFE reset (t25b/t25c dumps: 0x0a8 = 0x0b0 = 0), so every burst of the WM
#   is "above max" -> WM0 bus error (0xC94 bit0) and the frame never completes -> no ping-pong done.
#   (The public LineageOS/xiaomi msm-4.4 copy writes max = 0 there, which is why the earlier read missed it.)
# Second stock difference: msm_isp_axi_update_cgc_override(START) sets CGC override bit <wm> in VFE 0x3C for
#   every stream (msm_vfe47_axi_update_cgc_override, confirmed in the stock ELF); upstream 4.8 leaves it empty.
# Changes:
#  1) 4-8: wm_set_ping/pong_addr also write the MAX register = (addr + size) & ~0x1f (size per WM, set by gen1
#     from the line's sizeimage, fixed-buffer size in mode 4). Param a6l_wmmax (default 1; 0 = old behaviour).
#  2) 4-8: set_cgc_override implemented (0x3C bit wm), param a6l_cgc (default 1).
#  3) gen1: a6l_wm bit2 (=4) "simple fixed" mode: two driver-owned ping/pong buffers (2x frame, filled with
#     0xA5A5A5A5), programmed once, frame-based, period 1 / pattern 1 (every frame), no reg-update/addr change
#     on done; each done copies the finished slot into a queued user buffer from a workqueue; at stream-off
#     the fixed buffers are scanned: A6L_WM4_SCAN slot s done N written W first F end E frame S.
#     bit4 (=16): in mode 4 program max = frame size instead of the 2x buffer. bit3 (=8): on a bus error IRQ
#     reload the WMs flagged in 0xC94 (recovery experiment, any mode).
#  4) diagnostics: A6L_WM<n>_START line with 0x3C and the whole WM register block after enable.
import sys, re

def sub1(path, old, new):
    s = open(path).read()
    n = s.count(old)
    assert n == 1, f"{path}: anchor found {n}x: {old[:70]!r}"
    open(path, 'w').write(s.replace(old, new))
    print(f"PATCHED {path.split('/')[-1]}: {old.strip().splitlines()[0][:60]}")

cs = sys.argv[1]
V48 = f"{cs}/camss-vfe-4-8.c"
G1 = f"{cs}/camss-vfe-gen1.c"
H = f"{cs}/camss-vfe.h"

# ---- header ----------------------------------------------------------------------------------------------
sub1(H, "#include <linux/spinlock_types.h>\n", "#include <linux/spinlock_types.h>\n#include <linux/workqueue.h>\n")
sub1(H, "\tu32 a6l_done, a6l_user, a6l_scr, a6l_empty;\n",
     "\tu32 a6l_done, a6l_user, a6l_scr, a6l_empty;\n"
     "\t/* A6L camfix4: simple fixed ping/pong mode (a6l_wm bit2), a6l_mode == 2 */\n"
     "\tvoid *a6l_fix_cpu[2];\n\tdma_addr_t a6l_fix[2];\n\tsize_t a6l_fix_sz, a6l_frame_sz;\n"
     "\tu32 a6l_slot_done[2], a6l_copied, a6l_copy_seq;\n\tu64 a6l_copy_ts;\n"
     "\tint a6l_copy_slot, a6l_busy, a6l_stop, a6l_work_ok;\n"
     "\tstruct work_struct a6l_work;\n\tstruct vfe_device *a6l_vfe;\n")

# ---- 4-8: max address, CGC override, bus error recovery, size table -----------------------------------------
sub1(V48, "extern int a6l_wm; /* camss-vfe-gen1.c */\nint a6l_dbg = 1;\n",
     "extern int a6l_wm; /* camss-vfe-gen1.c */\n"
     "/* camfix4: stock msm_vfe47_update_ping_pong_addr writes WR_PING/PONG_MAX_ADDR = (addr + size) & ~0x1f */\n"
     "int a6l_wmmax = 1;\nmodule_param(a6l_wmmax, int, 0644);\n"
     "MODULE_PARM_DESC(a6l_wmmax, \"A6L: 1 = program WM ping/pong MAX address (stock), 0 = leave 0 (upstream)\");\n"
     "int a6l_cgc = 1;\nmodule_param(a6l_cgc, int, 0644);\n"
     "MODULE_PARM_DESC(a6l_cgc, \"A6L: 1 = set VFE 0x3C CGC override bit per active WM (stock), 0 = upstream (none)\");\n"
     "static u32 a6l_wm_sz[2][8];\n"
     "void a6l_wm_set_size(struct vfe_device *vfe, u8 wm, u32 size)\n{\n"
     "\tif (wm < 8)\n\t\ta6l_wm_sz[vfe->id & 1][wm] = size;\n}\n\n"
     "static u32 a6l_wm_max(struct vfe_device *vfe, u8 wm, u32 addr)\n{\n"
     "\tu64 end;\n\n"
     "\tif (!a6l_wmmax || !addr || wm >= 8 || !a6l_wm_sz[vfe->id & 1][wm])\n\t\treturn 0;\n"
     "\tend = (u64)addr + a6l_wm_sz[vfe->id & 1][wm];\n"
     "\tif (end > 0xffffffffULL)\n\t\tend = 0xffffffffULL;\n"
     "\treturn (u32)end & ~0x1fU;\n}\n\n"
     "static void vfe_bus_reload_wm(struct vfe_device *vfe, u8 wm);\n"
     "int a6l_dbg = 1;\n")

sub1(V48, "\t/* s1: bit0 camif error, bit7 violation, bits 9..15 image master bus overflow */\n",
     "\t/* camfix4 experiment (a6l_wm bit3): reload the WMs that reported a bus error */\n"
     "\tif ((s1 & BIT(4)) && (a6l_wm & 8)) {\n"
     "\t\tu32 be = readl_relaxed(vfe->base + 0xc94);\n\t\tunsigned int w;\n\n"
     "\t\tfor (w = 0; w < 7; w++)\n\t\t\tif (be & BIT(w))\n\t\t\t\tvfe_bus_reload_wm(vfe, w);\n\t}\n"
     "\t/* s1: bit0 camif error, bit7 violation, bits 9..15 image master bus overflow */\n")

sub1(V48, "static void vfe_wm_set_ping_addr(struct vfe_device *vfe, u8 wm, u32 addr)\n{\n\twritel_relaxed(addr,\n\t\t       vfe->base + VFE_0_BUS_IMAGE_MASTER_n_WR_PING_ADDR(wm));\n}\n",
     "static void vfe_wm_set_ping_addr(struct vfe_device *vfe, u8 wm, u32 addr)\n{\n\twritel_relaxed(addr,\n\t\t       vfe->base + VFE_0_BUS_IMAGE_MASTER_n_WR_PING_ADDR(wm));\n"
     "\t/* camfix4: WR_PING_MAX_ADDR (stock), 0 = every burst out of range -> bus error, no done */\n"
     "\twritel_relaxed(a6l_wm_max(vfe, wm, addr),\n\t\t       vfe->base + VFE_0_BUS_IMAGE_MASTER_n_WR_PING_ADDR(wm) + 0x4);\n}\n")
sub1(V48, "static void vfe_wm_set_pong_addr(struct vfe_device *vfe, u8 wm, u32 addr)\n{\n\twritel_relaxed(addr,\n\t\t       vfe->base + VFE_0_BUS_IMAGE_MASTER_n_WR_PONG_ADDR(wm));\n}\n",
     "static void vfe_wm_set_pong_addr(struct vfe_device *vfe, u8 wm, u32 addr)\n{\n\twritel_relaxed(addr,\n\t\t       vfe->base + VFE_0_BUS_IMAGE_MASTER_n_WR_PONG_ADDR(wm));\n"
     "\t/* camfix4: WR_PONG_MAX_ADDR (stock) */\n"
     "\twritel_relaxed(a6l_wm_max(vfe, wm, addr),\n\t\t       vfe->base + VFE_0_BUS_IMAGE_MASTER_n_WR_PONG_ADDR(wm) + 0x4);\n}\n")

sub1(V48, "static void vfe_set_cgc_override(struct vfe_device *vfe, u8 wm, u8 enable)\n{\n\t/* empty */\n}\n",
     "static void vfe_set_cgc_override(struct vfe_device *vfe, u8 wm, u8 enable)\n{\n"
     "\t/* camfix4: stock msm_vfe47_axi_update_cgc_override (called for every stream at START/STOP) */\n"
     "\tif (!a6l_cgc && enable)\n\t\treturn;\n"
     "\tif (enable)\n\t\tvfe_reg_set(vfe, 0x03c, BIT(wm));\n\telse\n\t\tvfe_reg_clr(vfe, 0x03c, BIT(wm));\n"
     "\twmb();\n}\n")

# ---- gen1: mode 4 (a6l_mode 2) ---------------------------------------------------------------------------
sub1(G1, "#include <linux/dma-mapping.h>\n#include <linux/moduleparam.h>\n",
     "#include <linux/dma-mapping.h>\n#include <linux/moduleparam.h>\n#include <linux/workqueue.h>\n"
     "#include <media/videobuf2-dma-sg.h>\n\n"
     "void a6l_wm_set_size(struct vfe_device *vfe, u8 wm, u32 size); /* camss-vfe-4-8.c */\n"
     "#define A6L_FILL 0xa5a5a5a5U\n")

sub1(G1, "static u32 a6l_slot_addr(struct vfe_output *output, int slot, unsigned int plane)\n{\n",
     "static u32 a6l_slot_addr(struct vfe_output *output, int slot, unsigned int plane)\n{\n"
     "\tif (output->a6l_mode == 2)\n\t\treturn (u32)output->a6l_fix[slot & 1];\n")

sub1(G1, "/* before vfe_get_output/vfe_enable_output: sleeping allocation, outside output_lock */\n",
     r'''/* camfix4 mode 4: copy the slot the WM just finished into the next queued user buffer (process context) */
static void a6l_fix_work(struct work_struct *work)
{
	struct vfe_output *output = container_of(work, struct vfe_output, a6l_work);
	struct vfe_device *vfe = output->a6l_vfe;
	struct camss_buffer *b = NULL;
	struct sg_table *sgt;
	unsigned long flags;
	void *dst = NULL;
	size_t n = 0;
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
		output->a6l_copied++;
		if (a6l_dbg && output->a6l_copied <= 4)
			dev_info(vfe->camss->dev, "A6L_WM4 copy#%u slot %d seq %u %zu bytes%s\n", output->a6l_copied,
				 slot, seq, n, dst ? "" : " (no vaddr: ERROR)");
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
				dev_err(vfe->camss->dev, "A6L_WM4 fixed buffer %u (%zu bytes) alloc failed\n", s, sz);
				if (s) {
					dma_free_attrs(vfe->camss->dev, sz, output->a6l_fix_cpu[0], output->a6l_fix[0], 0);
					output->a6l_fix_cpu[0] = NULL;
				}
				return -ENOMEM;
			}
		}
		output->a6l_fix_sz = sz;
	}
	for (s = 0; s < 2; s++)
		memset32(output->a6l_fix_cpu[s], A6L_FILL, output->a6l_fix_sz / 4);
	if (!output->a6l_work_ok) {
		INIT_WORK(&output->a6l_work, a6l_fix_work);
		output->a6l_work_ok = 1;
	}
	output->a6l_vfe = vfe;
	output->a6l_frame_sz = frame;
	output->a6l_slot_done[0] = output->a6l_slot_done[1] = 0;
	output->a6l_copied = 0;
	output->a6l_busy = 0;
	output->a6l_stop = 0;
	output->a6l_mode = 2;
	dev_info(vfe->camss->dev, "A6L_WM vfe%u fixed mode (a6l_wm %d): ping %pad pong %pad %zu bytes each, frame %zu\n",
		 vfe->id, a6l_wm, &output->a6l_fix[0], &output->a6l_fix[1], output->a6l_fix_sz, frame);
	return 0;
}

/* camfix4 mode 4 stream-off: stop the copies, then report how much of each fixed buffer the WM wrote */
static void a6l_fixed_stop(struct vfe_device *vfe, struct vfe_output *output)
{
	unsigned long flags;
	unsigned int s;

	spin_lock_irqsave(&vfe->output_lock, flags);
	output->a6l_stop = 1;
	spin_unlock_irqrestore(&vfe->output_lock, flags);
	if (output->a6l_work_ok)
		cancel_work_sync(&output->a6l_work);
	for (s = 0; s < 2; s++) {
		const u32 *p = output->a6l_fix_cpu[s];
		size_t nw = output->a6l_fix_sz / 4, i, first = nw, last = 0, cnt = 0;

		if (!p)
			continue;
		for (i = 0; i < nw; i++)
			if (p[i] != A6L_FILL) {
				if (first == nw)
					first = i;
				last = i;
				cnt++;
			}
		dev_info(vfe->camss->dev,
			 "A6L_WM4_SCAN slot %u done %u written_words %zu first 0x%zx end 0x%zx frame 0x%zx buf 0x%zx head %08x %08x %08x %08x\n",
			 s, output->a6l_slot_done[s], cnt, first == nw ? 0 : first * 4, cnt ? (last + 1) * 4 : 0,
			 output->a6l_frame_sz, output->a6l_fix_sz, p[0], p[1], p[2], p[3]);
	}
	dev_info(vfe->camss->dev, "A6L_WM4_STOP done %u copied %u\n", output->a6l_done, output->a6l_copied);
}

/* before vfe_get_output/vfe_enable_output: sleeping allocation, outside output_lock */
''')

sub1(G1, "\tif (!(a6l_wm & 1) || line->id == VFE_LINE_PIX)\n\t\treturn;\n",
     "\tif (line->id == VFE_LINE_PIX)\n\t\treturn;\n"
     "\tif (a6l_wm & 4) { /* camfix4: simple fixed ping/pong */\n"
     "\t\tfor (i = 0; i < pix->num_planes && i < ARRAY_SIZE(pix->plane_fmt); i++)\n"
     "\t\t\tsz = max_t(size_t, sz, pix->plane_fmt[i].sizeimage);\n"
     "\t\tif (sz && !a6l_prepare_fixed(vfe, output, sz))\n\t\t\treturn;\n"
     "\t\tsz = 0;\n\t}\n"
     "\tif (!(a6l_wm & 1))\n\t\treturn;\n")

# stop: fixed-mode scan before the register dump
sub1(G1, "\ta6l_vfe_dump(vfe, \"STOP\");\n\tvfe_disable_output(line);\n",
     "\tif (line->output.a6l_mode == 2)\n\t\ta6l_fixed_stop(vfe, &line->output);\n"
     "\ta6l_vfe_dump(vfe, \"STOP\");\n\tvfe_disable_output(line);\n")

# init addrs: mode 2 hands the user buffers back to the pending list (the fixed buffers are the ping/pong)
sub1(G1, "\tif (output->a6l_mode) {\n\t\tfor (i = 0; i < output->wm_num; i++) {\n\t\t\tvfe->ops_gen1->wm_set_ping_addr(vfe, output->wm_idx[i], a6l_slot_addr(output, 0, i));\n",
     "\tif (output->a6l_mode == 2) {\n"
     "\t\tfor (i = 0; i < 2; i++)\n\t\t\tif (output->buf[i]) {\n"
     "\t\t\t\tvfe_buf_add_pending(output, output->buf[i]);\n\t\t\t\toutput->buf[i] = NULL;\n\t\t\t}\n\t}\n"
     "\tif (output->a6l_mode) {\n\t\tfor (i = 0; i < output->wm_num; i++) {\n\t\t\tvfe->ops_gen1->wm_set_ping_addr(vfe, output->wm_idx[i], a6l_slot_addr(output, 0, i));\n")

# enable: per-WM size for the MAX address registers
sub1(G1, "\toutput->state = VFE_OUTPUT_IDLE;\n\n\toutput->buf[0] = vfe_buf_get_pending(output);\n",
     "\toutput->state = VFE_OUTPUT_IDLE;\n\n"
     "\t/* camfix4: size used for WR_PING/PONG_MAX_ADDR (RDI frame-based WMs only) */\n"
     "\tfor (i = 0; i < output->wm_num; i++)\n"
     "\t\ta6l_wm_set_size(vfe, output->wm_idx[i], line->id == VFE_LINE_PIX ? 0 :\n"
     "\t\t\t\t(output->a6l_mode == 2 && !(a6l_wm & 16)) ? (u32)output->a6l_fix_sz :\n"
     "\t\t\t\tline->video_out.active_fmt.fmt.pix_mp.plane_fmt[0].sizeimage);\n\n"
     "\toutput->buf[0] = vfe_buf_get_pending(output);\n")

# enable: register snapshot of the WM after everything is programmed
sub1(G1, "\tops->reg_update(vfe, line->id);\n\n\tspin_unlock_irqrestore(&vfe->output_lock, flags);\n\n\treturn 0;\n}\n\nstatic int vfe_get_output",
     "\tops->reg_update(vfe, line->id);\n\n"
     "\tif (a6l_dbg && line->id != VFE_LINE_PIX) {\n"
     "\t\tu32 b = 0xa0 + 0x2c * output->wm_idx[0];\n\n"
     "\t\tdev_info(vfe->camss->dev,\n"
     "\t\t\t \"A6L_WM%u_START line %d mode %d cgc 0x%08x wm %08x ping %08x max %08x pong %08x max %08x addrcfg %08x ub %08x size %08x bufcfg %08x pat %08x sub %08x\\n\",\n"
     "\t\t\t output->wm_idx[0], line->id, output->a6l_mode, readl_relaxed(vfe->base + 0x3c),\n"
     "\t\t\t readl_relaxed(vfe->base + b), readl_relaxed(vfe->base + b + 0x4), readl_relaxed(vfe->base + b + 0x8),\n"
     "\t\t\t readl_relaxed(vfe->base + b + 0xc), readl_relaxed(vfe->base + b + 0x10), readl_relaxed(vfe->base + b + 0x14),\n"
     "\t\t\t readl_relaxed(vfe->base + b + 0x18), readl_relaxed(vfe->base + b + 0x1c), readl_relaxed(vfe->base + b + 0x20),\n"
     "\t\t\t readl_relaxed(vfe->base + b + 0x24), readl_relaxed(vfe->base + b + 0x28));\n\t}\n\n"
     "\tspin_unlock_irqrestore(&vfe->output_lock, flags);\n\n\treturn 0;\n}\n\nstatic int vfe_get_output")

# wm done: mode 2 = count + hand the finished slot to the copy work; the addresses are never changed
sub1(G1, "\tif (output->a6l_mode) {\n\t\t/* status bit = slot the WM writes now; the other slot just completed (= stock pingpong_bit) */\n",
     "\tif (output->a6l_mode == 2) {\n"
     "\t\tint slot = !active_index;\n"
     "\t\tint q = 0;\n\n"
     "\t\toutput->a6l_done++;\n\t\toutput->a6l_slot_done[slot]++;\n\t\toutput->sequence++;\n"
     "\t\tif (!output->a6l_stop && !output->a6l_busy && !list_empty(&output->pending_bufs)) {\n"
     "\t\t\toutput->a6l_busy = 1;\n\t\t\toutput->a6l_copy_slot = slot;\n"
     "\t\t\toutput->a6l_copy_seq = output->sequence - 1;\n\t\t\toutput->a6l_copy_ts = ts;\n\t\t\tq = 1;\n\t\t}\n"
     "\t\tspin_unlock_irqrestore(&vfe->output_lock, flags);\n"
     "\t\tif (q)\n\t\t\tqueue_work(system_highpri_wq, &output->a6l_work);\n"
     "\t\tif (a6l_dbg && output->a6l_done <= 8)\n"
     "\t\t\tdev_info(vfe->camss->dev, \"A6L_WM%u done#%u slot %d (fixed)%s\\n\", wm, output->a6l_done, slot,\n"
     "\t\t\t\t q ? \" -> copy\" : \"\");\n"
     "\t\treturn;\n\t}\n\n"
     "\tif (output->a6l_mode) {\n\t\t/* status bit = slot the WM writes now; the other slot just completed (= stock pingpong_bit) */\n")

sub1(G1, 'MODULE_PARM_DESC(a6l_wm, "A6L RDI WM: bit0 stock-like every-frame + scratch buffer, bit1 stock frame-based BUFFER_CFG; 0 = upstream");',
     'MODULE_PARM_DESC(a6l_wm, "A6L RDI WM: bit0 stock-like every-frame + scratch buffer, bit1 stock frame-based BUFFER_CFG, '
     'bit2 simple fixed ping/pong (+copy, scan), bit3 reload WM on bus error, bit4 fixed mode max = frame size; 0 = upstream");')
print("CAMFIX4_PATCH_PASS")
