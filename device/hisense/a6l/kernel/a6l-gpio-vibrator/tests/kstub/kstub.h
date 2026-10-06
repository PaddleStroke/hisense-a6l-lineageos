/* Minimal userspace stand-ins for the kernel APIs a6l_gpio_vib.c uses (r5 review F49 host test, 29 Sep 2026).
 * Work items never run on their own: the test runs them explicitly (kst_run_work / kst_advance), which makes the
 * timing deterministic. Semantics kept where the driver depends on them: cancel_*_sync clears a pending item,
 * schedule_delayed_work does not re-arm an already pending item, led_classdev_unregister sets brightness 0. */
#ifndef KSTUB_H
#define KSTUB_H
#include <linux/input.h>	/* real uapi struct ff_effect via kstub/linux/input.h include_next */
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#ifndef EOPNOTSUPP
#define EOPNOTSUPP 95
#endif
typedef uint16_t u16;
#define container_of(p, t, m) ((t *)((char *)(p) - offsetof(t, m)))
#define READ_ONCE(x) (x)
#define WRITE_ONCE(x, v) ((x) = (v))
#define min(a, b) ((a) < (b) ? (a) : (b))
#define __set_bit(n, p) (*(p) |= 1UL << (n))
#define __clear_bit(n, p) (*(p) &= ~(1UL << (n)))
static inline void *memchr_inv(const void *p, int c, size_t n)
{ const unsigned char *s = p; for (size_t i = 0; i < n; i++) if (s[i] != (unsigned char)c) return (void *)(s + i); return NULL; }
#define THIS_MODULE NULL
#define MODULE_DESCRIPTION(x)
#define MODULE_LICENSE(x)
#define MODULE_DEVICE_TABLE(a, b)
#define IS_ERR(p) ((uintptr_t)(p) >= (uintptr_t)-4095)
#define PTR_ERR(p) ((long)(intptr_t)(p))
#define ATTRIBUTE_UNUSED __attribute__((unused))
/* locks */
typedef struct { int x; } spinlock_t;
#define spin_lock_init(l) ((void)(l))
#define spin_lock_irqsave(l, f) ((void)(l), (f) = 0)
#define spin_unlock_irqrestore(l, f) ((void)(l), (void)(f))
struct mutex { pthread_mutex_t m; };
#define mutex_init(x) pthread_mutex_init(&(x)->m, NULL)
#define mutex_lock(x) pthread_mutex_lock(&(x)->m)
#define mutex_unlock(x) pthread_mutex_unlock(&(x)->m)
/* work */
struct work_struct { void (*fn)(struct work_struct *); int pending; };
struct delayed_work { struct work_struct work; long expires; };
extern long kst_jiffies;	/* 1 jiffy = 1 ms here */
#define INIT_WORK(w, f) ((w)->fn = (f), (w)->pending = 0)
#define INIT_DELAYED_WORK(d, f) ((d)->work.fn = (f), (d)->work.pending = 0, (d)->expires = 0)
#define to_delayed_work(w) container_of(w, struct delayed_work, work)
static inline bool schedule_work(struct work_struct *w) { if (w->pending) return false; w->pending = 1; return true; }
static inline bool cancel_work_sync(struct work_struct *w) { int p = w->pending; w->pending = 0; return p; }
static inline bool schedule_delayed_work(struct delayed_work *d, unsigned long j)
{ if (d->work.pending) return false; d->work.pending = 1; d->expires = kst_jiffies + (long)j; return true; }
static inline bool cancel_delayed_work_sync(struct delayed_work *d) { int p = d->work.pending; d->work.pending = 0; return p; }
#define msecs_to_jiffies(ms) ((unsigned long)(ms))
/* device */
struct device { void *drvdata; const char *name; };
static inline void *dev_get_drvdata(const struct device *d) { return d->drvdata; }
static inline const char *dev_name(const struct device *d) { return d->name; }
#define dev_dbg(d, ...) ((void)(d))
#define dev_info(d, ...) ((void)(d))
static inline int dev_err_probe(const struct device *d, int e, const char *f, ...) { (void)d; (void)f; return e; }
struct platform_device { struct device dev; };
#define platform_set_drvdata(p, v) ((p)->dev.drvdata = (v))
#define platform_get_drvdata(p) ((p)->dev.drvdata)
#define devm_kzalloc(d, n, f) calloc(1, n)
#define GFP_KERNEL 0
/* gpio */
struct gpio_desc { int value; int sets; };
enum gpiod_flags { GPIOD_OUT_LOW };
extern struct gpio_desc kst_gpio;
static inline struct gpio_desc *devm_gpiod_get(struct device *d, const char *n, enum gpiod_flags f)
{ (void)d; (void)n; (void)f; kst_gpio.value = 0; return &kst_gpio; }
static inline void gpiod_set_value_cansleep(struct gpio_desc *g, int v) { g->value = v; g->sets++; }
static inline int gpiod_get_value_cansleep(struct gpio_desc *g) { return g->value; }
static inline int desc_to_gpio(struct gpio_desc *g) { (void)g; return 79; }
/* sysfs */
struct attribute { const char *name; };
struct device_attribute { struct attribute attr;
	ssize_t (*show)(struct device *, struct device_attribute *, char *);
	ssize_t (*store)(struct device *, struct device_attribute *, const char *, size_t); };
