/* A6L: API shims for building the NXP tfa98xx v6.7.14 out-of-tree driver against Linux 7.2 */
#include <linux/version.h>
#include <sound/soc.h>
#include <linux/firmware.h>
#include <linux/gpio.h>
#ifndef FW_ACTION_HOTPLUG
#define FW_ACTION_HOTPLUG FW_ACTION_UEVENT
#endif
#ifndef SND_SOC_DAIFMT_CBS_CFS
#define SND_SOC_DAIFMT_CBS_CFS SND_SOC_DAIFMT_CBC_CFC
#endif
#ifndef GPIOF_DIR_IN
#define GPIOF_DIR_IN GPIOF_IN
#endif
#ifndef SLAB_MEM_SPREAD
#define SLAB_MEM_SPREAD 0UL
#endif
/* snd_soc_kcontrol_component() was removed; the kcontrol private chip is the component */
#define snd_soc_kcontrol_component(k) ((struct snd_soc_component *)snd_kcontrol_chip(k))
/* devm_gpio_free() was removed: devres releases the GPIO on unbind; explicit early frees become no-ops */
#define devm_gpio_free(dev, gpio) do { (void)(dev); (void)(gpio); } while (0)
