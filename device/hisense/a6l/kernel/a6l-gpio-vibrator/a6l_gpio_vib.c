// SPDX-License-Identifier: GPL-2.0-only
/*
 * a6l_gpio_vib (agent misc2, 25 Sep 2026): Hisense A6L vibrator.
 *
 * Stock does NOT use the PM660 haptics block (qcom,haptic@c000 is in the stock DT but the stock kernel has no
 * qpnp-haptic driver at all: no symbol, no string). The stock vibrator is "timed-gpio" (drivers/staging/android
 * timed_gpio.c, DT node timed-gpio { vib-gpio = <&tlmm 79 0>; pinctrl vib_en_pin: gpio79 output-low }):
 * timed_gpio_probe -> gpiod_direction_output_raw(gpio79, 0); gpio_enable(ms) -> raw level 1 (active_low = 0),
 * hrtimer off after min(ms, 15000). I.e. TLMM GPIO79 is the enable of a DC motor driver: high = vibrate.
 *
 * This driver exposes it as an input force-feedback device (memless) with FF_CONSTANT, FF_PERIODIC and FF_RUMBLE,
 * which is what vendor.qti.hardware.vibrator (VibratorOL, InputFFDevice) looks for (FF_CONSTANT || FF_PERIODIC),
 * and what the a6l_vib test tool uses (FF_RUMBLE). The motor has no amplitude control: any non-zero level = on.
 * FF_GAIN is provided by ff-memless (gain 0 = off).
 *
 * DT: compatible = "hisense,a6l-gpio-vibrator"; enable-gpios = <&tlmm 79 GPIO_ACTIVE_HIGH>; optional pinctrl.
 * debugfs a6l_gpio_vib/pulse (root, write "<ms>", 1..2000): one bounded pulse for the attended test.
 *
 * r5 review fix F49 (29 Sep 2026): the packaged Android HAL (vendor.qti.hardware.vibrator.service, VibratorOL) only
 * accepts input FF devices from a fixed name list (qti-haptics, aw8697_haptic, ...), so "a6l_gpio_vibrator" was rejected
 * and on() silently succeeded with no effect. The HAL's other backend, LedVibratorDevice, drives
 * /sys/class/leds/vibrator/{state,duration,activate} (the LED "transient" contract: state = level while active,
 * duration = ms, activate 1 = start / 0 = stop) and reports only CAP_ON_CALLBACK - no amplitude control and no
 * effects, which is exactly what a binary GPIO motor can honour (the FF path would have advertised FF_GAIN amplitude
 * control that the GPIO cannot apply). This driver therefore also registers LED class device "vibrator" with those
 * three attributes implemented here (CONFIG_LEDS_TRIGGER_TRANSIENT is not set in the v67 kernel; the HAL rc's
 * `write .../trigger transient` just fails). The LED backend has priority in VibratorOL. The FF input device stays
 * for the a6l_vib test tool. Duration is capped at A6L_VIB_MAX_MS like stock timed-gpio (the HAL's 15 s clamp only
 * applies to its qpnp-vibrator-ldo backend).
 */
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/input.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/leds.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <linux/workqueue.h>

#define A6L_VIB_MAX_MS	15000	/* stock timed-gpio max_timeout */

struct a6l_vib {
	struct device *dev;
	struct input_dev *input;
	struct gpio_desc *gpio;
	struct work_struct work;
	struct delayed_work pulse_off;
	struct delayed_work timed_off;	/* F49: end of an LED-interface activation */
	struct led_classdev led;	/* F49: /sys/class/leds/vibrator for VibratorOL's LedVibratorDevice */
	struct mutex apply_lock;	/* serialises GPIO updates (work, debugfs, LED sysfs, PM) */
	struct mutex led_lock;		/* serialises LED activate/brightness requests */
	spinlock_t lock;
	unsigned long active_types;	/* bit per FF type index that currently asks for vibration */
	bool suspended;
	bool on;
	bool pulse;
	bool timed;			/* F49: LED activation in progress */
	bool removing;
	unsigned int led_duration;	/* ms, "duration" attribute */
	unsigned int led_state;		/* 0/1, "state" attribute */
	struct dentry *dbg;
	unsigned long on_count;
};

