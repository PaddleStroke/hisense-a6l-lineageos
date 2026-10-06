// SPDX-License-Identifier: Apache-2.0
/* eink-round4 host test: which plane-set changes discard a capture (capture_plane_snapshot.h a6l_plane_layout_equal).
 * 6 Oct 17:01 regression of round 3 (0014): any plane change discarded the capture, so a continuously redrawing status
 * bar layer starved the mirror for 15-45 s. Now only a change of the LCD layout counts; content flips (FB_ID) of a plane,
 * and anything on planes that are not on the LCD CRTC (the leased e-ink plane), do not.
 * cc $(pkg-config --cflags libdrm) -o t test_capture_guard.c */
#include <stdio.h>
#include "../src/capture_plane_snapshot.h"

static int fails, checks;
#define CHECK(c, what) do { checks++; if (c) printf("ok   %s\n", what); else { printf("FAIL %s\n", what); fails++; } } while (0)

enum { LCD = 77, EINK = 88 };
static void plane(struct a6l_plane_snapshot *s, unsigned i, uint32_t id, uint32_t crtc, uint32_t fb, int y, int h, int z) {
    struct a6l_object_tuple *p = &s->planes[i];
    memset(p, 0, sizeof *p); p->id = id; p->present = (1u << PF_N) - 1;
    p->v[PF_FB] = fb; p->v[PF_CRTC] = crtc; p->v[PF_CY] = (uint64_t)y; p->v[PF_CW] = 1080; p->v[PF_CH] = (uint64_t)h;
    p->v[PF_SW] = 1080ull << 16; p->v[PF_SH] = (uint64_t)h << 16; p->v[PF_ZPOS] = (uint64_t)z; p->v[PF_ROT] = 1; p->v[PF_ALPHA] = 0xffff;
}
static void base(struct a6l_plane_snapshot *s) {
    memset(s, 0, sizeof *s); s->width = 1080; s->height = 2340; s->count = 5;
    s->crtc.id = LCD; s->crtc.present = (1u << PF_ACTIVE) | (1u << PF_MODE); s->crtc.v[PF_ACTIVE] = 1; s->crtc.v[PF_MODE] = 5;
    plane(s, 0, 31, LCD, 100, 0, 2340, 0);	/* launcher / wallpaper */
    plane(s, 1, 32, LCD, 200, 0, 2340, 1);	/* app */
    plane(s, 2, 33, LCD, 300, 0, 75, 2);	/* status bar (the "changed plane 2" of the 17:01 log) */
    plane(s, 3, 34, EINK, 400, 0, 725, 0);	/* leased e-ink primary: a6l_epdd flips it at 85 Hz during a drive */
    plane(s, 4, 35, 0, 0, 0, 0, 0);		/* unused */
}

int main(void) {
    struct a6l_plane_snapshot a, b;
    base(&a); base(&b);
    CHECK(a6l_plane_layout_equal(&a, &b, LCD), "identical plane sets: same layout");
    b.planes[2].v[PF_FB] = 301;
    CHECK(a6l_plane_layout_equal(&a, &b, LCD), "status bar flipped its buffer (FB_ID only): same layout, capture kept");
    CHECK(!a6l_plane_snapshot_equal(&a, &b), "  ... (round 3's whole-set comparison discarded exactly this)");
    base(&b); b.planes[3].v[PF_FB] = 401;
    CHECK(a6l_plane_layout_equal(&a, &b, LCD), "e-ink lessee plane flipped: ignored");
    base(&b); b.planes[3].v[PF_CRTC] = 0; b.planes[3].v[PF_FB] = 0;
    CHECK(a6l_plane_layout_equal(&a, &b, LCD), "e-ink CRTC switched off: ignored");
    base(&b); b.planes[1].v[PF_CY] = 900; b.planes[1].v[PF_CH] = 1440;
    CHECK(!a6l_plane_layout_equal(&a, &b, LCD), "app plane moved/resized (shade or IME frame): layout change");
    base(&b); plane(&b, 4, 35, LCD, 500, 0, 1200, 3);
    CHECK(!a6l_plane_layout_equal(&a, &b, LCD), "a new plane appeared on the LCD (shade pulled): layout change");
    base(&b); b.planes[2].v[PF_CRTC] = 0; b.planes[2].v[PF_FB] = 0;
    CHECK(!a6l_plane_layout_equal(&a, &b, LCD), "a plane left the LCD: layout change");
    base(&b); b.planes[1].v[PF_ALPHA] = 0x8000;
    CHECK(!a6l_plane_layout_equal(&a, &b, LCD), "plane alpha changed (fade): layout change");
    base(&b); b.planes[0].v[PF_ZPOS] = 5;
    CHECK(!a6l_plane_layout_equal(&a, &b, LCD), "z-order changed: layout change");
    base(&b); b.crtc.v[PF_ACTIVE] = 0;
    CHECK(!a6l_plane_layout_equal(&a, &b, LCD), "LCD CRTC state changed: layout change");
    base(&b); b.height = 1080;
    CHECK(!a6l_plane_layout_equal(&a, &b, LCD), "display size changed: layout change");
    printf("A6L_CAPTURE_GUARD_TEST %s (%d checks)\n", fails ? "FAIL" : "PASS", checks);
    return fails != 0;
}
