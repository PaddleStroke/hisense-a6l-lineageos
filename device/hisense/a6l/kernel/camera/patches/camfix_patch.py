#!/usr/bin/env python3
# camfix (24 Sep 2026): source edits for mainline 7.2.3 qcom-camss (sdm660) and hi846, applied to COPIES
# of the kernel sources in an external-module build dir.  usage: camfix_patch.py <camss_dir> <hi846.c>
# Every edit asserts that its anchor exists exactly once, so a changed upstream file fails loudly.
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
    print(f"PATCHED {path.split('/')[-1]}: params")

cs, hi = sys.argv[1], sys.argv[2]

# 1) ROOT CAUSE of "VFE sof timeout": CAMSS_660 was dropped from the ISPIF allocation list, so on sdm660 the
#    driver links CSID straight to the VFE (the newer-SoC topology). The sdm660 hardware routes CSID->VFE only
#    through ISPIF (ispif_res_660 exists and camss-ispif.c handles CAMSS_660), so the VFE never sees SOF.
sub1(f"{cs}/camss.c",
     "\t    camss->res->version == CAMSS_8x96) {\n\t\tcamss->ispif = devm_kcalloc",
     "\t    camss->res->version == CAMSS_8x96 ||\n\t    camss->res->version == CAMSS_660) {\n\t\tcamss->ispif = devm_kcalloc")

# 2) Same omission for the CSIPHY->CSID clock mux (0xca00120/124/128 "csiphyN_clk_mux"): without it the
#    CSIPHY output is never steered to the selected CSID.
sub1(f"{cs}/camss-csiphy.c",
     "\t    camss->res->version == CAMSS_8x96) {\n\t\tcsiphy->base_clk_mux =",
     "\t    camss->res->version == CAMSS_8x96 ||\n\t    camss->res->version == CAMSS_660) {\n\t\tcsiphy->base_clk_mux =")

# 3) "Failed to power up pipeline: -22" (S5K3T1 on CSID2): csid_set_power() powers "its" VFE via
#    vfe_parent_dev_ops_get(id) which returns -EINVAL for id >= vfe_num (sdm660: 4 CSIDs, 2 VFEs).
#    On ISPIF SoCs the CSID has no VFE parent: treat it as a no-op (put() already is).
sub1(f"{cs}/camss.c",
     "static int vfe_parent_dev_ops_get(struct camss *camss, int id)\n{\n\tint ret = -EINVAL;",
     "static int vfe_parent_dev_ops_get(struct camss *camss, int id)\n{\n\t/* A6L: CSID2/3 on ISPIF SoCs have no same-index VFE (they reach VFEs through ISPIF) */\n\tint ret = camss->ispif ? 0 : -EINVAL;")

# 4) Stock-like PHY timer / CSID clocks (stock DT: csiphy timer 269.33/310 MHz, csi_src 310 MHz). Mainline picks
#    the lowest level >= link_freq/4 (100 MHz), giving settle_cnt 3 at 255 MHz. Param default on, 0 = upstream.
add_after_includes(f"{cs}/camss-csiphy.c",
    "\nstatic bool a6l_phy_fast = true;\nmodule_param(a6l_phy_fast, bool, 0644);\n"
    "MODULE_PARM_DESC(a6l_phy_fast, \"A6L: CSIPHY timer at the highest level (stock-like, 269.33 MHz)\");\n")
sub1(f"{cs}/camss-csiphy.c",
     "\t\t\tu64 min_rate = link_freq / 4;\n\t\t\tlong round_rate;\n\n\t\t\tcamss_add_clock_margin(&min_rate);\n",
     "\t\t\tu64 min_rate = link_freq / 4;\n\t\t\tlong round_rate;\n\n\t\t\tcamss_add_clock_margin(&min_rate);\n"
     "\t\t\tif (a6l_phy_fast && min_rate && min_rate < 200000001ULL)\n\t\t\t\tmin_rate = 200000001ULL;\n")
