// SPDX-License-Identifier: GPL-2.0-only
/*
 * A6L bring-up experiment: enable regulators by their sysfs device name (regulator.N) without a DT consumer.
 * 22 Sep 2026: rooted-stock diff idle->e-ink update shows pm660_l1 (1.2 V) switching ON and pm660_l8 (1.8 V) gaining
 * users; the TC358767 bridge needs a 1.2 V core rail and 1.8 V IO. insmod a6l-reg-hold.ko names=pm660_l8,pm660_l11 ofpath=/soc@0/display-subsystem@c900000/dsi@c996000 ofid=vdda (pm660 l1)
 */
#include <linux/module.h>
#include <linux/of.h>
#include <linux/regulator/consumer.h>
#include <linux/regulator/of_regulator.h>

static char *names[8];
static int nnames;
module_param_array(names, charp, &nnames, 0444);
static char *ofpath;	/* DT node that carries "<ofid>-supply" (for regulators without a regulator-name) */
module_param(ofpath, charp, 0444);
static char *ofid;
module_param(ofid, charp, 0444);
static struct regulator *held[8], *ofheld;

static int __init a6l_reg_hold_init(void)
{
	int i, ret;

	for (i = 0; i < nnames; i++) {
		held[i] = regulator_get(NULL, names[i]);
		if (IS_ERR(held[i])) {
			pr_err("a6l-reg-hold: %s: %ld\n", names[i], PTR_ERR(held[i]));
			held[i] = NULL;
			continue;
		}
		ret = regulator_enable(held[i]);
		pr_info("a6l-reg-hold: %s enable=%d uV=%d\n", names[i], ret, regulator_get_voltage(held[i]));
	}
	if (ofpath && ofid) {
		struct device_node *np = of_find_node_by_path(ofpath);

		ofheld = np ? of_regulator_get(NULL, np, ofid) : ERR_PTR(-ENOENT);
		of_node_put(np);
		if (IS_ERR(ofheld)) {
			pr_err("a6l-reg-hold: %s/%s: %ld\n", ofpath, ofid, PTR_ERR(ofheld));
			ofheld = NULL;
		} else {
			ret = regulator_enable(ofheld);
			pr_info("a6l-reg-hold: %s/%s enable=%d uV=%d\n", ofpath, ofid, ret, regulator_get_voltage(ofheld));
		}
	}
	return 0;
}

static void __exit a6l_reg_hold_exit(void)
{
	int i;

	for (i = 0; i < nnames; i++)
		if (held[i]) {
			regulator_disable(held[i]);
			regulator_put(held[i]);
		}
	if (ofheld) {
		regulator_disable(ofheld);
		regulator_put(ofheld);
	}
}
module_init(a6l_reg_hold_init);
module_exit(a6l_reg_hold_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Hisense A6L: hold regulators enabled by name (bring-up experiment)");
