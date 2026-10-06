/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * a6l_ovl_reparent.h (flash/microSD agent, 28 Sep 2026): shared helper for the runtime-overlay loader modules.
 * The OF reconfig notifier (drivers/of/platform.c of_platform_notify) creates the platform device of a node that
 * an overlay enables with parent = of_find_device_by_node(parent node), which is NULL when the parent is not a
 * platform device (SPMI PMIC, rpmsg smd-rpm) -> the device ends up under /sys/devices/platform and drivers that
 * use dev->parent (leds-qcom-flash: dev_get_regmap(dev->parent); qcom_smd-regulator: dev_get_drvdata(parent))
 * fail with -ENODEV. a6l_ovl_reparent() recreates the device with the parent of an already-bound sibling.
 */
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>

static struct device *a6l_sibling_parent(struct device_node *np)
{
	struct device_node *c;

	for_each_child_of_node(np->parent, c) {
		struct platform_device *s;
		struct device *p;

		if (c == np)
			continue;
		s = of_find_device_by_node(c);
		if (!s)
			continue;
		p = s->dev.parent;
		if (p && p->bus) {	/* a real bus device (spmi / rpmsg), not the platform_bus root */
			get_device(p);
			put_device(&s->dev);
			of_node_put(c);
			return p;
		}
		put_device(&s->dev);
	}
	return NULL;
}

static int a6l_ovl_reparent(const char *tag, struct device_node *np)
{
	struct device *par = a6l_sibling_parent(np);
	struct platform_device *pdev;

	if (!par) {
		pr_err("%s no bound sibling under %pOF to borrow the parent device from\n", tag, np->parent);
		return -ENODEV;
	}
	pdev = of_find_device_by_node(np);
	if (pdev) {
		if (pdev->dev.parent == par) {
			pr_info("%s %pOF already under %s\n", tag, np, dev_name(par));
			put_device(&pdev->dev);
			put_device(par);
			return 0;
		}
		pr_info("%s recreating %s (notifier parent %s) under %s\n", tag, dev_name(&pdev->dev),
			pdev->dev.parent ? dev_name(pdev->dev.parent) : "none", dev_name(par));
		of_platform_device_destroy(&pdev->dev, NULL);
		put_device(&pdev->dev);
	}
	pdev = of_platform_device_create(np, NULL, par);
	pr_info("%s %pOF -> %s parent %s\n", tag, np, pdev ? dev_name(&pdev->dev) : "CREATE-FAILED",
		dev_name(par));
	put_device(par);
	return pdev ? 0 : -EIO;
}
