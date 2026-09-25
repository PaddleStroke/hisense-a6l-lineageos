// SPDX-License-Identifier: GPL-2.0-only
/*
 * a6l_fl_ovl (agent dualux, 25 Sep 2026): ATTENDED TEST ONLY (V74 recovery), like a6l_hall_ovl / a6l_cam_ovl.
 * Applies a6l-eink-frontlight-v75.dtbo (PM660L LPG channel 4 -> DTEST2 -> PM660L GPIO6, leds-pwm "epd-backlight",
 * off at boot) to the LIVE device tree. Needs leds-qcom-lpg.ko and leds-pwm.ko loaded (or loadable) for the devices
 * to bind. Removing the module removes the overlay (after `echo 0 > /sys/class/leds/epd-backlight/brightness`).
 */
#include <linux/module.h>
#include <linux/of.h>
#include <linux/printk.h>
#include "a6l_fl_dtbo.h"

static int ovcs_id;

static int __init a6l_fl_ovl_init(void)
{
	struct device_node *np;
	int ret;

	np = of_find_node_by_path("/a6l-epd-frontlight");
	if (np) {
		of_node_put(np);
		pr_info("A6L_FL_OVL already present in the DT\n");
		return -EEXIST;
	}
	ret = of_overlay_fdt_apply(a6l_fl_dtbo, sizeof(a6l_fl_dtbo), &ovcs_id, NULL);
	if (ret) {
		pr_err("A6L_FL_OVL of_overlay_fdt_apply failed: %d\n", ret);
		if (ovcs_id)
			of_overlay_remove(&ovcs_id);
		return ret;
	}
	pr_info("A6L_FL_OVL applied (ovcs %d)\n", ovcs_id);
	return 0;
}

static void __exit a6l_fl_ovl_exit(void)
{
	pr_info("A6L_FL_OVL removed: %d\n", of_overlay_remove(&ovcs_id));
}

module_init(a6l_fl_ovl_init);
module_exit(a6l_fl_ovl_exit);
MODULE_DESCRIPTION("A6L attended-test DT overlay for the rear e-ink frontlight");
MODULE_LICENSE("GPL");
