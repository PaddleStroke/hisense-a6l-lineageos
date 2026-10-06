/* r5 review F49 host test, driver side: the UNCHANGED a6l_gpio_vib.c compiled against kstub/ (userspace stand-ins). */
#include "../a6l_gpio_vib.c"
#include "vib_test_api.h"

long kst_jiffies;
struct gpio_desc kst_gpio;
int kst_led_register_err;
static struct platform_device pdev = { .dev = { .name = "a6l-vibrator" } };
static struct a6l_vib *V;

int kd_probe(void) { int r = kst_driver->probe(&pdev); V = r ? NULL : platform_get_drvdata(&pdev); return r; }
void kd_remove(void) { kst_driver->remove(&pdev); }
int kd_led_registered(void) { return V && V->led.registered; }
const char *kd_led_name(void) { return V ? V->led.name : ""; }
int kd_gpio(void) { return kst_gpio.value; }
unsigned long kd_on_count(void) { return V->on_count; }
static struct device_attribute *attr(const char *name)
{
	for (struct attribute **a = V->led.groups[0]->attrs; *a; a++)
		if (!strcmp((*a)->name, name))
			return container_of(*a, struct device_attribute, attr);
	return NULL;
}
int kd_has_attr(const char *name) { return V && V->led.registered && attr(name) != NULL; }
/* like kernfs: the store gets a NUL-terminated copy of the written bytes */
ssize_t kd_store(const char *name, const char *buf, size_t n)
{
	char page[64];
	struct device_attribute *a = attr(name);
	if (!a || n >= sizeof(page)) return -EINVAL;
	memcpy(page, buf, n); page[n] = 0;
	return a->store(V->led.dev, a, page, n);
}
int kd_show(const char *name, char *out) { struct device_attribute *a = attr(name); return a ? (int)a->show(V->led.dev, a, out) : -EINVAL; }
static void run_work(void) { if (V->work.pending) { V->work.pending = 0; V->work.fn(&V->work); } }
void kd_advance(long ms)
{
	for (long i = 0; i < ms; i++) {
		kst_jiffies++;
		struct delayed_work *d[] = { &V->timed_off, &V->pulse_off };
		for (int k = 0; k < 2; k++)
			if (d[k]->work.pending && d[k]->expires <= kst_jiffies) { d[k]->work.pending = 0; d[k]->work.fn(&d[k]->work); }
		run_work();
	}
}
int kd_timed_pending(void) { return V->timed_off.work.pending; }
int kd_suspend(void) { return a6l_vib_pm.suspend(&pdev.dev); }
int kd_resume(void) { return a6l_vib_pm.resume(&pdev.dev); }
int kd_brightness(int b) { return V->led.brightness_set_blocking(&V->led, (enum led_brightness)b); }
void kd_ff(int on)
{
	struct ff_effect e; memset(&e, 0, sizeof(e)); e.type = FF_CONSTANT; e.u.constant.level = on ? 0x7fff : 0;
	V->input->play(V->input, NULL, &e); run_work();
}
void kd_set_led_register_err(int e) { kst_led_register_err = e; }
