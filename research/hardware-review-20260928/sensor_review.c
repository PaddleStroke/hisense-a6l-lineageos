/* Offline fault injection into the actual HAL poll loop. No phone access.
 * r5 review fixes (29 Sep 2026): converted from reproducing F4 to asserting the corrected behavior. */
#define _GNU_SOURCE
#include <poll.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
static jmp_buf escape_poll;
static int observed_timeout;
static nfds_t observed_nfds;
static int review_poll(struct pollfd *fds, nfds_t n, int timeout)
{
    (void)fds;
    observed_nfds = n;
    observed_timeout = timeout;
    longjmp(escape_poll, 1);
}
#define poll review_poll
#include "sensors_a6l.c"
#undef poll

int main(void)
{
    /* The sysroot is an empty temporary directory supplied by reproduce.py. */
    struct a6l_motion motion;
    a6l_motion_init(&motion);
    int rc = a6l_motion_activate(&motion, A6L_H_ACCEL, 1);
    int retry = a6l_motion_reconcile(&motion);
    printf("LATE_START activate=%d enabled=%d retry_ms=%d\n", rc,
           motion.h[0].enabled, retry);
    /* fixed: the request is remembered and reconcile schedules the 2 s retry */
    assert(rc == 0 && motion.h[0].enabled && retry > 0 && retry <= 2001);
    a6l_motion_close(&motion);

    /* Directory FD injects EISDIR through the same error branch as ENODEV/EIO.
     * Only poll() is intercepted, after the real reconcile/service/fds path. */
    struct a6l_ctx *c = calloc(1, sizeof(*c));
    assert(c);
    assert(!pipe2(c->wake, O_CLOEXEC | O_NONBLOCK));
    pthread_mutex_init(&c->lock, NULL);
    c->evfd = -1;
    a6l_motion_init(&c->m);
    c->m.h[0].enabled = 1;
    c->m.d[0].on = 1;
    c->m.d[0].fd = open(getenv("A6L_SYSROOT"), O_RDONLY | O_DIRECTORY);
    assert(c->m.d[0].fd >= 0);
    c->m.d[0].rec = 16;
    c->m.d[0].watermark = 1;
    sensors_event_t out[4];
    if (!setjmp(escape_poll))
        a6l_poll((struct sensors_poll_device_t *)&c->dev, out, 4);
    printf("READ_FAILURE retry_pending=%d poll_nfds=%lu poll_timeout=%d\n",
           c->m.d[0].retry_ns > 0, (unsigned long)observed_nfds, observed_timeout);
    /* fixed: the retry scheduled by the read error bounds the poll (1 s), no infinite wait */
    assert(c->m.d[0].retry_ns > 0 && observed_nfds == 1 && observed_timeout >= 0 && observed_timeout <= 1001);
    a6l_motion_close(&c->m);
    close(c->wake[0]); close(c->wake[1]);
    pthread_mutex_destroy(&c->lock);
    free(c);
    puts("FIXED both sensor readiness/recovery defects (F4): late node retried, read-error retry bounds poll");
    return 0;
}
