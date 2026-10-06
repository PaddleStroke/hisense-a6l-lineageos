// SPDX-License-Identifier: GPL-2.0-only
/*
 * a6l_fl_ovl (agent dualux, 25 Sep 2026): ATTENDED TEST ONLY (V74 recovery), like a6l_hall_ovl / a6l_cam_ovl.
 * Applies a6l-eink-frontlight-v75.dtbo (PM660L LPG channel 4 -> DTEST2 -> PM660L GPIO6, leds-pwm "epd-backlight",
 * off at boot) to the LIVE device tree. Needs leds-qcom-lpg.ko and leds-pwm.ko loaded (or loadable) for the devices
 * to bind. Removing the module removes the overlay (after `echo 0 > /sys/class/leds/epd-backlight/brightness`).
 *
 * misc2 (25 Sep 2026) fix for "qcom-spmi-lpg 800f000.spmi:pmic@3:pwm: error -ENXIO: parent regmap unavailable":
 * the overlay flips pm660l_lpg (pmic@3/pwm) to "okay". The OF reconfig notifier (of_platform_notify) then creates
 * the platform device with parent = of_find_device_by_node(pmic@3), which only searches the PLATFORM bus; pmic@3 is
 * an SPMI device (qcom-spmi-pmic, which owns the regmap), so the LPG device got parent NULL (platform root) and
 * dev_get_regmap(parent) failed. This is a runtime-overlay artefact only: in a DT merged at build time (ROM, V75)
 * qcom-spmi-pmic populates the node with the right parent. Fix here: destroy that device and re-create it with the
 * SPMI pmic@3 device as parent (found through an already-populated sibling, e.g. the WLED at d800, because
 * spmi_bus_type is static in 7.2).
 */
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/printk.h>
#include "a6l_fl_dtbo.h"

/* the SPMI device of @pmic_np, through any child already populated as a platform device by qcom-spmi-pmic */
static struct device *a6l_fl_pmic_dev(struct device_node *pmic_np, struct device_node *skip)
{
	struct device_node *child;
	struct device *found = NULL;

	for_each_available_child_of_node(pmic_np, child) {
		struct platform_device *pd;

		if (child == skip)
			continue;
		pd = of_find_device_by_node(child);
		if (!pd)
			continue;
		if (pd->dev.parent && pd->dev.parent->of_node == pmic_np)
			found = get_device(pd->dev.parent);
		put_device(&pd->dev);
		if (found) {
			of_node_put(child);
			break;
		}
	}
	return found;
}

static int a6l_fl_fix_lpg_parent(void)
{
	struct device_node *np, *pmic_np;
	struct platform_device *pd, *npd;
	struct device *pmic;
	int ret = 0;

	np = of_find_compatible_node(NULL, NULL, "qcom,pm660l-lpg");
	if (!np) {
		pr_err("A6L_FL_OVL no qcom,pm660l-lpg node\n");
		return -ENODEV;
	}
	pmic_np = of_get_parent(np);
	pmic = a6l_fl_pmic_dev(pmic_np, np);
	if (!pmic) {
		pr_err("A6L_FL_OVL no populated sibling under %pOF: cannot find the SPMI pmic device\n", pmic_np);
		ret = -ENODEV;
		goto out;
	}
	pd = of_find_device_by_node(np);
	if (pd && pd->dev.parent == pmic) {
		pr_info("A6L_FL_OVL LPG %s already has parent %s (driver %s)\n", dev_name(&pd->dev), dev_name(pmic),
			pd->dev.driver ? pd->dev.driver->name : "none");
		put_device(&pd->dev);
		goto out_put;
	}
	if (pd) {
		pr_info("A6L_FL_OVL LPG %s has parent %s (no regmap): re-creating under %s\n", dev_name(&pd->dev),
			pd->dev.parent ? dev_name(pd->dev.parent) : "none", dev_name(pmic));
		of_platform_device_destroy(&pd->dev, NULL);	/* unregisters, clears OF_POPULATED */
		put_device(&pd->dev);
	}
	npd = of_platform_device_create(np, NULL, pmic);
	if (!npd) {
		pr_err("A6L_FL_OVL of_platform_device_create(%pOF) failed\n", np);
		ret = -ENODEV;
		goto out_put;
	}
	pr_info("A6L_FL_OVL LPG %s parent=%s driver=%s\n", dev_name(&npd->dev), dev_name(npd->dev.parent),
		npd->dev.driver ? npd->dev.driver->name : "none (leds-qcom-lpg loaded?)");
out_put:
	put_device(pmic);
out:
	of_node_put(pmic_np);
	of_node_put(np);
	return ret;
}

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
	ret = a6l_fl_fix_lpg_parent();
	if (ret)
		pr_err("A6L_FL_OVL LPG parent fix failed: %d (overlay kept)\n", ret);
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
