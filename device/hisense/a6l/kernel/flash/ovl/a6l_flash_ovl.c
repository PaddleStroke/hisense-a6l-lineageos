// SPDX-License-Identifier: GPL-2.0-only
/*
 * a6l_flash_ovl (flash/microSD agent, 28 Sep 2026): ATTENDED TEST ONLY, V74/V75-usb recovery (no configfs overlays).
 * Applies the embedded a6l-flash-rt-v75.dtbo (PM660L flash led-controller@d300 okay + led-0 on channel 1,
 * torch <= 500 mA, flash <= 500 mA / 100 ms) to the LIVE device tree, then re-parents the new platform device to
 * the PM660L SPMI device (leds-qcom-flash needs dev->parent's regmap). Insert BEFORE leds-qcom-flash.ko.
 */
#include <linux/module.h>
#include <linux/of.h>
#include <linux/printk.h>
#include "a6l_ovl_reparent.h"
#include "a6l_flash_dtbo.h"

static int ovcs_id;

static int __init a6l_flash_ovl_init(void)
{
	struct device_node *np;
	int ret;

	np = of_find_compatible_node(NULL, NULL, "qcom,spmi-flash-led");
	if (!np) {
		pr_err("A6L_FLASH_OVL_FAIL no qcom,spmi-flash-led node in the live DT\n");
		return -ENODEV;
	}
	if (of_device_is_available(np)) {
		pr_info("A6L_FLASH_OVL already enabled in the DT (%pOF)\n", np);
		of_node_put(np);
		return -EEXIST;
	}
	ret = of_overlay_fdt_apply(a6l_flash_dtbo, sizeof(a6l_flash_dtbo), &ovcs_id, NULL);
	if (ret) {
		pr_err("A6L_FLASH_OVL_FAIL of_overlay_fdt_apply: %d\n", ret);
		if (ovcs_id)
			of_overlay_remove(&ovcs_id);
		of_node_put(np);
		return ret;
	}
	pr_info("A6L_FLASH_OVL applied (ovcs %d)\n", ovcs_id);
	ret = a6l_ovl_reparent("A6L_FLASH_OVL", np);
	if (ret) {
		pr_err("A6L_FLASH_OVL_FAIL reparent: %d (overlay removed)\n", ret);
		of_overlay_remove(&ovcs_id);
		of_node_put(np);
		return ret;
	}
	of_node_put(np);
	pr_info("A6L_FLASH_OVL ready: now insmod led-class-flash.ko leds-qcom-flash.ko\n");
	return 0;
}

static void __exit a6l_flash_ovl_exit(void)
{
	struct device_node *np = of_find_compatible_node(NULL, NULL, "qcom,spmi-flash-led");
	struct platform_device *pdev = np ? of_find_device_by_node(np) : NULL;

	if (pdev) {
		of_platform_device_destroy(&pdev->dev, NULL);
		put_device(&pdev->dev);
	}
	of_node_put(np);
	pr_info("A6L_FLASH_OVL removed: %d\n", of_overlay_remove(&ovcs_id));
}

module_init(a6l_flash_ovl_init);
module_exit(a6l_flash_ovl_exit);
MODULE_DESCRIPTION("A6L attended-test DT overlay for the PM660L flash/torch LED");
MODULE_LICENSE("GPL");