static void a6l_vib_apply(struct a6l_vib *v)
{
	unsigned long flags;
	bool want;

	mutex_lock(&v->apply_lock);
	spin_lock_irqsave(&v->lock, flags);
	want = (v->active_types || v->pulse || v->timed) && !v->suspended && !v->removing;
	spin_unlock_irqrestore(&v->lock, flags);
	if (want != v->on) {
		gpiod_set_value_cansleep(v->gpio, want);
		v->on = want;
		if (want)
			v->on_count++;
		dev_dbg(v->dev, "A6L_VIB %s\n", want ? "on" : "off");
	}
	mutex_unlock(&v->apply_lock);
}

static void a6l_vib_work(struct work_struct *work)
{
	a6l_vib_apply(container_of(work, struct a6l_vib, work));
}

static int a6l_vib_type_idx(u16 type)
{
	switch (type) {
	case FF_RUMBLE: return 0;
	case FF_PERIODIC: return 1;
	case FF_CONSTANT: return 2;
	default: return -1;
	}
}

/* ff-memless play callback: called in atomic context with the COMBINED effect of one type (union zeroed first). */
static int a6l_vib_play(struct input_dev *dev, void *data, struct ff_effect *effect)
{
	struct a6l_vib *v = input_get_drvdata(dev);
	int idx = a6l_vib_type_idx(effect->type);
	bool on = memchr_inv(&effect->u, 0, sizeof(effect->u)) != NULL;
	unsigned long flags;

	if (idx < 0)
		return 0;
	spin_lock_irqsave(&v->lock, flags);
	if (on)
		__set_bit(idx, &v->active_types);
	else
		__clear_bit(idx, &v->active_types);
	spin_unlock_irqrestore(&v->lock, flags);
	schedule_work(&v->work);
	return 0;
}

static void a6l_vib_close(struct input_dev *input)
{
	struct a6l_vib *v = input_get_drvdata(input);
	unsigned long flags;

	spin_lock_irqsave(&v->lock, flags);
	v->active_types = 0;
	spin_unlock_irqrestore(&v->lock, flags);
	cancel_work_sync(&v->work);
	a6l_vib_apply(v);
}

static void a6l_vib_pulse_off(struct work_struct *work)
{
	struct a6l_vib *v = container_of(to_delayed_work(work), struct a6l_vib, pulse_off);
	unsigned long flags;

	spin_lock_irqsave(&v->lock, flags);
	v->pulse = false;
	spin_unlock_irqrestore(&v->lock, flags);
	a6l_vib_apply(v);
	dev_info(v->dev, "A6L_VIB pulse end (gpio=%d)\n", gpiod_get_value_cansleep(v->gpio));
}

static ssize_t a6l_vib_pulse_write(struct file *f, const char __user *ubuf, size_t len, loff_t *ppos)
{
	struct a6l_vib *v = file_inode(f)->i_private;
	unsigned int ms;
	unsigned long flags;
	int ret;

	ret = kstrtouint_from_user(ubuf, len, 0, &ms);
	if (ret)
		return ret;
	if (ms < 1 || ms > 2000)
		return -EINVAL;
	cancel_delayed_work_sync(&v->pulse_off);
	spin_lock_irqsave(&v->lock, flags);
	v->pulse = true;
	spin_unlock_irqrestore(&v->lock, flags);
	a6l_vib_apply(v);
	dev_info(v->dev, "A6L_VIB pulse %u ms (gpio=%d)\n", ms, gpiod_get_value_cansleep(v->gpio));
	schedule_delayed_work(&v->pulse_off, msecs_to_jiffies(ms));
	return len;
}

static const struct file_operations a6l_vib_pulse_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.write = a6l_vib_pulse_write,
};

/* ---- F49: LED "transient"-style interface (/sys/class/leds/vibrator) ---- */
static struct a6l_vib *led_to_vib(struct device *dev)
{
	return container_of(dev_get_drvdata(dev), struct a6l_vib, led);
}

static void a6l_vib_timed_off(struct work_struct *work)
{
	struct a6l_vib *v = container_of(to_delayed_work(work), struct a6l_vib, timed_off);
	unsigned long flags;

	spin_lock_irqsave(&v->lock, flags);
	v->timed = false;
	spin_unlock_irqrestore(&v->lock, flags);
	a6l_vib_apply(v);
}

/* stop an LED activation now (activate=0, brightness 0, remove, new activation) */
static void a6l_vib_timed_stop(struct a6l_vib *v)
{
	unsigned long flags;

	cancel_delayed_work_sync(&v->timed_off);
	spin_lock_irqsave(&v->lock, flags);
	v->timed = false;
	spin_unlock_irqrestore(&v->lock, flags);
	a6l_vib_apply(v);
}

