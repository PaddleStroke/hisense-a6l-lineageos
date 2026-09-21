#!/system/bin/sh
# A6L: dump the MMSS clock state that matters for the display. Read-only. Usage: mmcc-diag.sh <tag>
# NOTE: the helper must not be called r: mksh has a builtin alias r='fc -e -'
M=${A6L_MMIO:-/tmp/a6l_mmio}; B=0x0c8c
rd() { printf '%-22s ' "$1"; $M r $2 ${3:-1} | tr '\n' ' '; echo; }
echo "=== mmcc-diag $1 $(cat /proc/uptime)"
rd mmpll0_mode..status  0c8cc000 10
rd mmpll5_mode..status  0c8c00a0 10
rd mmpll_vote_apcs      0c8c01e0 1
rd mdp_rcg_cmd_cfg      0c8c2040 2
rd pclk0_rcg_cmd..d     0c8c2000 5
rd byte0_rcg_cmd_cfg    0c8c2120 2
rd esc0_rcg_cmd_cfg     0c8c2160 2
rd vsync_rcg_cmd_cfg    0c8c2080 2
rd ahb_rcg_cmd_cfg      0c8c5000 2
rd axi_rcg_cmd_cfg      0c8cd000 2
rd mdss_gdscr           0c8c2304 1
rd mdss_ahb,hdmi,axi    0c8c2308 3
rd mdss_pclk0,1,mdp     0c8c2314 3
rd mdss_vsync           0c8c2328 1
rd mdss_byte0,1,esc0,1  0c8c233c 4
rd mdss_byte0_intf      0c8c2374 1
rd mnoc_ahb             0c8c5024 1
rd misc_ahb             0c8c0328 1
rd bimc_smmu_ahb,axi    0c8ce004 2
rd bimc_smmu_gdscr      0c8ce020 1
grep -E "mmpll5|mdp_clk_src|pclk0_clk_src|byte0_clk_src|dsi0pll |mmss_gpll0" /sys/kernel/debug/clk/clk_summary | cut -c1-110