add_after_includes(f"{cs}/camss-csid.c",
    "\nstatic bool a6l_csid_fast = true;\nmodule_param(a6l_csid_fast, bool, 0644);\n"
    "MODULE_PARM_DESC(a6l_csid_fast, \"A6L: CSID core clock >= 310 MHz (stock csi_src rate)\");\n")
sub1(f"{cs}/camss-csid.c",
     "\t\t\tu64 min_rate = link_freq / 4;\n\t\t\tlong rate;\n\n\t\t\tcamss_add_clock_margin(&min_rate);\n",
     "\t\t\tu64 min_rate = link_freq / 4;\n\t\t\tlong rate;\n\n\t\t\tcamss_add_clock_margin(&min_rate);\n"
     "\t\t\tif (a6l_csid_fast && min_rate && min_rate < 200000001ULL)\n\t\t\t\tmin_rate = 200000001ULL;\n")
sub1(f"{cs}/camss-csid.c",
     "\t\t\tret = clk_set_rate(clock->clk, rate);\n\t\t\tif (ret < 0) {\n\t\t\t\tdev_err(dev, \"clk set rate failed: %d\\n\", ret);\n\t\t\t\treturn ret;\n\t\t\t}\n\t\t} else if (clock->nfreqs) {",
     "\t\t\tret = clk_set_rate(clock->clk, rate);\n\t\t\tif (ret < 0) {\n\t\t\t\tdev_err(dev, \"clk set rate failed: %d\\n\", ret);\n\t\t\t\treturn ret;\n\t\t\t}\n"
     "\t\t\tdev_info(dev, \"A6L_CSID%u %s rate %ld (link_freq %lld lanes %u)\\n\", csid->id, clock->name, rate, (long long)link_freq, csid->phy.lane_cnt);\n"
     "\t\t} else if (clock->nfreqs) {")

# 5) Debug: log the PHY programming for every stream-on.
sub1(f"{cs}/camss-csiphy-3ph-1-0.c",
     "\tsettle_cnt = csiphy_settle_cnt_calc(link_freq, csiphy->timer_clk_rate);\n",
     "\tsettle_cnt = csiphy_settle_cnt_calc(link_freq, csiphy->timer_clk_rate);\n"
     "\tdev_info(csiphy->camss->dev, \"A6L_CSIPHY%d link_freq %lld timer %u settle_cnt %u lanes %u mask 0x%x csid %u\\n\",\n"
     "\t\t csiphy->id, (long long)link_freq, csiphy->timer_clk_rate, settle_cnt, c->num_data, lane_mask, cfg->csid_id);\n")

# 6) hi846: set_format() checks the lane support of the CURRENT mode before choosing the new one, and the
#    default mode (640x480) has no 4-lane table -> every S_FMT fails with "not supported for 4 lanes" on a
#    4-lane board. Default to 1632x1224 on 4 lanes and snap requests to a 4-lane-capable mode.
sub1(hi, "\thi846->cur_mode = &supported_modes[0];\n",
         "\thi846->cur_mode = &supported_modes[hi846->nr_lanes == 4 ? 2 : 0];\n")
sub1(hi, "\tif (hi846->nr_lanes == 2) {\n\t\tif (!hi846->cur_mode->reg_list_2lane.num_of_regs) {",
         "\tif (hi846->nr_lanes == 4) {\n"
         "\t\tconst struct hi846_mode *nm =\n"
         "\t\t\tv4l2_find_nearest_size(supported_modes, ARRAY_SIZE(supported_modes),\n"
         "\t\t\t\t\t       width, height, mf->width, mf->height);\n\n"
         "\t\tif (!nm->reg_list_4lane.num_of_regs)\n\t\t\tnm = &supported_modes[2];\n"
         "\t\tmf->width = nm->width;\n\t\tmf->height = nm->height;\n\t}\n\n"
         "\tif (hi846->nr_lanes == 2) {\n\t\tif (!hi846->cur_mode->reg_list_2lane.num_of_regs) {")
print("CAMFIX_PATCH_PASS")
