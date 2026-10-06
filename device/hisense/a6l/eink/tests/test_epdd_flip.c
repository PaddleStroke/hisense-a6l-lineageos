// SPDX-License-Identifier: Apache-2.0
/* r5 bug hunt eink-display (E1): a6l_epdd's real flip() with a fake page-flip event stream (drmModePageFlip / poll /
 * drmHandleEvent replaced below; everything else is the unchanged a6l_epdd.c). A page-flip event that arrives later than
 * the 1 s vblank wait must not be taken for the completion of the NEXT flip (that flip would then return one vblank
 * early and the one after it would fail with EBUSY, for ever: one stale event stays queued). */
#define _GNU_SOURCE
#include <poll.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

static int k_pending;		/* the "kernel": a flip is queued and not yet completed */
static int k_events;		/* completion events readable on the fd */
static int k_seq;
static int delay_polls;		/* the next flip completes only after this many poll() calls that time out */
static int n_ebusy;
static int t_pageflip(int fd, uint32_t crtc, uint32_t id, uint32_t flags, void *d) {
    (void)fd; (void)crtc; (void)id; (void)flags; (void)d;
    if (k_pending) { n_ebusy++; errno = EBUSY; return -1; }
    k_pending = 1; return 0;
}
static void t_complete(void) { if (k_pending) { k_pending = 0; k_events++; } }
static int t_poll(struct pollfd *p, nfds_t n, int to) {
    (void)to;
    if (!k_events && k_pending) { if (delay_polls > 0) { delay_polls--; return 0; } t_complete(); }
    if (n) p[0].revents = k_events ? POLLIN : 0;
    return k_events ? 1 : 0;
}
static int t_handle(int fd, drmEventContextPtr ev) {
    (void)fd; if (!k_events) return 0;
    k_events--; ev->page_flip_handler(fd, (unsigned)++k_seq, 0, 0, NULL); return 0;
}
#define drmModePageFlip t_pageflip
#define poll t_poll
#define drmHandleEvent t_handle
#define main a6l_epdd_main
#include "../src/a6l_epdd.c"
#undef main

static int fails;
#define EXPECT(c, what) do { if (c) printf("ok   %s\n", what); else { printf("FAIL %s\n", what); fails++; } } while (0)

int main(void) {
    dfd = 100; crtc = 1;
    EXPECT(flip(1) == 0 && !k_pending && !k_events, "normal flip completes on its own event");
    /* flip 2: its event is late (> the 1 s wait): reported as a failure */
    delay_polls = 1;
    int r = flip(2);
    EXPECT(r != 0, "late event -> vblank timeout reported");
    t_complete();			/* ... the late event arrives afterwards */
    /* the next flips must each wait for their OWN completion (or ask for a DRM reset), never succeed on the stale one */
    int ok3 = flip(3) == 0, own3 = !k_pending && !k_events;
    int ok4 = flip(4) == 0, own4 = !k_pending && !k_events;
    printf("after the timeout: flip3=%s(own event %d) flip4=%s(own event %d) ebusy=%d drm_lost=%d\n", ok3 ? "ok" : "fail", own3, ok4 ? "ok" : "fail", own4, n_ebusy, drm_lost);
    EXPECT(!ok3 || own3, "flip after a timeout does not return on the stale event");
    EXPECT(ok4 && own4 && n_ebusy == 0, "the event stream is back in step (no EBUSY)");
    /* an event that never comes: the next flip must ask for a DRM reset instead of queueing behind it */
    drm_lost = 0; delay_polls = 3; r = flip(5);
    EXPECT(r != 0, "missing event -> timeout");
    r = flip(6);
    EXPECT(r != 0 && drm_lost, "still no event on the next flip -> DRM reset requested (drm_lost)");
    printf("A6L_EPDD_FLIP_TEST %s (%d failures)\n", fails ? "FAIL" : "PASS", fails);
    return fails != 0;
}
