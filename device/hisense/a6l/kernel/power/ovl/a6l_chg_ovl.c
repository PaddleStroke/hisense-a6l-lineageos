// SPDX-License-Identifier: GPL-2.0-only
/*
 * a6l_chg_ovl (agent power, 26 Sep 2026): ATTENDED TEST ONLY (V74 recovery), like a6l_fl_ovl / a6l_cam_ovl.
 * Applies a6l-charger-test.dtbo (= a6l-charger-v75 with a 0.8 A DCP input limit) to the LIVE device tree:
 * /battery (simple-battery 3800 mAh / 4.4 V), pm660_rradc (adc@4500), pm660_fg (battery@4000) and pm660_charger
 * (charger@1000) -> "okay". It does NOT load any driver: binding happens only when the run script insmods
 * qcom-spmi-rradc.ko / pmi8998_fg.ko (read-only telemetry) and, later and separately, qcom_smbx.ko (writes the SMB2
 * init sequence, enables charging). Removing the module removes the overlay (unbinds the drivers).
 *
 * Runtime-overlay artefact (same as a6l_fl_ovl, misc2 25 Sep): of_platform_notify() creates the new platform devices
 * with parent = of_find_device_by_node(pmic@0), which only searches the platform bus; pmic@0 is an SPMI device, so the
 * devices end up under the platform root and dev_get_regmap(parent) fails ("failed to locate the regmap"). Fix: destroy
 * each new device and re-create it with the SPMI pmic@0 device (found through an already-populated sibling such as the
 * PON or temp-alarm) as parent. In a DT merged at build time (ROM V75) qcom-spmi-pmic populates them correctly.
 */
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/printk.h>
#include "a6l_chg_dtbo.h"

static const char * const a6l_chg_compat[] = { "qcom,pm660-rradc", "qcom,pmi8998-fg", "qcom,pm660-charger" };

static struct device *a6l_chg_pmic_dev(struct device_node *pmic_np)
{
	struct device_node *child;
	struct device *found = NULL;

	for_each_available_child_of_node(pmic_np, child) {
		struct platform_device *pd = of_find_device_by_node(child);

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

static int a6l_chg_fix_parent(const char *compat)
{
	struct device_node *np, *pmic_np;
	struct platform_device *pd, *npd;
	struct device *pmic;
	int ret = 0;

	np = of_find_compatible_node(NULL, NULL, compat);
	if (!np) {
		pr_err("A6L_CHG_OVL no %s node\n", compat);
		return -ENODEV;
	}
	pmic_np = of_get_parent(np);
	pmic = a6l_chg_pmic_dev(pmic_np);
	if (!pmic) {
		pr_err("A6L_CHG_OVL no populated sibling under %pOF\n", pmic_np);
		ret = -ENODEV;
		goto out;
	}
	pd = of_find_device_by_node(np);
	if (pd && pd->dev.parent == pmic) {
		pr_info("A6L_CHG_OVL %s: %s already under %s\n", compat, dev_name(&pd->dev), dev_name(pmic));
		put_device(&pd->dev);
		goto out_put;
	}
	if (pd) {
		pr_info("A6L_CHG_OVL %s: %s parent %s -> re-creating under %s\n", compat, dev_name(&pd->dev),
			pd->dev.parent ? dev_name(pd->dev.parent) : "none", dev_name(pmic));
		of_platform_device_destroy(&pd->dev, NULL);
		put_device(&pd->dev);
	}
	npd = of_platform_device_create(np, NULL, pmic);
	if (!npd) {
		pr_err("A6L_CHG_OVL of_platform_device_create(%pOF) failed\n", np);
		ret = -ENODEV;
		goto out_put;
	}
	pr_info("A6L_CHG_OVL %s: %s parent=%s driver=%s\n", compat, dev_name(&npd->dev), dev_name(npd->dev.parent),
		npd->dev.driver ? npd->dev.driver->name : "none (not loaded yet)");
out_put:
	put_device(pmic);
out:
	of_node_put(pmic_np);
	of_node_put(np);
	return ret;
}

static int ovcs_id;

static int __init a6l_chg_ovl_init(void)
{
	struct device_node *np;
	unsigned int i;
	int ret;

	np = of_find_compatible_node(NULL, NULL, "qcom,pm660-charger");
	if (np && of_device_is_available(np)) {
		of_node_put(np);
		pr_info("A6L_CHG_OVL charger already enabled in this DT (V75?): nothing to do\n");
		return -EEXIST;
	}
	of_node_put(np);
	ret = of_overlay_fdt_apply(a6l_chg_dtbo, sizeof(a6l_chg_dtbo), &ovcs_id, NULL);
	if (ret) {
		pr_err("A6L_CHG_OVL of_overlay_fdt_apply failed: %d\n", ret);
		if (ovcs_id)
			of_overlay_remove(&ovcs_id);
		return ret;
	}
	pr_info("A6L_CHG_OVL applied (ovcs %d)\n", ovcs_id);
	for (i = 0; i < ARRAY_SIZE(a6l_chg_compat); i++)
		if (a6l_chg_fix_parent(a6l_chg_compat[i]))
			pr_err("A6L_CHG_OVL parent fix failed for %s (overlay kept)\n", a6l_chg_compat[i]);
	return 0;
}

static void __exit a6l_chg_ovl_exit(void)
{
	pr_info("A6L_CHG_OVL removed: %d\n", of_overlay_remove(&ovcs_id));
}

module_init(a6l_chg_ovl_init);
module_exit(a6l_chg_ovl_exit);
MODULE_DESCRIPTION("A6L attended-test DT overlay for the PM660 charger / fuel gauge / RRADC");
MODULE_LICENSE("GPL");
