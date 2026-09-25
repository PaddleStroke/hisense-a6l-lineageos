// SPDX-License-Identifier: GPL-2.0-only
/*
 * a6l_cam_ovl (24 Sep 2026): ATTENDED TEST ONLY, V74 recovery.
 * Applies a6l-camera-v75.dtbo (CCI, IMX576/Hi-846/S5K3T1, GT9769 VCM, CAMSS) to the LIVE device tree,
 * so the cameras can be tested without a new boot image. Load BEFORE the camera modules.
 */
#include <linux/module.h>
#include <linux/of.h>
#include <linux/printk.h>
#include "a6l_cam_dtbo.h"

static int ovcs_id;

static int __init a6l_cam_ovl_init(void)
{
	struct device_node *np;
	int ret;

	np = of_find_compatible_node(NULL, NULL, "sony,imx576");
	if (np) {
		of_node_put(np);
		pr_info("A6L_CAM_OVL already present in the DT\n");
		return -EEXIST;
	}
	ret = of_overlay_fdt_apply(a6l_cam_dtbo, sizeof(a6l_cam_dtbo), &ovcs_id, NULL);
	if (ret) {
		pr_err("A6L_CAM_OVL of_overlay_fdt_apply failed: %d\n", ret);
		if (ovcs_id)
			of_overlay_remove(&ovcs_id);
		return ret;
	}
	pr_info("A6L_CAM_OVL applied (ovcs %d)\n", ovcs_id);
	return 0;
}

static void __exit a6l_cam_ovl_exit(void)
{
	pr_info("A6L_CAM_OVL removed: %d\n", of_overlay_remove(&ovcs_id));
}

module_init(a6l_cam_ovl_init);
module_exit(a6l_cam_ovl_exit);
MODULE_DESCRIPTION("A6L attended-test DT overlay for the cameras");
MODULE_LICENSE("GPL");
