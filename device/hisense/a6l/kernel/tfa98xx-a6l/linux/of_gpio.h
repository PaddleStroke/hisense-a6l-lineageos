/* A6L compat shim: <linux/of_gpio.h> was removed upstream; provide of_get_named_gpio() on top of gpiod. */
#ifndef A6L_COMPAT_OF_GPIO_H
#define A6L_COMPAT_OF_GPIO_H
#include <linux/gpio.h>
#include <linux/gpio/consumer.h>
#include <linux/of.h>
#include <linux/string.h>
#include <linux/err.h>
static inline int of_get_named_gpio(const struct device_node *np, const char *propname, int index)
{
	char con[64];
	struct gpio_desc *d;
	size_t l;
	int gpio;

	strscpy(con, propname, sizeof(con));
	l = strlen(con);
	if (l > 6 && !strcmp(con + l - 6, "-gpios"))
		con[l - 6] = 0;
	else if (l > 5 && !strcmp(con + l - 5, "-gpio"))
		con[l - 5] = 0;
	d = fwnode_gpiod_get_index(of_fwnode_handle((struct device_node *)np), con, index, GPIOD_ASIS, "tfa98xx-probe");
	if (IS_ERR(d))
		return PTR_ERR(d);
	gpio = desc_to_gpio(d);
	gpiod_put(d);
	return gpio;
}
#endif
