// SPDX-License-Identifier: GPL-2.0-only
/*
 * a6l_ipa2_ovl (ipa agent, 24 Sep 2026): ATTENDED TEST ONLY, V74 recovery (no boot-image change needed).
 * Applies a6l-ipa-v75.dtbo (default: IPA behind the anoc2 SMMU, translated) or, with identity=1,
 * a6l-ipa-identity-v75.dtbo (same node + a compatible that makes arm-smmu-qcom use an identity/bypass domain,
 * like the stock "qcom,smmu-s1-bypass") to the LIVE device tree. /soc@0 is a populated simple-bus, so the
 * platform device ipa@14780000 appears at once; load ipa2_lite.ko FIRST so it binds immediately.
 * The modem must NOT be running yet (the IPA probe resets the whole IPA block).
 */
#include <linux/delay.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/printk.h>
#include "a6l_ipa2_dtbo.h"

static bool identity;
module_param(identity, bool, 0444);
MODULE_PARM_DESC(identity, "use the identity-domain (SMMU bypass) variant of the IPA node");
/* A6L ipa2fix: node without "iommus" -> no anoc2 SMMU programming at all. Only for register-level steps
 * (ipa2_lite stop_at<=13): any IPA DMA would then be an unmatched stream (USFCFG fault). */
static bool noiommu;
module_param(noiommu, bool, 0444);
MODULE_PARM_DESC(noiommu, "use the variant without iommus (register steps only, no DMA)");

static int ovcs_id;

static int __init a6l_ipa2_ovl_init(void)
{
	const void *fdt = identity ? a6l_ipa_identity_dtbo : a6l_ipa_dtbo;
	u32 size = identity ? sizeof(a6l_ipa_identity_dtbo) : sizeof(a6l_ipa_dtbo);
	const char *vname = identity ? "identity" : "translated";
	struct device_node *np;
	int ret;

	if (noiommu) {
		if (identity) {
			pr_err("A6L_IPA2_OVL identity=1 and noiommu=1 are exclusive\n");
			return -EINVAL;
		}
		fdt = a6l_ipa_noiommu_dtbo;
		size = sizeof(a6l_ipa_noiommu_dtbo);
		vname = "noiommu";
	}

	np = of_find_compatible_node(NULL, NULL, "qcom,sdm660-ipa-lite");
	if (!np)
		np = of_find_compatible_node(NULL, NULL, "qcom,sdm660-ipa");	/* kvoice ipa-legacy overlay */
	if (np) {
		of_node_put(np);
		pr_err("A6L_IPA2_OVL an IPA node is already in the live DT (%s): not applying\n",
		       identity ? "identity" : "translated");
		return -EEXIST;
	}
	np = of_find_node_by_path("/soc@0/remoteproc@4080000");
	if (!np) {
		pr_err("A6L_IPA2_OVL modem remoteproc node not found\n");
		return -ENODEV;
	}
	of_node_put(np);

	/* the platform device is created, SMMU-attached and probed (ipa2_lite already loaded) inside this call */
	pr_emerg("A6L_IPA_OVL_STEP applying %s overlay (device create + SMMU attach + ipa2_lite probe follow)\n", vname);
	if (!noiommu)
		pr_emerg("A6L_IPA_OVL_STEP SMMU attach anoc2 SID 0x19c0 next; the next line should be A6L_IPA_STEP 0\n");
	msleep(50);
	ret = of_overlay_fdt_apply(fdt, size, &ovcs_id, NULL);
	if (ret) {
		pr_err("A6L_IPA2_OVL of_overlay_fdt_apply failed: %d\n", ret);
		if (ovcs_id)
			of_overlay_remove(&ovcs_id);
		return ret;
	}
	pr_emerg("A6L_IPA2_OVL applied (ovcs %d): ipa@14780000 (%s)\n", ovcs_id, vname);
	return 0;
}

static void __exit a6l_ipa2_ovl_exit(void)
{
	int ret = of_overlay_remove(&ovcs_id);

	pr_info("A6L_IPA2_OVL removed: %d\n", ret);
}

module_init(a6l_ipa2_ovl_init);
module_exit(a6l_ipa2_ovl_exit);
MODULE_DESCRIPTION("A6L attended-test DT overlay for IPA v2.6L (ipa2-lite, mobile data)");
MODULE_LICENSE("GPL");
