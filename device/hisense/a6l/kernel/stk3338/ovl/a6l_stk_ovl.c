// SPDX-License-Identifier: GPL-2.0-only
/*
 * a6l_stk_ovl (stk agent, 25 Sep 2026): ATTENDED TEST ONLY, V74 recovery. Applies a6l-stk-power-v74.dtbo to the
 * LIVE device tree (same approach as a6l_cam_ovl / a6l_hall_ovl): always-on votes on pm660l L3 (STK3338 vdd) and
 * pm660 L13 (vio) + gpio71 pull-up, so the read-only I2C probe of the front STK3338 runs with the sensor powered.
 */
#include <linux/module.h>
#include <linux/of.h>
#include <linux/printk.h>
#include "a6l_stk_dtbo.h"

static int ovcs_id;

static int __init a6l_stk_ovl_init(void)
{
	struct device_node *np;
	int ret;

	np = of_find_node_by_path("/a6l-stk-vdd-vote");
	if (np) {
		of_node_put(np);
		pr_info("A6L_STK_OVL already present in the DT\n");
		return -EEXIST;
	}
	ret = of_overlay_fdt_apply(a6l_stk_dtbo, sizeof(a6l_stk_dtbo), &ovcs_id, NULL);
	if (ret) {
		pr_err("A6L_STK_OVL of_overlay_fdt_apply failed: %d\n", ret);
		if (ovcs_id)
			of_overlay_remove(&ovcs_id);
		return ret;
	}
	pr_info("A6L_STK_OVL applied (ovcs %d)\n", ovcs_id);
	return 0;
}

static void __exit a6l_stk_ovl_exit(void)
{
	pr_info("A6L_STK_OVL removed: %d\n", of_overlay_remove(&ovcs_id));
}

module_init(a6l_stk_ovl_init);
module_exit(a6l_stk_ovl_exit);
MODULE_DESCRIPTION("A6L attended-test DT overlay for the STK3338 front ALS/prox power vote");
MODULE_LICENSE("GPL");
