// SPDX-License-Identifier: GPL-2.0-only
/*
 * A6L bring-up experiment: hold RPM-SMD clocks enabled from a module parameter (no DT change needed).
 * 22 Sep 2026: stock keeps LN_BB_CLK1 (19.2 MHz PMIC buffer, "ln_bb_clk1_ao en=1") running while mainline turns
 * every unused RPM clock off; the e-ink bridge (TC358767 @0x0f, SYS_PLLPARAM = 19.2 MHz REFCLK) never ACKs on
 * our kernel. insmod a6l-clk-hold.ko ids=80 (RPM_SMD_LN_BB_CLK1); rmmod releases the clocks again.
 */
#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/module.h>
#include <linux/of.h>

static unsigned int ids[8];
static int nids;
module_param_array(ids, uint, &nids, 0444);
static struct clk *held[8];

static int __init a6l_clk_hold_init(void)
{
	struct device_node *np = of_find_compatible_node(NULL, NULL, "qcom,rpmcc-sdm660");
	int i, ret;

	if (!np)
		return -ENODEV;
	for (i = 0; i < nids; i++) {
		struct of_phandle_args a = { .np = np, .args_count = 1, .args = { ids[i] } };

		held[i] = of_clk_get_from_provider(&a);
		if (IS_ERR(held[i])) {
			pr_err("a6l-clk-hold: id %u: %ld\n", ids[i], PTR_ERR(held[i]));
			held[i] = NULL;
			continue;
		}
		ret = clk_prepare_enable(held[i]);
		pr_info("a6l-clk-hold: id %u enable=%d rate=%lu\n", ids[i], ret, clk_get_rate(held[i]));
		if (ret) {
			clk_put(held[i]);
			held[i] = NULL;
		}
	}
	of_node_put(np);
	return 0;
}

static void __exit a6l_clk_hold_exit(void)
{
	int i;

	for (i = 0; i < nids; i++)
		if (held[i]) {
			clk_disable_unprepare(held[i]);
			clk_put(held[i]);
		}
}
module_init(a6l_clk_hold_init);
module_exit(a6l_clk_hold_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Hisense A6L: hold RPM-SMD clocks enabled (bring-up experiment)");