static ssize_t activate_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct a6l_vib *v = led_to_vib(dev);

	return sysfs_emit(buf, "%d\n", READ_ONCE(v->timed));
}

static ssize_t activate_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t len)
{
	struct a6l_vib *v = led_to_vib(dev);
	unsigned int act, ms, state;
	unsigned long flags;
	int ret;

	ret = kstrtouint(buf, 0, &act);
	if (ret)
		return ret;
	if (act > 1)
		return -EINVAL;
	mutex_lock(&v->led_lock);
	a6l_vib_timed_stop(v);		/* a new activation replaces the previous one; 0 = stop */
	ms = min(READ_ONCE(v->led_duration), (unsigned int)A6L_VIB_MAX_MS);
	state = READ_ONCE(v->led_state);
	/* transient with state 0 = "off for duration": the motor just stays off */
	if (act && ms && state) {
		spin_lock_irqsave(&v->lock, flags);
		if (v->removing)
			ret = -ENODEV;
		else
			v->timed = true;
		spin_unlock_irqrestore(&v->lock, flags);
		if (!ret) {
			a6l_vib_apply(v);
			schedule_delayed_work(&v->timed_off, msecs_to_jiffies(ms));
		}
	}
	mutex_unlock(&v->led_lock);
	return ret ? ret : len;
}

static ssize_t duration_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%u\n", READ_ONCE(led_to_vib(dev)->led_duration));
}

static ssize_t duration_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t len)
{
	unsigned int ms;
	int ret = kstrtouint(buf, 0, &ms);

	if (ret)
		return ret;
	WRITE_ONCE(led_to_vib(dev)->led_duration, ms);	/* applied (capped) at the next activate */
	return len;
}

static ssize_t state_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%u\n", READ_ONCE(led_to_vib(dev)->led_state));
}

static ssize_t state_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t len)
{
	unsigned int st;
	int ret = kstrtouint(buf, 0, &st);

	if (ret)
		return ret;
	WRITE_ONCE(led_to_vib(dev)->led_state, st ? 1 : 0);	/* binary motor: any non-zero level = on */
	return len;
}

static DEVICE_ATTR_RW(activate);
static DEVICE_ATTR_RW(duration);
static DEVICE_ATTR_RW(state);

static struct attribute *a6l_vib_led_attrs[] = {
	&dev_attr_activate.attr,
	&dev_attr_duration.attr,
	&dev_attr_state.attr,
	NULL
};
ATTRIBUTE_GROUPS(a6l_vib_led);

/* brightness: 0 stops an activation; a non-zero level without a duration is refused (no untimed on) */
static int a6l_vib_led_set(struct led_classdev *cdev, enum led_brightness b)
{
	struct a6l_vib *v = container_of(cdev, struct a6l_vib, led);

	if (b)
		return -EOPNOTSUPP;
	mutex_lock(&v->led_lock);
	a6l_vib_timed_stop(v);
	mutex_unlock(&v->led_lock);
	return 0;
}

