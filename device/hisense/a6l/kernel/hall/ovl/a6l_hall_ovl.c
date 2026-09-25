// SPDX-License-Identifier: GPL-2.0-only
/*
 * a6l_hall_ovl (misc agent, 24 Sep 2026): ATTENDED TEST ONLY, V74 recovery.
 * Applies a6l-hall-v75.dtbo (L13 vote through an always-on regulator-fixed + gpio-keys SW_LID on gpio75 with
 * pull-up) to the LIVE device tree, like a6l_cam_ovl. Needs gpio_keys (built in or module) for the input device.
 */
#include <linux/module.h>
#include <linux/of.h>
#include <linux/printk.h>
#include "a6l_hall_dtbo.h"

static int ovcs_id;

static int __init a6l_hall_ovl_init(void)
{
	struct device_node *np;
	int ret;

	np = of_find_node_by_path("/hall-sensor");
	if (np) {
		of_node_put(np);
		pr_info("A6L_HALL_OVL already present in the DT\n");
		return -EEXIST;
	}
	ret = of_overlay_fdt_apply(a6l_hall_dtbo, sizeof(a6l_hall_dtbo), &ovcs_id, NULL);
	if (ret) {
		pr_err("A6L_HALL_OVL of_overlay_fdt_apply failed: %d\n", ret);
		if (ovcs_id)
			of_overlay_remove(&ovcs_id);
		return ret;
	}
	pr_info("A6L_HALL_OVL applied (ovcs %d)\n", ovcs_id);
	return 0;
}

static void __exit a6l_hall_ovl_exit(void)
{
	pr_info("A6L_HALL_OVL removed: %d\n", of_overlay_remove(&ovcs_id));
}

module_init(a6l_hall_ovl_init);
module_exit(a6l_hall_ovl_exit);
MODULE_DESCRIPTION("A6L attended-test DT overlay for the Hall sensor");
MODULE_LICENSE("GPL");
