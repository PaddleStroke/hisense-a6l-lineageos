#!/usr/bin/env python3
# camfix5 (27 Sep 2026): applied AFTER camfix_patch.py, camfix2_patch.py, camfix3_patch.py and camfix4_patch.py.
#   usage: camfix5_patch.py <camss_dir>
# Input: attended run t27 (V75-usb recovery, bundle camera5 = camfix4, MODE=wmloop, kmsg streamed to the laptop:
# ~/A6L-usb-20260915/v75/logs/kmsg-t27-093736.txt, copy in .relay/outbox/cam27/).
#   - Every mode (wm4/wm3/wm0, MAX on/off, CGC on/off): RDI0 SOF on every frame, WM0 bus error (s1 bit4,
#     0xC94 = 0x1) on the 2nd SOF, i.e. inside the FIRST frame; at most ONE done per capture, 2-3.6 s late.
#   - The wm4 scan of the driver-owned 0xA5-filled slots shows how far the WM ever got in a frame:
#     end 0x51ee0 / 0x4aaa0 / 0x2ffc0 / 0x417c0 / 0x11860 = 5..93 lines of 3600 B out of 2156, never more.
#     MAX was far away (fdece000 = +15.5 MB) or 0, so it is not the max-address check of camfix4.
#   - VFE core clock: "A6L_VFE0 vfe0 rate 120000000 (min 60440427)". Upstream sizes the RDI clock as
#     pixel_rate * bpp / 64 (64 bit per clock) and picks the LOWEST level, 120 MHz. The stock DT
#     (firmware/extracted/device-trees/stock-00.dts, qcom,vfe0@ca10000 qcom,clock-rates) only has 404 / 480 /
#     576 MHz for vfe_clk_src, and msm_isp sets the VFE clock >= the sensor op_pixel_clk (1 pixel per clock),
#     so the stock VFE never runs below 404 MHz.
# Root-cause hypothesis: the RDI write path cannot drain the unified buffer at 120 MHz. The input (2880 x
#   RAW10 = 3600 B per line, ~30.7 fps) is ~250 MB/s during the frame. If the drain is ~2 B/clk * 120 MHz =
#   240 MB/s, the UB (682 words) fills by ~120-150 B per line. It overflows after tens of lines, which matches
#   the 55..93 lines seen. On VFE 4.8 the overflow is reported as the per-WM bus error (0xC94, s1 bit4; stock
#   msm_vfe47_get_overflow_mask = 0x09fffe7e includes bit4, and stock handles it as an overflow -> halt/recovery).
#   After that the frame never reaches EOF cleanly, so there is no ping-pong done, which gives "TIMEOUT waiting
#   for frame 1". (When a done does slip through, the copied "frame 0" is mostly fill pattern.)
# Changes:
#  1) camss-vfe.c: a6l_vfe_min default 404000000 (= stock lowest vfe_clk_src level). 0 = upstream formula.
#     Also applied in vfe_check_clock_rates, so a second pipeline does not accept a slower clock.
#  2) camss-vfe-4-8.c: per-frame counters (SOF, WM done, bus error, first bus-error SOF index) and a summary line at
#     stream-off: A6L_VFE<n>_FRAMES sof S done D buserr B first_buserr_sof F logged L.
#  3) camss-vfe-4-8.c: fallback diagnostic mode a6l_fdump=N: for the first N SOF / done / bus-error events it logs
#     A6L_VFE<n>_F ev sof#/t_us/s0/s1/pp(0x338)/buserr(0xC94)/cgc(0x3C) and the whole WM block of a6l_fdump_wm
#     (0xA0+0x2C*wm: cfg ping pmax pong qmax addrcfg ub imgsize bufcfg pattern subsample). Default 0 = off.
#  4) camss-vfe-4-8.c: a6l_ub=<depth>: UB depth for WM0 (offset 0) instead of 2047/3 = 682 (experiment; only safe
#     while WM0 is the only active WM, which holds for one RDI stream). Default 0 = upstream.
import sys