static int a6l_vib_probe(struct platform_device *pdev)
{
	struct a6l_vib *v;
	int ret;

	v = devm_kzalloc(&pdev->dev, sizeof(*v), GFP_KERNEL);
	if (!v)
		return -ENOMEM;
	v->dev = &pdev->dev;
	spin_lock_init(&v->lock);
	INIT_WORK(&v->work, a6l_vib_work);
	INIT_DELAYED_WORK(&v->pulse_off, a6l_vib_pulse_off);
	INIT_DELAYED_WORK(&v->timed_off, a6l_vib_timed_off);
	mutex_init(&v->apply_lock);
	mutex_init(&v->led_lock);

	/* stock: direction output, raw low at probe */
	v->gpio = devm_gpiod_get(&pdev->dev, "enable", GPIOD_OUT_LOW);
	if (IS_ERR(v->gpio))
		return dev_err_probe(&pdev->dev, PTR_ERR(v->gpio), "A6L_VIB no enable-gpios\n");

	v->input = devm_input_allocate_device(&pdev->dev);
	if (!v->input)
		return -ENOMEM;
	v->input->name = "a6l_gpio_vibrator";
	v->input->id.bustype = BUS_HOST;
	v->input->id.vendor = 0x2a6c;	/* same pseudo-vendor as the other A6L input devices */
	v->input->id.product = 0x0f01;
	v->input->close = a6l_vib_close;
	input_set_drvdata(v->input, v);
	input_set_capability(v->input, EV_FF, FF_RUMBLE);
	input_set_capability(v->input, EV_FF, FF_CONSTANT);
	input_set_capability(v->input, EV_FF, FF_PERIODIC);
	input_set_capability(v->input, EV_FF, FF_SQUARE);
	input_set_capability(v->input, EV_FF, FF_SINE);

	ret = input_ff_create_memless(v->input, NULL, a6l_vib_play);
	if (ret)
		return dev_err_probe(&pdev->dev, ret, "A6L_VIB ff create\n");
	ret = input_register_device(v->input);
	if (ret)
		return dev_err_probe(&pdev->dev, ret, "A6L_VIB input register\n");

	platform_set_drvdata(pdev, v);
	/* F49: VibratorOL LedVibratorDevice interface; registered last, unregistered first in remove() */
	v->led.name = "vibrator";
	v->led.max_brightness = 1;
	v->led.brightness_set_blocking = a6l_vib_led_set;
	v->led.groups = a6l_vib_led_groups;
	ret = led_classdev_register(&pdev->dev, &v->led);
	if (ret)
		return dev_err_probe(&pdev->dev, ret, "A6L_VIB led register\n");
	v->dbg = debugfs_create_dir("a6l_gpio_vib", NULL);
	debugfs_create_file("pulse", 0200, v->dbg, v, &a6l_vib_pulse_fops);
	debugfs_create_ulong("on_count", 0400, v->dbg, &v->on_count);
	dev_info(&pdev->dev, "A6L_VIB ready: gpio=%d (stock timed-gpio TLMM 79, high = on), input %s, led %s\n",
		 desc_to_gpio(v->gpio), dev_name(&v->input->dev), dev_name(v->led.dev));
	return 0;
}

static void a6l_vib_remove(struct platform_device *pdev)
{
	struct a6l_vib *v = platform_get_drvdata(pdev);
	unsigned long flags;

	led_classdev_unregister(&v->led);	/* no more sysfs activations after this */
	debugfs_remove_recursive(v->dbg);
	cancel_delayed_work_sync(&v->pulse_off);
	spin_lock_irqsave(&v->lock, flags);
	v->removing = true;
	spin_unlock_irqrestore(&v->lock, flags);
	cancel_delayed_work_sync(&v->timed_off);
	spin_lock_irqsave(&v->lock, flags);
	v->timed = false;
	v->pulse = false;
	v->active_types = 0;
	spin_unlock_irqrestore(&v->lock, flags);
	cancel_work_sync(&v->work);
	a6l_vib_apply(v);
	gpiod_set_value_cansleep(v->gpio, 0);
}

static int a6l_vib_suspend(struct device *dev)
{
	struct a6l_vib *v = dev_get_drvdata(dev);
	unsigned long flags;

	spin_lock_irqsave(&v->lock, flags);
	v->suspended = true;
	spin_unlock_irqrestore(&v->lock, flags);
	cancel_work_sync(&v->work);
	a6l_vib_apply(v);
	return 0;
}

static int a6l_vib_resume(struct device *dev)
{
	struct a6l_vib *v = dev_get_drvdata(dev);
	unsigned long flags;

	spin_lock_irqsave(&v->lock, flags);
	v->suspended = false;
	spin_unlock_irqrestore(&v->lock, flags);
	a6l_vib_apply(v);
	return 0;
}

static DEFINE_SIMPLE_DEV_PM_OPS(a6l_vib_pm, a6l_vib_suspend, a6l_vib_resume);

static const struct of_device_id a6l_vib_of_match[] = {
	{ .compatible = "hisense,a6l-gpio-vibrator" },
	{ }
};
MODULE_DEVICE_TABLE(of, a6l_vib_of_match);

static struct platform_driver a6l_vib_driver = {
	.probe = a6l_vib_probe,
	.remove = a6l_vib_remove,
	.driver = {
		.name = "a6l-gpio-vibrator",
		.of_match_table = a6l_vib_of_match,
		.pm = pm_sleep_ptr(&a6l_vib_pm),
	},
};
module_platform_driver(a6l_vib_driver);

MODULE_DESCRIPTION("Hisense A6L GPIO79 vibrator (stock timed-gpio equivalent): LED 'vibrator' (VibratorOL) + input FF device");
MODULE_LICENSE("GPL");
