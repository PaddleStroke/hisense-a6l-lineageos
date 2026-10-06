// SPDX-License-Identifier: GPL-2.0-only
/*
 * a6l_vib_ovl (agent misc2, 25 Sep 2026): ATTENDED TEST ONLY, V74 recovery.
 * Applies the test form of a6l-vibrator-v75.dtbo (TLMM gpio79 pin state + /a6l-vibrator node, WITHOUT the
 * pm660_haptics status change) to the LIVE device tree. a6l_gpio_vib.ko then binds to /a6l-vibrator.
 */
#include <linux/module.h>
#include <linux/of.h>
#include <linux/printk.h>
#include "a6l_vib_dtbo.h"

static int ovcs_id;

static int __init a6l_vib_ovl_init(void)
{
	struct device_node *np;
	int ret;

	np = of_find_node_by_path("/a6l-vibrator");
	if (np) {
		of_node_put(np);
		pr_info("A6L_VIB_OVL already present in the DT\n");
		return -EEXIST;
	}
	ret = of_overlay_fdt_apply(a6l_vib_dtbo, sizeof(a6l_vib_dtbo), &ovcs_id, NULL);
	if (ret) {
		pr_err("A6L_VIB_OVL of_overlay_fdt_apply failed: %d\n", ret);
		if (ovcs_id)
			of_overlay_remove(&ovcs_id);
		return ret;
	}
	pr_info("A6L_VIB_OVL applied (ovcs %d)\n", ovcs_id);
	return 0;
}

static void __exit a6l_vib_ovl_exit(void)
{
	pr_info("A6L_VIB_OVL removed: %d\n", of_overlay_remove(&ovcs_id));
}

module_init(a6l_vib_ovl_init);
module_exit(a6l_vib_ovl_exit);
MODULE_DESCRIPTION("A6L attended-test DT overlay for the GPIO79 vibrator");
MODULE_LICENSE("GPL");
