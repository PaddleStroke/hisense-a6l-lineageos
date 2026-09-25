// SPDX-License-Identifier: GPL-2.0-only
/*
 * a6l_voice_ovl (kvoice agent, 24 Sep 2026): ATTENDED TEST ONLY, V74 recovery.
 * Applies a6l-voice-onbase-v75.dtbo to the LIVE device tree at insmod, so the q6voice path can be tested
 * without a new boot image: APR services q6mvm/q6cvs/q6cvp (+ q6voice-dais) and the sound-card links
 * MultiMedia1 / MultiMedia2 / VoiceMMode1 (V74 base "onbase" layout; needs the patched q6asm-dai).
 * MUST be loaded BEFORE the ADSP is started (apr creates its service devices when the ADSP glink edge
 * comes up) and before snd-soc-sm8250 binds the card. rmmod reverts the overlay (only while unused).
 * Kernel overlays prepend new children exactly like fdtoverlay (see docs/audio3-20260924.md).
 */
#include <linux/module.h>
#include <linux/of.h>
#include <linux/printk.h>
#include "a6l_voice_dtbo.h"

static int ovcs_id;

static int __init a6l_voice_ovl_init(void)
{
	struct device_node *np;
	int ret;

	np = of_find_compatible_node(NULL, NULL, "qcom,q6mvm");
	if (np) {
		of_node_put(np);
		pr_info("A6L_VOICE_OVL already present in the DT, nothing to do\n");
		return -EEXIST;
	}
	np = of_find_node_by_path("/soc@0/remoteproc@15700000/glink-edge/apr");
	if (!np) {
		pr_err("A6L_VOICE_OVL apr node not found\n");
		return -ENODEV;
	}
	of_node_put(np);

	ret = of_overlay_fdt_apply(a6l_voice_dtbo, sizeof(a6l_voice_dtbo), &ovcs_id, NULL);
	if (ret) {
		pr_err("A6L_VOICE_OVL of_overlay_fdt_apply failed: %d\n", ret);
		if (ovcs_id)
			of_overlay_remove(&ovcs_id);
		return ret;
	}
	pr_info("A6L_VOICE_OVL applied (ovcs %d): q6mvm/q6cvs/q6cvp + VoiceMMode1 link\n", ovcs_id);
	return 0;
}

static void __exit a6l_voice_ovl_exit(void)
{
	int ret = of_overlay_remove(&ovcs_id);

	pr_info("A6L_VOICE_OVL removed: %d\n", ret);
}

module_init(a6l_voice_ovl_init);
module_exit(a6l_voice_ovl_exit);
MODULE_DESCRIPTION("A6L attended-test DT overlay for q6voice (call audio)");
MODULE_LICENSE("GPL");
