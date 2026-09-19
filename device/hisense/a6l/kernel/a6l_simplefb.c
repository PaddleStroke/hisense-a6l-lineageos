// SPDX-License-Identifier: GPL-2.0-only
/* Expose only the existing, reserved LCD buffer through the built-in simpleDRM.
 * No PMIC, display-controller, e-ink, storage or arbitrary-address parameters.
 */
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/platform_data/simplefb.h>

#define FB_BASE 0x9d400000ULL
#define FB_RESERVED 0x23ff000ULL
#define FB_BYTES (1080U * 2340U * 4U)
static struct platform_device *fb;
static const struct simplefb_platform_data info = {
	.width = 1080, .height = 2340, .stride = 4320, .format = "a8r8g8b8",
};
static int __init a6l_fb_init(void)
{
	struct device_node *node;
	struct resource reserved, mem = {
		.start = FB_BASE, .end = FB_BASE + FB_BYTES - 1,
		.flags = IORESOURCE_MEM, .name = "a6l-boot-lcd",
	};
	int ret;
	if (!of_machine_is_compatible("hisense,hlte730t"))
		return -ENODEV;
	node = of_find_node_by_path("/reserved-memory/framebuffer@9d400000");
	if (!node)
		return -ENODEV;
	ret = of_address_to_resource(node, 0, &reserved);
	if (ret || reserved.start != FB_BASE || resource_size(&reserved) != FB_RESERVED ||
	    !of_property_present(node, "no-map")) {
		of_node_put(node);
		return -EINVAL;
	}
	of_node_put(node);
	fb = platform_device_register_resndata(NULL, "simple-framebuffer", PLATFORM_DEVID_AUTO,
		&mem, 1, &info, sizeof(info));
	if (IS_ERR(fb))
		return PTR_ERR(fb);
	pr_info("A6L_SIMPLEFB_REGISTERED base=%llx bytes=%u size=1080x2340 stride=4320\n",
		FB_BASE, FB_BYTES);
	return 0;
}
static void __exit a6l_fb_exit(void)
{
	platform_device_unregister(fb);
}
module_init(a6l_fb_init);
module_exit(a6l_fb_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("A6L guarded boot framebuffer to simpleDRM RAM diagnostic");
