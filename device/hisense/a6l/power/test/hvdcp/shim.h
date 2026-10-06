/* SPDX-License-Identifier: GPL-2.0-only */
/* A6L power28 (28 Sep 2026): userspace shim so drivers/power/supply/qcom_smbx.c (A6L-patched) compiles on the host
 * against a fake PM660 SMB2 register file + QC adapter model (sim-smb2.c). Only what qcom_smbx.c uses. */
#ifndef A6L_SHIM_H
#define A6L_SHIM_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <errno.h>
#include <assert.h>

typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef int64_t s64;
#define BIT(n) (1U << (n))
#define GENMASK(h, l) (((~0U) << (l)) & (~0U >> (31 - (h))))
#define container_of(p, t, m) ((t *)((char *)(p) - offsetof(t, m)))
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define min_t(t, a, b) ((t)(a) < (t)(b) ? (t)(a) : (t)(b))
#define max_t(t, a, b) ((t)(a) > (t)(b) ? (t)(a) : (t)(b))
#define clamp_t(t, v, lo, hi) min_t(t, max_t(t, v, lo), hi)
#define READ_ONCE(x) (x)
#define MAX_ERRNO 4095
#define IS_ERR(p) ((unsigned long)(p) >= (unsigned long)-MAX_ERRNO)
#define PTR_ERR(p) ((long)(p))
#define ERR_PTR(e) ((void *)(long)(e))
#define GFP_KERNEL 0
#define __init
#define __exit