struct attribute_group { struct attribute **attrs; };
#define DEVICE_ATTR_RW(n) struct device_attribute dev_attr_##n = { { #n }, n##_show, n##_store }
#define ATTRIBUTE_GROUPS(x) static const struct attribute_group x##_group = { .attrs = x##_attrs }; \
	static const struct attribute_group *x##_groups[] = { &x##_group, NULL }
#define sysfs_emit sprintf
static inline int kstrtouint(const char *s, unsigned int base, unsigned int *res)
{
	char *e;
	unsigned long v;
	if (!*s || *s == '-' || *s == ' ')
		return -EINVAL;
	errno = 0;
	v = strtoul(s, &e, base);
	if (errno || e == s || v > 0xffffffffUL)
		return -EINVAL;
	if (*e == '\n')
		e++;
	if (*e)
		return -EINVAL;
	*res = (unsigned int)v;
	return 0;
}
/* leds */
enum led_brightness { LED_OFF = 0 };
struct led_classdev { const char *name; unsigned int max_brightness;
	int (*brightness_set_blocking)(struct led_classdev *, enum led_brightness);
	const struct attribute_group **groups; struct device *dev; struct device devstore; int registered; };
extern int kst_led_register_err;
static inline int led_classdev_register(struct device *parent, struct led_classdev *c)
{ (void)parent; if (kst_led_register_err) return kst_led_register_err;
  c->devstore.drvdata = c; c->devstore.name = c->name; c->dev = &c->devstore; c->registered = 1; return 0; }
static inline void led_classdev_unregister(struct led_classdev *c)
{ if (c->brightness_set_blocking) c->brightness_set_blocking(c, LED_OFF); c->registered = 0; }
/* input */
struct input_dev { const char *name; struct input_id id; void (*close)(struct input_dev *); void *drvdata; struct device dev;
	int (*play)(struct input_dev *, void *, struct ff_effect *); };
static inline struct input_dev *devm_input_allocate_device(struct device *d) { (void)d; struct input_dev *i = calloc(1, sizeof(*i)); i->dev.name = "input9"; return i; }
#define input_set_drvdata(i, v) ((i)->drvdata = (v))
#define input_get_drvdata(i) ((i)->drvdata)
#define input_set_capability(i, t, c) ((void)(i))
static inline int input_ff_create_memless(struct input_dev *i, void *d, int (*p)(struct input_dev *, void *, struct ff_effect *))
{ (void)d; i->play = p; return 0; }
static inline int input_register_device(struct input_dev *i) { (void)i; return 0; }
/* debugfs */
struct dentry { int x; };
struct inode { void *i_private; };
struct file { struct inode *inode; };
#define file_inode(f) ((f)->inode)
struct file_operations { void *owner; int (*open)(struct inode *, struct file *);
	ssize_t (*write)(struct file *, const char *, size_t, loff_t *); };
static inline int simple_open(struct inode *i, struct file *f) { (void)i; (void)f; return 0; }
#define __user
static inline int kstrtouint_from_user(const char *b, size_t n, unsigned int base, unsigned int *r)
{ char t[32]; if (n >= sizeof(t)) return -EINVAL; memcpy(t, b, n); t[n] = 0; return kstrtouint(t, base, r); }
static inline struct dentry *debugfs_create_dir(const char *n, void *p) { (void)n; (void)p; static struct dentry d; return &d; }
static inline void debugfs_create_file(const char *n, int m, struct dentry *p, void *d, const struct file_operations *f) { (void)n; (void)m; (void)p; (void)d; (void)f; }
static inline void debugfs_create_ulong(const char *n, int m, struct dentry *p, unsigned long *v) { (void)n; (void)m; (void)p; (void)v; }
static inline void debugfs_remove_recursive(struct dentry *d) { (void)d; }
/* pm / of / platform driver */
struct dev_pm_ops { int (*suspend)(struct device *); int (*resume)(struct device *); };
#define DEFINE_SIMPLE_DEV_PM_OPS(n, s, r) const struct dev_pm_ops n = { s, r }
#define pm_sleep_ptr(x) (x)
struct of_device_id { const char *compatible; };
struct device_driver { const char *name; const struct of_device_id *of_match_table; const struct dev_pm_ops *pm; };
struct platform_driver { int (*probe)(struct platform_device *); void (*remove)(struct platform_device *); struct device_driver driver; };
#define module_platform_driver(d) struct platform_driver *kst_driver = &(d)
#endif