def sub1(path, old, new):
    s = open(path).read()
    n = s.count(old)
    assert n == 1, f"{path}: anchor found {n}x: {old[:70]!r}"
    open(path, 'w').write(s.replace(old, new))
    print(f"PATCHED {path.split('/')[-1]}: {old.strip().splitlines()[0][:60]}")

cs = sys.argv[1]
V = f"{cs}/camss-vfe.c"
V48 = f"{cs}/camss-vfe-4-8.c"

# ---- 1) VFE clock floor = stock ------------------------------------------------------------------------------
sub1(V, 'static uint a6l_vfe_min;\nmodule_param(a6l_vfe_min, uint, 0644);\n'
        'MODULE_PARM_DESC(a6l_vfe_min, "A6L: minimum VFE core clock in Hz (0 = upstream pixel-rate based)");\n',
     '/* camfix5: stock sdm660 never clocks the VFE below 404 MHz (DT vfe_clk_src levels 404/480/576 MHz); upstream\n'
     ' * picks 120 MHz for IMX576 full-res RDI and the WM0 unified buffer overflows (bus error 0xC94) mid-frame. */\n'
     'static uint a6l_vfe_min = 404000000;\nmodule_param(a6l_vfe_min, uint, 0644);\n'
     'MODULE_PARM_DESC(a6l_vfe_min, "A6L: minimum VFE core clock in Hz (default 404000000 = stock; 0 = upstream pixel-rate based)");\n')

sub1(V, "\t\t\tcamss_add_clock_margin(&min_rate);\n\n\t\t\trate = clk_get_rate(clock->clk);\n",
     "\t\t\tcamss_add_clock_margin(&min_rate);\n"
     "\t\t\tif (a6l_vfe_min && min_rate < a6l_vfe_min)\n\t\t\t\tmin_rate = a6l_vfe_min;\n\n"
     "\t\t\trate = clk_get_rate(clock->clk);\n")

# ---- 2+3) per-frame counters and per-frame register dump ------------------------------------------------------
sub1(V48, "#include <linux/moduleparam.h>\nextern int a6l_wm; /* camss-vfe-gen1.c */\n",
     "#include <linux/moduleparam.h>\n#include <linux/ktime.h>\nextern int a6l_wm; /* camss-vfe-gen1.c */\n"
     "/* camfix5: fallback diagnostic = per-event (RDI0 SOF, WM done, bus error) VFE register lines */\n"
     "static uint a6l_fdump;\nmodule_param(a6l_fdump, uint, 0644);\n"
     "MODULE_PARM_DESC(a6l_fdump, \"A6L: log VFE regs for the first N SOF/done/bus-error events of each stream (0 = off)\");\n"
     "static uint a6l_fdump_wm;\nmodule_param(a6l_fdump_wm, uint, 0644);\n"
     "MODULE_PARM_DESC(a6l_fdump_wm, \"A6L: write master whose register block a6l_fdump prints (default 0)\");\n"
     "static uint a6l_ub;\nmodule_param(a6l_ub, uint, 0644);\n"
     "MODULE_PARM_DESC(a6l_ub, \"A6L: UB depth for WM0 at offset 0 (0 = upstream 1/3 split; single RDI stream only)\");\n"
     "static u32 a6l_f_sof[2], a6l_f_done[2], a6l_f_be[2], a6l_f_be_first[2], a6l_f_logged[2];\n"
     "static ktime_t a6l_f_t0[2];\n")

