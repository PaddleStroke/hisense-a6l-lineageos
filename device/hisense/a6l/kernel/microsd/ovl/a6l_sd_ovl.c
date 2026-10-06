// SPDX-License-Identifier: GPL-2.0-only
/*
 * a6l_sd_ovl (flash/microSD agent, 28 Sep 2026): ATTENDED TEST ONLY, V74/V75-usb recovery (no configfs overlays).
 * Applies one of the embedded a6l-microsd-rt-v75 dtbos (param cd: 0 = cd-gpio54 active-high = stock,
 * 1 = active-low, 2 = broken-cd polling) to the LIVE device tree:
 *   - new rpm regulator node regulators-a6lsd (pm660l L2 vqmmc, L5 vmmc), re-parented to the smd-rpm device,
 *   - tlmm gpio54 cd pin state, sdhc_2 (mmc@c084000) okay.
 * sdhci-msm (already loaded for the eMMC) probes sdhc_2 when the regulators register (deferred probe).
 * The eMMC (mmc@c0c4000, mmc1/mmcblk1 on this kernel) is not touched.
 */
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/printk.h>
#include "a6l_ovl_reparent.h"
#include "a6l_sd_dtbo.h"

static int cd;
module_param(cd, int, 0444);
MODULE_PARM_DESC(cd, "card detect: 0 = gpio54 active-high (stock), 1 = active-low, 2 = broken-cd (polling); 3 = live L5 window fix + regulators reprobe (after 0..2)");

static int ovcs_id;

static int __init a6l_sd_ovl_init(void)
{
	static const struct { const void *p; size_t n; } dtbo[3] = {
		{ a6l_sd_dtbo0, sizeof(a6l_sd_dtbo0) },
		{ a6l_sd_dtbo1, sizeof(a6l_sd_dtbo1) },
		{ a6l_sd_dtbo2, sizeof(a6l_sd_dtbo2) },
	};
	struct device_node *np;
	int ret;

	if (cd == 3) {
		struct platform_device *pdev;

		np = of_find_node_by_path("/remoteproc/glink-edge/rpm-requests/regulators-a6lsd");
		if (!np) {
			pr_err("A6L_SD_FIX_FAIL no regulators-a6lsd (load cd=0 first)\n");
			return -ENODEV;
		}
		/*
		 * r5 bug hunt round2 kernel-drivers (29 Sep 2026): look the device up BEFORE applying, and remove the overlay
		 * on every failure. A failed module init has no exit, so the fix overlay used to stay applied with its
		 * changeset id lost (not removable, the next cd=3 stacked a second copy on the live regulators).
		 */
		pdev = of_find_device_by_node(np);
		if (!pdev) {
			pr_err("A6L_SD_FIX_FAIL no platform device for regulators-a6lsd\n");
			of_node_put(np);
			return -ENODEV;
		}
		ret = of_overlay_fdt_apply(a6l_sd_fix_dtbo, sizeof(a6l_sd_fix_dtbo), &ovcs_id, NULL);
		of_node_put(np);
		if (ret) {
			pr_err("A6L_SD_FIX_FAIL of_overlay_fdt_apply: %d\n", ret);
			if (ovcs_id)
				of_overlay_remove(&ovcs_id);
			put_device(&pdev->dev);
			return ret;
		}
		ret = device_reprobe(&pdev->dev);
		pr_info("A6L_SD_FIX applied (ovcs %d), regulators reprobe: %d, driver=%s\n", ovcs_id, ret,
			pdev->dev.driver ? pdev->dev.driver->name : "none");
		put_device(&pdev->dev);
		return 0;
	}
	if (cd < 0 || cd > 2)
		return -EINVAL;
	np = of_find_node_by_path("/soc@0/mmc@c084000");
	if (!np) {
		pr_err("A6L_SD_OVL_FAIL no /soc@0/mmc@c084000 (sdhc_2) in the live DT\n");
		return -ENODEV;
	}
	ret = of_device_is_available(np);
	of_node_put(np);
	if (ret) {
		pr_info("A6L_SD_OVL sdhc_2 already enabled in the DT\n");
		return -EEXIST;
	}
	np = of_find_node_by_path("/remoteproc/glink-edge/rpm-requests/regulators-a6lsd");
	if (np) {
		of_node_put(np);
		pr_info("A6L_SD_OVL regulators-a6lsd already present\n");
		return -EEXIST;
	}
	ret = of_overlay_fdt_apply(dtbo[cd].p, dtbo[cd].n, &ovcs_id, NULL);
	if (ret) {
		pr_err("A6L_SD_OVL_FAIL of_overlay_fdt_apply: %d\n", ret);
		if (ovcs_id)
			of_overlay_remove(&ovcs_id);
		return ret;
	}
	pr_info("A6L_SD_OVL applied (ovcs %d, cd mode %d)\n", ovcs_id, cd);
	np = of_find_node_by_path("/remoteproc/glink-edge/rpm-requests/regulators-a6lsd");
	ret = np ? a6l_ovl_reparent("A6L_SD_OVL", np) : -ENOENT;
	of_node_put(np);
	if (ret) {
		pr_err("A6L_SD_OVL_FAIL regulators reparent: %d (overlay removed)\n", ret);
		of_overlay_remove(&ovcs_id);
		return ret;
	}
	pr_info("A6L_SD_OVL ready: sdhc_2 probes once pm660l_l2/l5 register\n");
	return 0;
}

static void __exit a6l_sd_ovl_exit(void)
{
	pr_info("A6L_SD_OVL removed: %d\n", of_overlay_remove(&ovcs_id));
}

module_init(a6l_sd_ovl_init);
module_exit(a6l_sd_ovl_exit);
MODULE_DESCRIPTION("A6L attended-test DT overlay for the microSD slot (sdhc_2)");
MODULE_LICENSE("GPL");
