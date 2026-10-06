/* Host stubs of the kernel APIs q6voice.c and q6cvs.c use (btcall host test, 29 Sep 2026). */
#ifndef A6L_KSTUB_BTCALL_H
#define A6L_KSTUB_BTCALL_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32;
#define __packed __attribute__((packed))
#define BIT(n) (1U << (n))
#define EXPORT_SYMBOL_GPL(x)
#define MODULE_AUTHOR(x)
#define MODULE_DESCRIPTION(x)
#define MODULE_LICENSE(x)
#define MODULE_PARM_DESC(a, b)
#define MODULE_DEVICE_TABLE(a, b)
#define module_param(n, t, p)
#define of_match_ptr(p) (p)
#define GFP_KERNEL 0
#define ERR_PTR(e) ((void *)(intptr_t)(e))
#define PTR_ERR(p) ((long)(intptr_t)(p))
#define IS_ERR(p) ((uintptr_t)(p) >= (uintptr_t)-4095)
#define IS_ERR_OR_NULL(p) (!(p) || IS_ERR(p))
struct device { void *drvdata; };
static inline void *kzalloc(size_t n, int f) { (void)f; return calloc(1, n); }
static inline void kfree(const void *p) { free((void *)p); }
static inline void *devm_kzalloc(struct device *d, size_t n, int f) { (void)d; (void)f; return calloc(1, n); }
static inline int devm_add_action(struct device *d, void (*fn)(void *), void *data) { (void)d; (void)fn; (void)data; return 0; }
#define dev_dbg(d, ...) do { (void)(d); } while (0)
#define dev_info(d, ...) do { (void)(d); if (getenv("V")) fprintf(stderr, __VA_ARGS__); } while (0)
#define dev_warn(d, ...) do { (void)(d); fprintf(stderr, __VA_ARGS__); } while (0)
#define dev_err(d, ...) do { (void)(d); if (getenv("V")) fprintf(stderr, __VA_ARGS__); } while (0)
#define pr_info(...) do { if (getenv("V")) fprintf(stderr, __VA_ARGS__); } while (0)
#define pr_warn(...) fprintf(stderr, __VA_ARGS__)
struct kref { int refcount; };
typedef struct { int x; } wait_queue_head_t;
typedef struct { int x; } spinlock_t;
struct mutex { int locked; };
static inline void mutex_init(struct mutex *m) { m->locked = 0; }
static inline void mutex_destroy(struct mutex *m) { (void)m; }
static inline void mutex_lock(struct mutex *m) { if (m->locked) abort(); m->locked = 1; }
static inline void mutex_unlock(struct mutex *m) { if (!m->locked) abort(); m->locked = 0; }
static inline size_t strscpy(char *d, const char *s, size_t n) { snprintf(d, n, "%s", s); return strlen(d); }
#endif