sub1(V48, "\tif (n <= 12 || (a6l_dbg > 1 && !(n % 64)))\n",
     "\t/* camfix5: per-frame accounting (RDI0 SOF = s1 bit29, WM ping-pong done = s0 bits 8..14, bus error = s1 bit4) */\n"
     "\tif (n == 1 || !a6l_f_t0[v])\n\t\ta6l_f_t0[v] = ktime_get();\n"
     "\tif (s1 & BIT(29))\n\t\ta6l_f_sof[v]++;\n"
     "\tif (s0 & GENMASK(14, 8))\n\t\ta6l_f_done[v]++;\n"
     "\tif (s1 & BIT(4)) {\n\t\tif (!a6l_f_be[v])\n\t\t\ta6l_f_be_first[v] = a6l_f_sof[v];\n\t\ta6l_f_be[v]++;\n\t}\n"
     "\tif (a6l_fdump && a6l_f_logged[v] < a6l_fdump && ((s1 & (BIT(29) | BIT(4))) || (s0 & GENMASK(14, 8)))) {\n"
     "\t\tu32 b = 0xa0 + 0x2c * (a6l_fdump_wm & 7);\n\n"
     "\t\ta6l_f_logged[v]++;\n"
     "\t\tdev_info(vfe->camss->dev,\n"
     "\t\t\t \"A6L_VFE%u_F %s%s%s sof#%u t %lldus s0 %08x s1 %08x pp %08x be %08x cgc %08x wm%u %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x\\n\",\n"
     "\t\t\t vfe->id, (s1 & BIT(29)) ? \"SOF\" : \"\", (s0 & GENMASK(14, 8)) ? \"DONE\" : \"\", (s1 & BIT(4)) ? \"BUSERR\" : \"\",\n"
     "\t\t\t a6l_f_sof[v], ktime_us_delta(ktime_get(), a6l_f_t0[v]), s0, s1,\n"
     "\t\t\t readl_relaxed(vfe->base + 0x338), readl_relaxed(vfe->base + 0xc94), readl_relaxed(vfe->base + 0x3c),\n"
     "\t\t\t a6l_fdump_wm & 7, readl_relaxed(vfe->base + b), readl_relaxed(vfe->base + b + 0x4),\n"
     "\t\t\t readl_relaxed(vfe->base + b + 0x8), readl_relaxed(vfe->base + b + 0xc), readl_relaxed(vfe->base + b + 0x10),\n"
     "\t\t\t readl_relaxed(vfe->base + b + 0x14), readl_relaxed(vfe->base + b + 0x18), readl_relaxed(vfe->base + b + 0x1c),\n"
     "\t\t\t readl_relaxed(vfe->base + b + 0x20), readl_relaxed(vfe->base + b + 0x24), readl_relaxed(vfe->base + b + 0x28));\n"
     "\t}\n"
     "\tif (n <= 12 || (a6l_dbg > 1 && !(n % 64)))\n")

sub1(V48, "\tmemset(a6l_vfe_bits[v], 0, sizeof(a6l_vfe_bits[v]));\n\ta6l_vfe_nirq[v] = 0;\n",
     "\tdev_info(vfe->camss->dev, \"A6L_VFE%u_FRAMES %s sof %u done %u buserr %u first_buserr_sof %u logged %u\\n\",\n"
     "\t\t vfe->id, tag, a6l_f_sof[v], a6l_f_done[v], a6l_f_be[v], a6l_f_be[v] ? a6l_f_be_first[v] : 0, a6l_f_logged[v]);\n"
     "\ta6l_f_sof[v] = 0;\n\ta6l_f_done[v] = 0;\n\ta6l_f_be[v] = 0;\n\ta6l_f_be_first[v] = 0;\n\ta6l_f_logged[v] = 0;\n"
     "\ta6l_f_t0[v] = 0;\n"
     "\tmemset(a6l_vfe_bits[v], 0, sizeof(a6l_vfe_bits[v]));\n\ta6l_vfe_nirq[v] = 0;\n")

# ---- 4) UB depth experiment ---------------------------------------------------------------------------------
sub1(V48, "\treg = (offset << VFE_0_BUS_IMAGE_MASTER_n_WR_UB_CFG_OFFSET_SHIFT) |\n\t      depth;\n",
     "\t/* camfix5 experiment: give WM0 a deeper UB (single RDI stream only) */\n"
     "\tif (a6l_ub && wm == 0) {\n\t\toffset = 0;\n"
     "\t\tdepth = min_t(u32, a6l_ub, vfe->id ? MSM_VFE_VFE1_UB_SIZE : MSM_VFE_VFE0_UB_SIZE);\n\t}\n"
     "\treg = (offset << VFE_0_BUS_IMAGE_MASTER_n_WR_UB_CFG_OFFSET_SHIFT) |\n\t      depth;\n")
print("CAMFIX5_PATCH_PASS")
