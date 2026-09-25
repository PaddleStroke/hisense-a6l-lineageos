// SPDX-License-Identifier: GPL-2.0-only
/*
 * a6l_ipa_ovl (kvoice agent, 24 Sep 2026): ATTENDED TEST ONLY, V74 recovery.
 * Applies a6l-ipa-v75.dtbo to the LIVE device tree at insmod: /soc@0/ipa@147c0000 ("qcom,sdm660-ipa", ipa-legacy) and
 * /soc@0/dma-controller@14784000 ("a6l,ipa-bam-v1.7.0", a6l_ipa_bam). /soc@0 is a populated simple-bus, so the platform
 * devices are created by the overlay notifier; the drivers bind when their modules are loaded afterwards.
 * Load BEFORE the modem is started. rmmod reverts the overlay (only once the drivers are unloaded).
 */
#include <linux/module.h>
#include <linux/of.h>
#include <linux/printk.h>
#include "a6l_ipa_dtbo.h"

static int ovcs_id;

static int __init a6l_ipa_ovl_init(void)
{
	struct device_node *np;
	int ret;

	np = of_find_compatible_node(NULL, NULL, "qcom,sdm660-ipa");
	if (np) {
		of_node_put(np);
		pr_info("A6L_IPA_OVL already present in the DT, nothing to do\n");
		return -EEXIST;
	}
	np = of_find_node_by_path("/soc@0/remoteproc@4080000");
	if (!np) {
		pr_err("A6L_IPA_OVL modem remoteproc node not found\n");
		return -ENODEV;
	}
	of_node_put(np);

	ret = of_overlay_fdt_apply(a6l_ipa_dtbo, sizeof(a6l_ipa_dtbo), &ovcs_id, NULL);
	if (ret) {
		pr_err("A6L_IPA_OVL of_overlay_fdt_apply failed: %d\n", ret);
		if (ovcs_id)
			of_overlay_remove(&ovcs_id);
		return ret;
	}
	pr_info("A6L_IPA_OVL applied (ovcs %d): ipa@147c0000 + a6l ipa bam\n", ovcs_id);
	return 0;
}

static void __exit a6l_ipa_ovl_exit(void)
{
	int ret = of_overlay_remove(&ovcs_id);

	pr_info("A6L_IPA_OVL removed: %d\n", ret);
}

module_init(a6l_ipa_ovl_init);
module_exit(a6l_ipa_ovl_exit);
MODULE_DESCRIPTION("A6L attended-test DT overlay for IPA v2.6L (mobile data)");
MODULE_LICENSE("GPL");