/* logging */
extern char sim_log[1 << 20]; extern size_t sim_log_len; extern unsigned long jiffies;
void sim_logf(const char *lvl, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
struct device { struct device *parent; };
#define dev_err(d, ...) sim_logf("E", __VA_ARGS__)
#define dev_warn(d, ...) sim_logf("W", __VA_ARGS__)
#define dev_info(d, ...) sim_logf("I", __VA_ARGS__)
#define dev_dbg(d, ...) do { } while (0)
#define pr_err(...) sim_logf("E", __VA_ARGS__)
static inline int dev_err_probe(struct device *d, int rc, const char *fmt, ...) { (void)d; sim_logf("E", "probe error %d: %s", rc, fmt); return rc; }
static inline int sysfs_emit(char *buf, const char *fmt, ...) { va_list a; int n; va_start(a, fmt); n = vsnprintf(buf, 4096, fmt, a); va_end(a); return n; }
static inline int sysfs_emit_at(char *buf, int at, const char *fmt, ...) { va_list a; int n; assert(at >= 0 && at < 4096); va_start(a, fmt); n = vsnprintf(buf + at, 4096 - at, fmt, a); va_end(a); return n < 4096 - at ? n : 4095 - at; }
static inline int kstrtobool(const char *s, bool *r) { if (!s || !s[0]) return -EINVAL; switch (s[0]) { case '1': case 'y': case 'Y': *r = true; return 0; case '0': case 'n': case 'N': *r = false; return 0; } return -EINVAL; }
static inline size_t strscpy(char *d, const char *s, size_t n) { snprintf(d, n, "%s", s); return strlen(d); }

/* time */
#define HZ 1000
static inline unsigned long msecs_to_jiffies(unsigned int ms) { return ms; }
#define time_after(a, b) ((long)((b) - (a)) < 0)
#define time_before(a, b) time_after(b, a)
void msleep(unsigned int ms);

/* locking */
struct mutex { int locked; };
#define DEFINE_MUTEX(n) struct mutex n = { 0 }
static inline void mutex_init(struct mutex *m) { m->locked = 0; }
static inline void mutex_lock(struct mutex *m) { assert(!m->locked && "mutex recursion/deadlock"); m->locked = 1; }
static inline void mutex_unlock(struct mutex *m) { assert(m->locked); m->locked = 0; }

/* work */
struct work_struct { int dummy; };
struct delayed_work { struct work_struct work; void (*fn)(struct work_struct *); unsigned long due; int pending; };
struct workqueue_struct; extern struct workqueue_struct *system_wq;
void sim_register_work(struct delayed_work *dw);
static inline bool mod_delayed_work(struct workqueue_struct *wq, struct delayed_work *dw, unsigned long d) { (void)wq; dw->due = jiffies + d; dw->pending = 1; return true; }
static inline bool schedule_delayed_work(struct delayed_work *dw, unsigned long d) { if (dw->pending) return false; dw->due = jiffies + d; dw->pending = 1; return true; }
static inline bool cancel_delayed_work_sync(struct delayed_work *dw) { int p = dw->pending; dw->pending = 0; return p; }

/* device model */
struct fwnode_handle; struct of_device_id { const char *compatible; const void *data; };
struct platform_device { struct device dev; const char *name; void *drvdata; };
struct platform_driver { int (*probe)(struct platform_device *); void (*shutdown)(struct platform_device *); struct { const char *name; const struct of_device_id *of_match_table; } driver; };
#define to_platform_device(d) container_of(d, struct platform_device, dev)
static inline void platform_set_drvdata(struct platform_device *p, void *d) { p->drvdata = d; }
static inline void *platform_get_drvdata(struct platform_device *p) { return p->drvdata; }
static inline void *devm_kzalloc(struct device *d, size_t n, int f) { (void)d; (void)f; return calloc(1, n); }
static inline char *devm_kasprintf(struct device *d, int f, const char *fmt, const char *s) { char *b = malloc(64); (void)d; (void)f; snprintf(b, 64, fmt, s); return b; }
static inline const void *device_get_match_data(struct device *d) { (void)d; return "pm660"; }
static inline struct fwnode_handle *dev_fwnode(struct device *d) { (void)d; return NULL; }
static inline int device_property_read_u32(struct device *d, const char *n, unsigned int *v) { (void)d; (void)n; *v = 0x1000; return 0; }
int devm_add_action_or_reset(struct device *d, void (*fn)(void *), void *data);
int devm_delayed_work_autocancel(struct device *d, struct delayed_work *w, void (*fn)(struct work_struct *));
#define MODULE_DEVICE_TABLE(a, b)
#define module_platform_driver(d)
#define MODULE_AUTHOR(x)
#define MODULE_DESCRIPTION(x)
#define MODULE_LICENSE(x)
#define MODULE_PARM_DESC(a, b)
#define module_param(n, t, p)

/* module params */
struct kernel_param { void *arg; };
struct kernel_param_ops { int (*set)(const char *, const struct kernel_param *); int (*get)(char *, const struct kernel_param *); };
#define module_param_cb(n, ops, argp, perm) const struct kernel_param __param_##n = { .arg = (void *)(argp) }; const struct kernel_param_ops *__param_ops_##n = (ops)
static inline int param_set_bool(const char *v, const struct kernel_param *kp) { *(bool *)kp->arg = (v[0] == '1' || v[0] == 'y' || v[0] == 'Y'); return 0; }
static inline int param_get_bool(char *b, const struct kernel_param *kp) { return sprintf(b, "%c\n", *(bool *)kp->arg ? 'Y' : 'N'); }
static inline int param_set_uint(const char *v, const struct kernel_param *kp) { *(unsigned int *)kp->arg = strtoul(v, NULL, 0); return 0; }
static inline int param_get_uint(char *b, const struct kernel_param *kp) { return sprintf(b, "%u\n", *(unsigned int *)kp->arg); }

/* irq */
typedef int irqreturn_t;
#define IRQ_HANDLED 1
#define IRQF_ONESHOT 0
static inline int platform_get_irq_byname(struct platform_device *p, const char *n) { (void)p; (void)n; return 42; }
static inline int devm_request_threaded_irq(struct device *d, int irq, void *h, irqreturn_t (*fn)(int, void *), int f, const char *n, void *data) { (void)d; (void)irq; (void)h; (void)fn; (void)f; (void)n; (void)data; return 0; }
static inline int devm_device_init_wakeup(struct device *d) { (void)d; return 0; }
static inline int devm_pm_set_wake_irq(struct device *d, int irq) { (void)d; (void)irq; return 0; }

/* regmap */
struct regmap;
struct regmap *dev_get_regmap(struct device *d, const char *n);
int regmap_read(struct regmap *m, unsigned int reg, unsigned int *val);
int regmap_write(struct regmap *m, unsigned int reg, unsigned int val);
int regmap_update_bits(struct regmap *m, unsigned int reg, unsigned int mask, unsigned int val);
int regmap_bulk_read(struct regmap *m, unsigned int reg, void *val, size_t n);

/* iio */
struct iio_channel { int which; };
struct iio_channel *devm_iio_channel_get(struct device *d, const char *name);
int iio_read_channel_processed(struct iio_channel *c, int *val);

/* power supply */
enum power_supply_property { POWER_SUPPLY_PROP_STATUS, POWER_SUPPLY_PROP_HEALTH, POWER_SUPPLY_PROP_ONLINE, POWER_SUPPLY_PROP_CURRENT_MAX,
	POWER_SUPPLY_PROP_CURRENT_NOW, POWER_SUPPLY_PROP_VOLTAGE_NOW, POWER_SUPPLY_PROP_MANUFACTURER, POWER_SUPPLY_PROP_MODEL_NAME,
	POWER_SUPPLY_PROP_USB_TYPE };
enum { POWER_SUPPLY_STATUS_UNKNOWN, POWER_SUPPLY_STATUS_CHARGING, POWER_SUPPLY_STATUS_DISCHARGING, POWER_SUPPLY_STATUS_NOT_CHARGING, POWER_SUPPLY_STATUS_FULL };
enum { POWER_SUPPLY_HEALTH_GOOD = 1, POWER_SUPPLY_HEALTH_OVERHEAT, POWER_SUPPLY_HEALTH_OVERVOLTAGE, POWER_SUPPLY_HEALTH_COLD, POWER_SUPPLY_HEALTH_WARM, POWER_SUPPLY_HEALTH_COOL };
enum { POWER_SUPPLY_USB_TYPE_UNKNOWN, POWER_SUPPLY_USB_TYPE_SDP, POWER_SUPPLY_USB_TYPE_DCP, POWER_SUPPLY_USB_TYPE_CDP };
enum { POWER_SUPPLY_TYPE_USB = 4 };
union power_supply_propval { int intval; const char *strval; };
struct power_supply_battery_info { int voltage_max_design_uv; int constant_charge_current_max_ua; };
struct power_supply;
struct power_supply_desc { const char *name; int type; unsigned int usb_types; enum power_supply_property *properties; size_t num_properties;
	int (*get_property)(struct power_supply *, enum power_supply_property, union power_supply_propval *);
	int (*set_property)(struct power_supply *, enum power_supply_property, const union power_supply_propval *);
	int (*property_is_writeable)(struct power_supply *, enum power_supply_property); };
struct power_supply_config { void *drv_data; struct fwnode_handle *fwnode; };
struct power_supply { const struct power_supply_desc *desc; void *drv; };
static inline void *power_supply_get_drvdata(struct power_supply *p) { return p->drv; }
static inline int power_supply_get_property(struct power_supply *p, enum power_supply_property pp, union power_supply_propval *v) { return p->desc->get_property(p, pp, v); }
static inline void power_supply_changed(struct power_supply *p) { (void)p; }
struct power_supply *devm_power_supply_register(struct device *d, const struct power_supply_desc *desc, const struct power_supply_config *c);
int power_supply_get_battery_info(struct power_supply *p, struct power_supply_battery_info **bi);
#endif
