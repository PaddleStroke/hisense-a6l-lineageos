/* Host stubs of the few kernel APIs q6voice-common.c uses (r5 bug hunt round2 audio SSR test). */
#ifndef A6L_KSTUB_H
#define A6L_KSTUB_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32;
#define container_of(p, t, m) ((t *)((char *)(p) - offsetof(t, m)))
#define EXPORT_SYMBOL_GPL(x)
#define MODULE_AUTHOR(x)
#define MODULE_DESCRIPTION(x)
#define MODULE_LICENSE(x)
#define GFP_KERNEL 0
#define ERR_PTR(e) ((void *)(intptr_t)(e))
#define PTR_ERR(p) ((long)(intptr_t)(p))
#define IS_ERR(p) ((uintptr_t)(p) >= (uintptr_t)-4095)
/* device + devres (devm memory freed at unbind, like the kernel) */
struct devres { struct devres *next; void *p; };
struct device { int refs; bool released; void *drvdata; struct devres *devres; void (*release)(struct device *); };
static inline void *dev_get_drvdata(const struct device *d) { return d->drvdata; }
static inline void dev_set_drvdata(struct device *d, void *p) { d->drvdata = p; }
static inline struct device *get_device(struct device *d) { d->refs++; return d; }
static inline void put_device(struct device *d) { if (--d->refs == 0 && d->release) d->release(d); }
static inline void *kzalloc(size_t n, int f) { (void)f; return calloc(1, n); }
static inline void kfree(const void *p) { free((void *)p); }
static inline void *devm_kzalloc(struct device *d, size_t n, int f)
{
	struct devres *r = calloc(1, sizeof(*r));
	(void)f;
	r->p = calloc(1, n);
	r->next = d->devres;
	d->devres = r;
	return r->p;
}
static inline void devres_release_all(struct device *d)
{
	while (d->devres) { struct devres *r = d->devres; d->devres = r->next; free(r->p); free(r); }
}
#define dev_dbg(d, ...) do { (void)(d); } while (0)
#define dev_warn(d, ...) do { (void)(d); fprintf(stderr, __VA_ARGS__); } while (0)
#define dev_err(d, ...) do { (void)(d); fprintf(stderr, __VA_ARGS__); } while (0)
/* locks: single-threaded test, but the rwsem records misuse */
typedef struct { int x; } spinlock_t;
#define DEFINE_SPINLOCK(n) spinlock_t n
#define spin_lock_init(l) ((void)(l))
#define spin_lock_irqsave(l, f) do { (void)(l); (f) = 0; } while (0)
#define spin_unlock_irqrestore(l, f) do { (void)(l); (void)(f); } while (0)
struct rw_semaphore { int readers, writer; };
static inline void init_rwsem(struct rw_semaphore *s) { s->readers = s->writer = 0; }
static inline void down_read(struct rw_semaphore *s) { if (s->writer) abort(); s->readers++; }
static inline void up_read(struct rw_semaphore *s) { s->readers--; }
static inline void down_write(struct rw_semaphore *s) { if (s->readers || s->writer) abort(); s->writer = 1; }
static inline void up_write(struct rw_semaphore *s) { s->writer = 0; }
struct kref { int refcount; };
static inline void kref_init(struct kref *k) { k->refcount = 1; }
static inline void kref_get(struct kref *k) { k->refcount++; }
static inline int kref_put(struct kref *k, void (*rel)(struct kref *)) { if (--k->refcount == 0) { rel(k); return 1; } return 0; }
typedef struct { int x; } wait_queue_head_t;
#define init_waitqueue_head(w) ((void)(w))
#define wake_up(w) ((void)(w))
#define msecs_to_jiffies(m) (m)
/* the stub APR replies synchronously, so the condition is already true or never will be */
#define wait_event_timeout(w, cond, t) ((void)(t), (cond) ? 1 : 0)
#endif
