// SPDX-License-Identifier: Apache-2.0
/* Host unit tests for eink_logic.c (mode policy, key state machine, touch mapping). cc -o t test_eink_logic.c ../src/eink_logic.c */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../src/eink_logic.h"

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
static const char *kn(enum pol_kind k) { return k == POL_NONE ? "none" : k == POL_SHOW ? "show" : k == POL_REFRESH ? "refresh" : "clean-show"; }

/* Simulates the mirror: `content` = version of the front picture, `panel` = version last sent; a capture every 0.25 s.
 * moving = content changed since the previous capture; vs_panel = content != panel (frac given by the scenario). */
struct sim { struct pol_state s; double t, busy_until; int busy, content, prev, panel, n_fast, n_quality, n_refresh, n_clear; char log[8192]; };
static void sim_init(struct sim *m, const struct pol_cfg *c) { memset(m, 0, sizeof *m); pol_init(&m->s, c, 0); m->prev = m->panel = -1; }
static struct pol_action sim_tick(struct sim *m, double frac, double busy_s) {
    m->t += 0.25;
    if (m->busy && m->t >= m->busy_until) { m->busy = 0; pol_done(&m->s, m->busy_until); }
    double moving = m->prev != m->content ? frac : 0, vs_panel = m->panel != m->content ? frac : 0;
    m->prev = m->content;
    struct pol_action a = pol_step(&m->s, m->t, moving, vs_panel, m->busy);
    if (a.kind != POL_NONE) {
        pol_sent(&m->s, &a, m->t); m->busy = 1; m->busy_until = m->t + busy_s; m->panel = m->content;
        if (a.kind == POL_REFRESH) m->n_refresh++; else if (!strcmp(a.mode, POL_FASTEST)) m->n_fast++; else m->n_quality++;
        if (a.kind == POL_CLEAN_SHOW) m->n_clear++;
        char b[64]; snprintf(b, sizeof b, "%.2f:%s/%s ", m->t, kn(a.kind), a.mode ? a.mode : "-"); strncat(m->log, b, sizeof m->log - strlen(m->log) - 1);
    }
    return a;
}
static void settle(struct sim *m, int ticks, double frac, double busy_s) { for (int i = 0; i < ticks; i++) sim_tick(m, frac, busy_s); }

static void test_policy_auto(void) {
    struct pol_cfg c; pol_default_cfg(&c); c.clear_every = 3;
    struct sim m; sim_init(&m, &c);
    /* first picture: one quality update once the content has been quiet >= 300 ms, not before */
    struct pol_action a = sim_tick(&m, 1.0, 0.9); CHECK(a.kind == POL_NONE, "first capture waits for quiet (%s)", kn(a.kind));
    a = sim_tick(&m, 1.0, 0.9); CHECK(a.kind == POL_NONE, "quiet 250 ms < 300 ms");
    a = sim_tick(&m, 1.0, 0.9); CHECK(a.kind == POL_SHOW && !strcmp(a.mode, POL_QUALITY), "quality after 500 ms quiet (%s)", kn(a.kind));
    /* static screen: nothing more */
    int before = m.n_quality + m.n_fast + m.n_refresh; settle(&m, 12, 1.0, 0.9);
    CHECK(m.n_quality + m.n_fast + m.n_refresh == before, "static screen: no update (%s)", m.log);
    /* a discrete change (tap -> new screen) -> one quality, no A2 */
    m.content++; settle(&m, 8, 0.3, 0.9);
    CHECK(m.n_quality == 2 && m.n_fast == 0, "discrete change: 1 quality, 0 fast (q=%d f=%d) %s", m.n_quality, m.n_fast, m.log);
    /* scrolling for 3 s: A2 bursts roughly every busy period, none while one is outstanding */
    int q0 = m.n_quality;
    for (int i = 0; i < 12; i++) { m.content++; sim_tick(&m, 0.3, 0.4); }
    CHECK(m.n_fast >= 4 && m.n_fast <= 7, "burst: %d fastest updates in 3 s (%s)", m.n_fast, m.log);
    CHECK(m.n_quality == q0, "no quality update while scrolling");
    CHECK(pol_in_burst(&m.s), "in burst");
    /* the list stops: exactly one clean update (quality of the new content, or refresh if A2 already showed it) */
    int clean0 = m.n_quality + m.n_refresh; settle(&m, 12, 0.3, 0.9);
    CHECK(m.n_quality + m.n_refresh == clean0 + 1, "one clean update after the burst (%d) %s", m.n_quality + m.n_refresh - clean0, m.log);
    CHECK(!pol_in_burst(&m.s), "burst ended");
    /* clear_every = 3: the 3rd clean (quality) update is forced GC16 of the new page */
    for (int i = 0; i < 3; i++) { m.content++; settle(&m, 8, 0.5, 0.9); }
    CHECK(m.n_clear >= 1, "clear every 3rd clean update (%d) %s", m.n_clear, m.log);
    /* burst whose last A2 frame IS the settled picture -> forced refresh (the library skips identical images) */
    struct sim r; sim_init(&r, &c);
    r.content++; settle(&r, 4, 0.5, 0.9);
    for (int i = 0; i < 8; i++) { r.content++; sim_tick(&r, 0.3, 0.2); }
    settle(&r, 10, 0.3, 0.9);
    CHECK(r.n_refresh + r.n_quality >= 2 && strstr(r.log, "fastest") != NULL, "burst then settle: %s", r.log);
}
static void test_policy_rate(void) {
    struct pol_cfg c; pol_default_cfg(&c); c.max_per_min = 5; c.clear_every = 0;
    struct sim m; sim_init(&m, &c);
    for (int i = 0; i < 200; i++) { m.content++; sim_tick(&m, 0.5, 0.1); }	/* 50 s of continuous motion */
    int n = m.n_fast + m.n_quality + m.n_refresh;
    CHECK(n == 5, "rate limit 5/min over 50 s: %d updates", n);
    for (int i = 0; i < 60; i++) { m.content++; sim_tick(&m, 0.5, 0.1); }	/* next minute window */
    n = m.n_fast + m.n_quality + m.n_refresh;
    CHECK(n > 5 && n <= 10, "window resets after 60 s: %d", n);
}
static void test_policy_reading(void) {
    struct pol_cfg c; pol_default_cfg(&c); c.reading = 1; c.reading_refresh_every = 3; c.clear_every = 0;
    struct sim m; sim_init(&m, &c);
    for (int i = 0; i < 12; i++) { m.content++; struct pol_action a = sim_tick(&m, 0.4, 0.9); CHECK(a.kind == POL_NONE, "reading: no update while moving (%s at %.2f)", kn(a.kind), m.t); }
    settle(&m, 6, 0.4, 0.9);
    CHECK(strstr(m.log, "show/quality") != NULL && m.n_quality == 1 && m.n_fast == 0, "reading: first page GC16: %s", m.log);
    for (int k = 0; k < 3; k++) { m.content++; settle(&m, 8, 0.05, 0.9); }
    CHECK(strstr(m.log, "show/reading") != NULL && m.n_quality == 4, "reading: small changes -> reading waveform: %s", m.log);
    settle(&m, 10, 0.05, 0.9);
    CHECK(m.n_refresh == 1, "reading: one forced refresh after 3 partial updates: %s", m.log);
    m.content++; int q = m.n_quality; settle(&m, 8, 0.9, 0.9);
    CHECK(m.n_quality == q + 1 && strstr(m.log + strlen(m.log) - 20, "quality") != NULL, "reading: page turn -> GC16: %s", m.log);
}
static void test_keys(void) {
    struct key_state k; key_init(&k, 800);
    CHECK(key_event(&k, 1, 10.0) == KA_NONE, "down");
    CHECK(key_tick(&k, 10.3) == KA_NONE, "tick short");
    CHECK(key_event(&k, 0, 10.2) == KA_TOGGLE, "short press toggles");
    CHECK(key_event(&k, 1, 20.0) == KA_NONE, "down 2");
    CHECK(key_event(&k, 2, 20.5) == KA_NONE, "autorepeat before long");
    CHECK(key_tick(&k, 20.85) == KA_CLEAR, "long press fires while held");
    CHECK(key_tick(&k, 21.5) == KA_NONE, "long fires once");
    CHECK(key_event(&k, 0, 22.0) == KA_NONE, "release after long: nothing");
    CHECK(key_event(&k, 0, 23.0) == KA_NONE, "spurious up");
    CHECK(key_event(&k, 1, 30.0) == KA_NONE && key_event(&k, 0, 31.0) == KA_CLEAR, "long press with no tick in between -> clear on release");
    CHECK(mode_parse("mirror", 0) == EINK_MIRROR && mode_parse("off", 1) == EINK_OFF && mode_parse("junk", 1) == 1 && mode_parse(NULL, 0) == 0, "mode_parse");
}
static void test_explicit_fast_modes(void) {
    const char *modes[] = {"fast", "fastest"};
    for (int j = 0; j < 2; j++) {
        struct pol_cfg c; pol_default_cfg(&c); c.clear_every = 3;
        CHECK(pol_apply_refresh_mode(&c, modes[j]) == 0, "select %s", modes[j]);
        struct pol_state s; pol_init(&s, &c, 0);
        for (int i = 0; i < 3; i++) {
            double t = 1.0 + i * 2.0;
            struct pol_action a = pol_step(&s, t, 1.0, 0.5, 0);
            CHECK(a.kind == (i == 2 ? POL_CLEAN_SHOW : POL_SHOW) && !strcmp(a.mode, modes[j]),
                  "%s: entry/tap uses selected waveform, periodic GC16 cleanup retained", modes[j]);
            pol_sent(&s, &a, t); pol_done(&s, t + 0.2);
            a = pol_step(&s, t + 1.0, 0, 0, 0);
            CHECK(a.kind == POL_NONE, "%s: static page has no GC16 settle", modes[j]);
        }
        CHECK(pol_step(&s, 10.0, 1, 1, 1).kind == POL_NONE, "%s: busy panel blocks updates", modes[j]);
        CHECK(pol_apply_refresh_mode(&c, "auto") == 0 && !c.fixed_fast, "return to auto restores automatic policy");
    }
    struct pol_cfg c; pol_default_cfg(&c); c.clear_every = 0;
    pol_apply_refresh_mode(&c, "fastest");
    struct pol_state s; pol_init(&s, &c, 0);
    for (int i = 0; i < 90; i++) {
        double t = (i + 1) * 0.5;
        struct pol_action a = pol_step(&s, t, 1, 1, 0);
        CHECK(a.kind == POL_SHOW, "default rate cap must not create a mid-minute pause at update %d", i + 1);
        if (a.kind != POL_NONE) pol_sent(&s, &a, t);
        pol_done(&s, t + 0.2);
    }
}
static void test_tmap(void) {
    int ox, oy, dw, dh; fit_geometry(1080, 2340, 720, 1440, 0, &ox, &oy, &dw, &dh);
    CHECK(ox == 27 && oy == 0 && dw == 665 && dh == 1440, "letterbox 1080x2340 -> %d,%d %dx%d (M1 doc: 27 px bars)", ox, oy, dw, dh);
    struct tmap m = {720, 1440, 0, 0, 0, 720, 1440, ox, oy, dw, dh, 1080, 2340};
    int fx, fy;
    CHECK(tmap_apply(&m, 360, 720, &fx, &fy) && fx >= 535 && fx <= 545 && fy >= 1165 && fy <= 1175, "centre -> %d,%d", fx, fy);
    CHECK(!tmap_apply(&m, 5, 720, &fx, &fy) && fx == 0, "left bar is outside, clamped to x=0 (%d)", fx);
    CHECK(tmap_apply(&m, 27, 0, &fx, &fy) && fx == 0 && fy == 0, "picture top-left -> %d,%d", fx, fy);
    CHECK(tmap_apply(&m, 691, 1439, &fx, &fy) && fx == 1079 && fy == 2339, "picture bottom-right -> %d,%d", fx, fy);
    CHECK(tmap_parse_transform(&m, "swap,invx") == 0 && m.swap_xy && m.inv_x && !m.inv_y, "parse swap,invx");
    CHECK(tmap_parse_transform(&m, "bogus") < 0, "parse rejects junk");
    tmap_parse_transform(&m, "invx,invy");
    CHECK(tmap_apply(&m, 719 - 27, 1439, &fx, &fy) && fx == 0 && fy == 0, "rotated 180: raw (692,1439) -> front top-left %d,%d", fx, fy);
    tmap_parse_transform(&m, "");
    fit_geometry(1080, 2340, 720, 1440, 1, &ox, &oy, &dw, &dh);
    CHECK(ox == 0 && dw == 720 && oy == -60 && dh == 1560, "crop geometry %d,%d %dx%d", ox, oy, dw, dh);
    fit_geometry(1080, 2340, 720, 1440, FIT_STRETCH, &ox, &oy, &dw, &dh);
    CHECK(ox == 0 && oy == 0 && dw == 720 && dh == 1440, "stretch fills all rear pixels without cropping");
    m.img_x = ox; m.img_y = oy; m.img_w = dw; m.img_h = dh;
    CHECK(tmap_apply(&m, 0, 0, &fx, &fy) && fx == 0 && fy == 0, "stretch: whole-panel top-left is usable");
    CHECK(tmap_apply(&m, 719, 1439, &fx, &fy) && fx == 1079 && fy == 2339, "stretch: whole-panel bottom-right is usable");
    CHECK(tmap_apply(&m, 5, 720, &fx, &fy) && fx > 0, "stretch: former white side bar forwards touch");
    fit_geometry(2340, 1080, 1440, 720, FIT_STRETCH, &ox, &oy, &dw, &dh);
    CHECK(ox == 0 && oy == 0 && dw == 1440 && dh == 720, "landscape stretch retains the complete picture");
}
/* r5 review round4 F41: plane rotation / reflection / plane alpha / blend modes (reviewer's 2x2 cases + more) */
static uint8_t fbpx[16 * 4];
static void setpx(int i, int v, int a) { fbpx[4 * i] = fbpx[4 * i + 1] = fbpx[4 * i + 2] = (uint8_t)v; fbpx[4 * i + 3] = (uint8_t)a; }
static int comp(uint8_t *g, int gw, int gh, uint32_t rot, uint32_t alpha16, int blend, int has_alpha, int bg) {
    memset(g, bg, (size_t)gw * gh);
    struct plane_geo q = {0, 0, gw, gh, 0, 0, 2, 2, rot, alpha16, blend};
    struct plane_fb f = {fbpx, 8, 2, 2, 4, 1, has_alpha};
    return plane_compose(g, gw, gh, &q, &f);
}
static void test_plane(void) {
    uint8_t g[16];
    for (int i = 0; i < 4; i++) setpx(i, 255, 255);
    CHECK(comp(g, 2, 2, PLANE_ROT_0, 0, PLANE_BLEND_PREMULTI, 0, 0) == 0 && g[0] == 0, "plane alpha 0 white over black -> 0 (got %u)", g[0]);
    CHECK(comp(g, 2, 2, PLANE_ROT_0, 0x8000, PLANE_BLEND_NONE, 0, 0) == 0 && g[0] == 128, "plane alpha 0.5 -> 128 (got %u)", g[0]);
    setpx(0, 0, 255); setpx(1, 64, 255); setpx(2, 128, 255); setpx(3, 255, 255);
    comp(g, 2, 2, PLANE_ROT_0, 0xffff, PLANE_BLEND_PREMULTI, 0, 0);
    CHECK(g[0] == 0 && g[1] == 64 && g[2] == 128 && g[3] == 255, "identity %u,%u,%u,%u", g[0], g[1], g[2], g[3]);
    comp(g, 2, 2, PLANE_ROT_180, 0xffff, PLANE_BLEND_PREMULTI, 0, 0);
    CHECK(g[0] == 255 && g[1] == 128 && g[2] == 64 && g[3] == 0, "rotation 180 -> 255,128,64,0 (got %u,%u,%u,%u)", g[0], g[1], g[2], g[3]);
    comp(g, 2, 2, PLANE_ROT_90, 0xffff, PLANE_BLEND_PREMULTI, 0, 0);
    CHECK(g[0] == 64 && g[1] == 255 && g[2] == 0 && g[3] == 128, "rotation 90 ccw -> 64,255,0,128 (got %u,%u,%u,%u)", g[0], g[1], g[2], g[3]);
    comp(g, 2, 2, PLANE_ROT_270, 0xffff, PLANE_BLEND_PREMULTI, 0, 0);
    CHECK(g[0] == 128 && g[1] == 0 && g[2] == 255 && g[3] == 64, "rotation 270 ccw -> 128,0,255,64 (got %u,%u,%u,%u)", g[0], g[1], g[2], g[3]);
    comp(g, 2, 2, PLANE_ROT_0 | PLANE_REFLECT_X, 0xffff, PLANE_BLEND_PREMULTI, 0, 0);
    CHECK(g[0] == 64 && g[1] == 0 && g[2] == 255 && g[3] == 128, "reflect-x -> 64,0,255,128 (got %u,%u,%u,%u)", g[0], g[1], g[2], g[3]);
    comp(g, 2, 2, PLANE_ROT_0 | PLANE_REFLECT_Y, 0xffff, PLANE_BLEND_PREMULTI, 0, 0);
    CHECK(g[0] == 128 && g[1] == 255 && g[2] == 0 && g[3] == 64, "reflect-y -> 128,255,0,64 (got %u,%u,%u,%u)", g[0], g[1], g[2], g[3]);
    comp(g, 2, 2, PLANE_ROT_90 | PLANE_REFLECT_X, 0xffff, PLANE_BLEND_PREMULTI, 0, 0);
    CHECK(g[0] == 0 && g[1] == 128 && g[2] == 64 && g[3] == 255, "reflect-x then rotate 90 -> 0,128,64,255 (got %u,%u,%u,%u)", g[0], g[1], g[2], g[3]);
    comp(g, 4, 4, PLANE_ROT_180, 0xffff, PLANE_BLEND_PREMULTI, 0, 0);
    CHECK(g[0] == 255 && g[1] == 255 && g[5] == 255 && g[2] == 128 && g[15] == 0 && g[10] == 0, "scaled 2x2 -> 4x4 with rotation 180");
    for (int i = 0; i < 4; i++) setpx(i, 200, 128);
    CHECK(comp(g, 2, 2, PLANE_ROT_0, 0xffff, PLANE_BLEND_COVERAGE, 1, 0) == 0 && g[0] == 100, "coverage 200 @ alpha 128 over black -> 100 (got %u)", g[0]);
    for (int i = 0; i < 4; i++) setpx(i, 100, 128);
    comp(g, 2, 2, PLANE_ROT_0, 0xffff, PLANE_BLEND_PREMULTI, 1, 200);
    CHECK(g[0] == 200, "premultiplied 100 @ alpha 128 over 200 -> 200 (got %u)", g[0]);
    for (int i = 0; i < 4; i++) setpx(i, 90, 0);
    comp(g, 2, 2, PLANE_ROT_0, 0xffff, PLANE_BLEND_NONE, 1, 200);
    CHECK(g[0] == 90, "blend None ignores pixel alpha (got %u)", g[0]);
    CHECK(comp(g, 2, 2, PLANE_ROT_0 | PLANE_ROT_90, 0xffff, PLANE_BLEND_PREMULTI, 0, 0) < 0, "two rotation bits rejected");
    CHECK(comp(g, 2, 2, 1u << 6, 0xffff, PLANE_BLEND_PREMULTI, 0, 0) < 0, "unknown rotation bit rejected");
    CHECK(comp(g, 2, 2, PLANE_ROT_0, 0xffff, 7, 0, 0) < 0, "unknown blend mode rejected");
}
static void test_policy_stock(void) {
    struct pol_cfg c; pol_default_cfg(&c); c.clear_every = 3;
    CHECK(pol_apply_refresh_mode(&c, "stock") == 0 && c.stock && c.clear_every == 3, "select stock");
    CHECK(c.min_gap_ms == 0, "stock: no extra gap after the epdd ACK (%d ms)", c.min_gap_ms);
    struct sim m; sim_init(&m, &c);
    struct pol_action a = sim_tick(&m, 1.0, 0.5);
    CHECK(a.kind == POL_SHOW && !strcmp(a.mode, POL_READING), "stock: first change sent at once as REGAL (%s)", kn(a.kind));
    for (int i = 0; i < 12; i++) { m.content++; sim_tick(&m, 0.4, 0.5); }	/* 3 s of scrolling */
    CHECK(m.n_fast == 0 && strstr(m.log, "fastest") == NULL && strstr(m.log, "quality") == NULL, "stock: no A2/GC16 while scrolling: %s", m.log);
    CHECK(m.n_quality >= 4 && m.n_quality <= 7, "stock: latest-frame REGAL updates while scrolling, coalesced by busy (%d) %s", m.n_quality, m.log);
    int r0 = m.n_refresh; settle(&m, 4, 0.4, 0.5);
    CHECK(m.n_refresh == r0, "stock: cleanup deferred until still for settle_ms: %s", m.log);
    settle(&m, 8, 0.4, 0.5);
    CHECK(m.n_refresh == r0 + 1 && strstr(m.log, "refresh/reading") != NULL, "stock: one forced REGAL cleanup when idle: %s", m.log);
    settle(&m, 12, 0.4, 0.5);
    CHECK(m.n_refresh == r0 + 1, "stock: no repeated cleanup on a still page: %s", m.log);
}
/* eink-round2-20261006: the first capture after a held drag is released can still be the drag position (filmed: a
 * half-swiped home page shown for 1.4 s). Captures every 100 ms as in the ROM (a6l_eink.rc --interval 100). */
static void test_policy_release_settle(void) {
    struct pol_cfg c; pol_default_cfg(&c);
    CHECK(pol_apply_refresh_mode(&c, "stock") == 0 && c.release_quiet_ms == 90 && c.release_max_ms == 700, "stock keeps the release settle");
    struct pol_state s; pol_init(&s, &c, 0);
    double t = 10.0;
    pol_gesture_released(&s, t);
    struct pol_action a = pol_step(&s, t + 0.01, 0.3, 0.3, 0);	/* last drag frame vs the pre-drag capture */
    CHECK(a.kind == POL_NONE, "release: the last drag position is not sent (%s)", kn(a.kind));
    a = pol_step(&s, t + 0.11, 0.2, 0.4, 0);			/* app's settled frame != drag frame */
    CHECK(a.kind == POL_NONE, "release: content still changing, nothing sent (%s)", kn(a.kind));
    a = pol_step(&s, t + 0.21, 0, 0.4, 0);			/* identical capture pair */
    CHECK(a.kind == POL_SHOW && a.mode && !strcmp(a.mode, POL_READING), "release: settled page sent after one unchanged pair (%s)", kn(a.kind));
    pol_sent(&s, &a, t + 0.21); pol_done(&s, t + 1.0);
    a = pol_step(&s, t + 1.3, 0.1, 0.1, 0);
    CHECK(a.kind == POL_SHOW, "release settle is over: a later change is sent at once (%s)", kn(a.kind));
    pol_sent(&s, &a, t + 1.3); pol_done(&s, t + 2.0);
    pol_gesture_released(&s, 20.0);					/* a fling that keeps moving: bounded wait */
    int sent_at = -1;
    for (int k = 0; k < 12 && sent_at < 0; k++) { a = pol_step(&s, 20.01 + 0.1 * k, 0.1, 0.5, 0); if (a.kind != POL_NONE) sent_at = k; }
    CHECK(sent_at == 7, "release: still moving -> normal policy once release_max_ms elapsed (capture %d)", sent_at);
    c.release_quiet_ms = 0; pol_init(&s, &c, 0); pol_gesture_released(&s, 30);
    a = pol_step(&s, 30.01, 0.3, 0.3, 0);
    CHECK(a.kind == POL_SHOW, "release settle off (0): sent at once (%s)", kn(a.kind));
    struct pol_cfg q; pol_default_cfg(&q); q.release_quiet_ms = 120; q.release_max_ms = 400;
    CHECK(pol_apply_refresh_mode(&q, "partial") == 0 && q.release_quiet_ms == 120 && q.release_max_ms == 400, "refresh mode change keeps release settings");
}
/* eink-round2: tone curve. Identity and the pre-round2 linear contrast LUT are reproduced exactly; the default curve
 * maps light Material surfaces to paper white and dark/thin text to (near) black, monotonically. */
static void test_tone(void) {
    uint8_t l[256]; int ok = 1, mono = 1;
    tone_lut(l, 0, 0, 255, 100); for (int i = 0; i < 256; i++) ok &= l[i] == i;
    CHECK(ok, "tone: 0/0/255/100 is the identity");
    for (int c = 0; c <= 100; c += 25) {
        tone_lut(l, c, 0, 255, 100); ok = 1;
        int bp = c * 60 / 100, wp = 255 - c * 60 / 100;
        for (int i = 0; i < 256; i++) { int v = i <= bp ? 0 : i >= wp ? 255 : (i - bp) * 255 / (wp - bp); ok &= l[i] == v; }
        CHECK(ok, "tone: contrast %d with clips off and gamma 100 = old linear LUT", c);
    }
    tone_lut(l, 0, TONE_DEFAULT_BLACK, TONE_DEFAULT_WHITE, TONE_DEFAULT_GAMMA);
    for (int i = 1; i < 256; i++) mono &= l[i] >= l[i - 1];
    CHECK(mono, "tone: default curve monotonic");
    CHECK(l[255] == 255 && l[240] == 255 && l[232] == 255, "tone: Material surfaces (232..255) -> paper white (%d %d)", l[240], l[232]);
    CHECK(l[28] <= 2 && l[20] == 0, "tone: onSurface text (luma ~28) -> black (%d; < 8.5 = panel level 0)", l[28]);
    CHECK(l[105] >= 50 && l[105] <= 75, "tone: filmed thin-text grey 105 darkened to %d", l[105]);
    CHECK(l[128] >= 80 && l[128] <= 100, "tone: mid grey 128 -> %d", l[128]);
    CHECK(l[200] > 150 && l[200] < 230, "tone: light grey 200 stays a grey (%d)", l[200]);
    tone_lut(l, 0, 0, 255, 200); CHECK(l[128] >= 62 && l[128] <= 66, "tone: gamma 2.0 at 128/255 = %d (exp. 64)", l[128]);
    tone_lut(l, 0, 200, 210, 150); CHECK(l[100] > 0 && l[100] < 255, "tone: nonsense clips ignored (%d)", l[100]);
    {   /* eink-round11: inverted rendering */
        uint8_t n[256], v[256]; tone_lut(n, 0, TONE_DEFAULT_BLACK, TONE_DEFAULT_WHITE, TONE_DEFAULT_GAMMA); memcpy(v, n, 256); tone_lut_invert(v);
        int same = 1; for (int i = 0; i < 256; i++) same &= v[i] == n[255 - i];
        CHECK(same, "tone invert: v[i] = tone(255 - i)");
        CHECK(v[18] == 255 && v[45] > 200 && v[240] == 0 && v[255] == 0, "tone invert: dark-theme background (#121212) -> paper white, surface (#2d2d2d) -> light grey (%d), light text (240) -> black", v[45]);
        int mono = 1; for (int i = 1; i < 256; i++) mono &= v[i] <= v[i - 1];
        CHECK(mono, "tone invert: monotonic decreasing");
        tone_lut_invert(v); CHECK(!memcmp(v, n, 256), "tone invert twice = identity");
    }
}
/* eink-round3: A -> B -> A during the copy. The simulated scanout swaps the displayed slot every `period` checks (a
 * refresh); the BufferQueue slot A is re-rendered while B is shown. A before/after FB comparison accepts that copy. */
struct scan_sim { int calls, flip_at, back_at; int fb; uint8_t *slot_a; size_t n; };
static int scan_same(void *p) {
    struct scan_sim *s = p; s->calls++;
    if (s->calls == s->flip_at) { s->fb = 2; memset(s->slot_a, 0x77, s->n); }	/* B shown, A re-rendered */
    if (s->calls == s->back_at) s->fb = 1;					/* A shown again */
    return s->fb == 1;
}
static void test_guarded_copy(void) {
    enum { N = 10 * 1024 * 1024 + 123 };
    static uint8_t src[N], dst[N]; int k = 0;
    for (size_t i = 0; i < N; i++) src[i] = (uint8_t)(i * 31 + 7);
    struct scan_sim s = {0, -1, -1, 1, src, N};
    CHECK(guarded_copy(dst, src, N, GUARDED_COPY_CHUNK, scan_same, &s, &k) == N && !memcmp(dst, src, N), "guarded copy: unchanged plane set -> complete, exact copy");
    CHECK(k == 11 && s.calls == 11, "guarded copy: 10 MiB + tail in 1 MiB chunks = 11 checks (%d)", k);
    for (size_t i = 0; i < N; i++) src[i] = (uint8_t)(i * 31 + 7);
    struct scan_sim aba = {0, 4, 6, 1, src, N};
    CHECK(guarded_copy(dst, src, N, GUARDED_COPY_CHUNK, scan_same, &aba, &k) == -1 && k == 4, "guarded copy: A->B->A flip is caught at the first check after it (check %d)", k);
    /* what the before/after-only comparison accepted: same FB at both ends, rows from two different frames */
    for (size_t i = 0; i < N; i++) src[i] = (uint8_t)(i * 31 + 7);
    struct scan_sim old = {0, 4, 6, 1, src, N};
    for (int c = 0; c < 11; c++) { size_t o = (size_t)c << 20, m = N - o < (1u << 20) ? N - o : (1u << 20); memcpy(dst + o, src + o, m); scan_same(&old); }
    CHECK(old.fb == 1 && dst[0] != 0x77 && dst[N - 1] == 0x77, "guarded copy: reference - unguarded copy ends on the same FB but is torn");
    CHECK(guarded_copy(dst, src, 0, 0, scan_same, &s, &k) == 0 && k == 0, "guarded copy: empty buffer");
    CHECK(guarded_copy(dst, src, 100, 0, NULL, NULL, &k) == 100 && k == 1, "guarded copy: no checker, default chunk");
}
/* ---------------- eink-round5: the notification-drawer case (plane_copy_policy) ----------------
 * One 1080x2340 XRGB plane (4352 B rows, 10.2 MB) of a BufferQueue with 3 slots, animated at 60 Hz (the shade being
 * pulled / an animating notification): frame f is rendered into slot f % 3 and shown during [f T, (f+1) T). When frame
 * f+1 is shown, the slot of frame f is released and the producer renders frame f+3 into it, top to bottom, from d0 ms
 * after the release over R ms. The mirror reads write-combined memory at ~110 MB/s (6 Oct logs: copy=156-195 ms for
 * ~20 MB), so a plane copy takes ~93 ms = 5-6 flips. Each row carries its frame number at both ends; a capture is valid
 * when its rows are bands of CONSECUTIVE frames from top to bottom, each frame displayed during the copy (a clean seam
 * per flip, like a vsync tear); rows of a buffer being re-rendered show up as a jump (N -> N+3) or a step back. */
#define DR_ROWS 2340
#define DR_ROWB 4352
#define DR_N ((size_t)DR_ROWS * DR_ROWB)
#define DR_KB_MS 110.0			/* KB per ms */
struct drawer {
    double t, T, d0, R, open_ms; size_t knob; int retries, opens;
    int rowframe[3][DR_ROWS]; uint8_t *slot[3];
    int geom_change_frame, gone_frame, opened_geom;
};
static int dr_shown(const struct drawer *d) { return d->T > 0 ? (int)(d->t / d->T) : 0; }
static int dr_geom(const struct drawer *d, int f) { return d->geom_change_frame > 0 && f >= d->geom_change_frame; }
static int dr_row_frame(const struct drawer *d, int s, int r) {	/* newest frame (= s mod 3) whose row r is rendered by t */
    if (d->T <= 0) return s;
    /* frame s + 3k (k >= 1) is rendered into row r once (s + 3k - 2) T + d0 + R r / ROWS <= t (frames 0..2 exist at t = 0) */
    double x = ((d->t - d->d0 - d->R * r / DR_ROWS) / d->T + 2 - s) / 3.0;
    int k = x < 1 ? 0 : (int)x;
    return s + 3 * k;
}
static void dr_advance(struct drawer *d, double ms) {
    d->t += ms;
    for (int s = 0; s < 3; s++) for (int r = 0; r < DR_ROWS; r++) {
        int f = dr_row_frame(d, s, r); if (f == d->rowframe[s][r]) continue;
        d->rowframe[s][r] = f; uint8_t *row = d->slot[s] + (size_t)r * DR_ROWB;
        memcpy(row, &f, 4); memcpy(row + DR_ROWB - 4, &f, 4);
    }
}
static size_t dr_chunk_now(const struct drawer *d) {	/* the chunk plane_copy_policy uses for this open */
    size_t c = pcopy_chunk(d->knob, DR_N);
    return d->opens > d->retries && c > PCOPY_STITCH_CHUNK ? PCOPY_STITCH_CHUNK : c;
}
static int dr_open(void *p, const uint8_t **px, uint32_t *gen) {
    struct drawer *d = p; d->opens++; dr_advance(d, d->open_ms);
    int f = dr_shown(d); if (d->gone_frame > 0 && f >= d->gone_frame) return 1;
    *px = d->slot[f % 3]; *gen = 100 + (uint32_t)(f % 3);
    int g = dr_geom(d, f), changed = d->opens > 1 && g != d->opened_geom; d->opened_geom = g;
    return changed ? 2 : 0;
}
static void dr_close(void *p) { (void)p; }
static int dr_still(void *p, uint32_t gen) {
    struct drawer *d = p; dr_advance(d, dr_chunk_now(d) / 1024.0 / DR_KB_MS);
    int f = dr_shown(d);
    return 100 + (uint32_t)(f % 3) == gen && dr_geom(d, f) == d->opened_geom && !(d->gone_frame > 0 && f >= d->gone_frame);
}
static const struct pcopy_ops dr_ops = {dr_open, dr_close, dr_still};
static void dr_init(struct drawer *d, double T, double t0, size_t knob) {
    uint8_t *keep[3] = {d->slot[0], d->slot[1], d->slot[2]};
    memset(d, 0, sizeof *d); memcpy(d->slot, keep, sizeof keep);
    d->T = T; d->d0 = 1.0; d->R = 8.0; d->open_ms = 0.3; d->knob = knob; d->retries = PCOPY_RETRIES;
    for (int s = 0; s < 3; s++) for (int r = 0; r < DR_ROWS; r++) d->rowframe[s][r] = -1;
    dr_advance(d, t0);
}
/* 1 = valid; *bands = number of frame bands; f0..f1 = frames displayed during the copy */
static int dr_valid(const uint8_t *dst, int f0, int f1, int *bands) {
    int prev = -1, ok = 1; *bands = 0;
    for (int r = 0; r < DR_ROWS; r++) {	/* a row cut by a chunk boundary at a seam: its start from frame a, its end from a + 1 */
        int a, b; memcpy(&a, dst + (size_t)r * DR_ROWB, 4); memcpy(&b, dst + (size_t)r * DR_ROWB + DR_ROWB - 4, 4);
        if (a < f0 || b > f1 || (b != a && b != a + 1)) ok = 0;
        if (a != prev) { if (prev >= 0 && a != prev + 1) ok = 0; (*bands)++; prev = a; }
        if (b != prev) { (*bands)++; prev = b; }
    }
    return ok;
}
/* round 4 (0018) references: knob 0 = a copy without any check, accepted as consistent; guard on = a guarded copy, after
 * the last retry the whole plane copied again unguarded from the final attempt's buffer (*torn = 1, forced after 400 ms) */
static void dr_copy_unguarded(struct drawer *d, uint8_t *dst, const uint8_t *src) {
    for (size_t o = 0; o < DR_N; o += GUARDED_COPY_CHUNK) {
        size_t c = DR_N - o < GUARDED_COPY_CHUNK ? DR_N - o : GUARDED_COPY_CHUNK;
        memcpy(dst + o, src + o, c); dr_advance(d, c / 1024.0 / DR_KB_MS);
    }
}
static int dr_round4_guard_on(struct drawer *d, uint8_t *dst) {	/* returns 1 when the torn finish was used */
    for (int attempt = 0; attempt <= PCOPY_RETRIES; attempt++) {
        dr_advance(d, d->open_ms); int f = dr_shown(d); const uint8_t *src = d->slot[f % 3]; uint32_t gen = 100 + (uint32_t)(f % 3); int flipped = 0;
        for (size_t o = 0; o < DR_N && !flipped; o += GUARDED_COPY_CHUNK) {
            size_t c = DR_N - o < GUARDED_COPY_CHUNK ? DR_N - o : GUARDED_COPY_CHUNK;
            memcpy(dst + o, src + o, c); dr_advance(d, c / 1024.0 / DR_KB_MS);
            flipped = 100 + (uint32_t)(dr_shown(d) % 3) != gen;
        }
        if (!flipped) return 0;
        if (attempt == PCOPY_RETRIES) { dr_copy_unguarded(d, dst, src); return 1; }
    }
    return 0;
}
static void test_drawer_capture(void) {
    static struct drawer d; static uint8_t *dst;
    for (int s = 0; s < 3; s++) if (!d.slot[s]) d.slot[s] = calloc(1, DR_N);
    if (!dst) dst = calloc(1, DR_N);
    if (!dst || !d.slot[0] || !d.slot[1] || !d.slot[2]) { CHECK(0, "drawer: out of memory"); return; }
    const double T = 1000.0 / 60; const int NCAP = 40; unsigned seed = 12345;
    int r4_knob0_bad = 0, r4_forced = 0, r4_forced_bad = 0, new_bad = 0, new_torn = 0, new_stitched = 0, new_ok = 0, max_bands = 0, max_sw = 0;
    for (int i = 0; i < NCAP; i++) {
        seed = seed * 1103515245u + 12345u; double t0 = 200 + (seed >> 8) % 4000 / 4.0;
        int bands, f0, f1;
        /* round 4 as installed on 6 Oct 21:18 (copy_guard_kib=0): no check, accepted */
        dr_init(&d, T, t0, 0); dr_advance(&d, d.open_ms); f0 = dr_shown(&d);
        dr_copy_unguarded(&d, dst, d.slot[f0 % 3]); f1 = dr_shown(&d);
        if (!dr_valid(dst, f0, f1, &bands)) r4_knob0_bad++;
        /* round 4 with the guard on: what the never-starve rule forces after 3 discards / 400 ms */
        dr_init(&d, T, t0, GUARDED_COPY_CHUNK); f0 = dr_shown(&d);
        if (dr_round4_guard_on(&d, dst)) { r4_forced++; f1 = dr_shown(&d); if (!dr_valid(dst, f0, f1, &bands)) r4_forced_bad++; }
        /* round 5 */
        for (int knob = 0; knob < 2; knob++) {
            dr_init(&d, T, t0, knob ? GUARDED_COPY_CHUNK : 0); f0 = dr_shown(&d) ; struct pcopy_stats st;
            int r = plane_copy_policy(dst, DR_N, pcopy_chunk(d.knob, DR_N), PCOPY_RETRIES, PCOPY_MAX_SWITCHES, &dr_ops, &d, &st);
            f1 = dr_shown(&d);
            int v = dr_valid(dst, f0, f1, &bands);
            if (!v || r == PCOPY_ERR || r == PCOPY_GONE) new_bad++;
            if (r == PCOPY_TORN) new_torn++; else if (r == PCOPY_STITCHED) new_stitched++; else if (r == PCOPY_OK) new_ok++;
            if (bands > max_bands) max_bands = bands;
            if (st.switches > max_sw) max_sw = st.switches;
            if (r == PCOPY_STITCHED && bands > st.switches + 1) new_bad++;
        }
    }
    printf("drawer (60 Hz shade, %d captures): round4 knob0 torn %d/%d; round4 guard-on forced %d (torn %d); round5 ok %d stitched %d torn %d bad %d, max bands %d, max switches %d\n",
           NCAP, r4_knob0_bad, NCAP, r4_forced, r4_forced_bad, new_ok, new_stitched, new_torn, new_bad, max_bands, max_sw);
    CHECK(r4_knob0_bad * 4 >= NCAP, "drawer: reproduced - round 4 with copy_guard_kib=0 accepts torn copies (%d/%d)", r4_knob0_bad, NCAP);
    CHECK(r4_forced > 0 && r4_forced_bad * 2 >= r4_forced, "drawer: reproduced - round 4 guard on: the forced (never-starve) copy is torn (%d/%d)", r4_forced_bad, r4_forced);
    CHECK(new_bad == 0 && new_torn == 0, "drawer: round 5 never yields a torn plane (bad %d, torn %d of %d)", new_bad, new_torn, 2 * NCAP);
    CHECK(new_stitched > 0, "drawer: a plane flipping on every copy is stitched (%d), not discarded for ever", new_stitched);
    CHECK(max_sw <= 8, "drawer: stitching needs few buffer switches (max %d)", max_sw);
    /* static page: one attempt, exact */
    dr_init(&d, 0, 500, GUARDED_COPY_CHUNK); struct pcopy_stats st; int bands;
    int r = plane_copy_policy(dst, DR_N, pcopy_chunk(d.knob, DR_N), PCOPY_RETRIES, PCOPY_MAX_SWITCHES, &dr_ops, &d, &st);
    CHECK(r == PCOPY_OK && st.attempts == 1 && st.checks == 10 && dr_valid(dst, 0, 0, &bands) && bands == 1, "drawer: static plane -> consistent copy in one attempt, 10 checks (%d)", st.checks);
    /* knob 0: one check at the end of the plane, a flip is still seen */
    dr_init(&d, T, 700, 0); int f0 = dr_shown(&d);
    r = plane_copy_policy(dst, DR_N, pcopy_chunk(0, DR_N), PCOPY_RETRIES, PCOPY_MAX_SWITCHES, &dr_ops, &d, &st);
    CHECK(r == PCOPY_STITCHED && st.attempts == 3 && st.checks > 20 && dr_valid(dst, f0, dr_shown(&d), &bands), "drawer: copy_guard_kib=0 no longer disables the guard: chunked checks (%d), stitched, valid", st.checks);
    /* the status bar layer (1080x75, 326 KB): shorter than a frame, consistent */
    int sb_ok = 0;
    for (int i = 0; i < 20; i++) {
        dr_init(&d, T, 300 + i * 7.3, GUARDED_COPY_CHUNK); size_t n = (size_t)75 * DR_ROWB;
        r = plane_copy_policy(dst, n, pcopy_chunk(d.knob, n), PCOPY_RETRIES, PCOPY_MAX_SWITCHES, &dr_ops, &d, &st);
        sb_ok += r == PCOPY_OK;
    }
    CHECK(sb_ok == 20, "drawer: status-bar sized plane flipping at 60 Hz -> consistent copies (%d/20)", sb_ok);
    /* the plane changes geometry (scrim alpha / size) mid-copy: restarted on the new geometry, never mixed */
    dr_init(&d, T, 1000, GUARDED_COPY_CHUNK); d.geom_change_frame = dr_shown(&d) + 2;
    r = plane_copy_policy(dst, DR_N, pcopy_chunk(d.knob, DR_N), PCOPY_RETRIES, PCOPY_MAX_SWITCHES, &dr_ops, &d, &st);
    CHECK((r == PCOPY_OK || r == PCOPY_STITCHED) && st.geometry_changes >= 1 && dr_valid(dst, d.geom_change_frame, dr_shown(&d), &bands),
          "drawer: geometry change mid-copy -> copy restarted on the new geometry (r=%d, changes %d)", r, st.geometry_changes);
    /* the plane leaves the CRTC */
    dr_init(&d, T, 1000, GUARDED_COPY_CHUNK); d.gone_frame = dr_shown(&d) + 1;
    r = plane_copy_policy(dst, DR_N, pcopy_chunk(d.knob, DR_N), PCOPY_RETRIES, PCOPY_MAX_SWITCHES, &dr_ops, &d, &st);
    CHECK(r == PCOPY_GONE, "drawer: plane gone during the copy -> PCOPY_GONE (%d)", r);
    CHECK(pcopy_chunk(0, 1000) == GUARDED_COPY_CHUNK && pcopy_chunk(4096, 1000) == 4096, "pcopy chunk: knob 0 = default 1 MiB chunks (the guard cannot be switched off)");
    CHECK(pcache_usable(0.3, 1) && !pcache_usable(0.6, 1) && !pcache_usable(0.1, 0), "plane cache: recent and same geometry only");
}
static void test_front_follow(void) {
    struct front_follow f = {0};
    CHECK(front_follow_step(&f, 0) == 0, "front follow: LCD on -> nothing");
    CHECK(front_follow_step(&f, 1) == 1, "front follow: LCD off -> e-ink CRTC off once");
    CHECK(front_follow_step(&f, 1) == 0 && front_follow_step(&f, 1) == 0, "front follow: repeated off captures -> no repeat");
    CHECK(front_follow_step(&f, 0) == 0 && front_follow_step(&f, 1) == 1, "front follow: next sleep -> off again");
}
/* eink-round4: the 6 Oct 17:01 starvation (status bar redrawing: every capture discarded for 15-45 s) cannot recur */
static void test_cap_guard(void) {
    struct cap_guard g; cap_guard_init(&g, CAP_GUARD_MAX_DISCARDS, CAP_GUARD_MAX_MS);
    CHECK(cap_guard_step(&g, CAPK_OK, 1.0) == CV_ACCEPT, "cap guard: consistent capture accepted");
    CHECK(cap_guard_step(&g, CAPK_LAYOUT, 1.1) == CV_DISCARD && cap_guard_step(&g, CAPK_TORN, 1.2) == CV_DISCARD, "cap guard: layout change / torn copy discarded");
    CHECK(cap_guard_step(&g, CAPK_OK, 1.3) == CV_ACCEPT && g.discards == 0, "cap guard: a consistent capture ends the streak");
    /* continuous redraw, captures every 0.1 s: never more than 3 discards and never more than max_ms without a picture */
    int longest = 0, run = 0, forced = 0; double t = 2.0, last_accept = t, worst_gap = 0;
    for (int i = 0; i < 300; i++, t += 0.1) {
        enum cap_verdict v = cap_guard_step(&g, i % 7 == 6 ? CAPK_TORN : CAPK_LAYOUT, t);
        if (v == CV_DISCARD) { if (++run > longest) longest = run; }
        else { run = 0; forced += v == CV_FORCE; if (t - last_accept > worst_gap) worst_gap = t - last_accept; last_accept = t; }
    }
    CHECK(longest <= CAP_GUARD_MAX_DISCARDS && forced >= 70, "cap guard: endless redraw -> at most %d discards in a row (%d), %d forced pictures", CAP_GUARD_MAX_DISCARDS, longest, forced);
    CHECK(worst_gap <= CAP_GUARD_MAX_MS / 1000.0 + 0.1 + 1e-9, "cap guard: no picture gap longer than max_ms + one interval (%.2f s)", worst_gap);
    /* slow captures (0.25 s each, the filmed copy+compose time): the time bound fires after 2 discards */
    cap_guard_init(&g, CAP_GUARD_MAX_DISCARDS, CAP_GUARD_MAX_MS);
    CHECK(cap_guard_step(&g, CAPK_LAYOUT, 10.0) == CV_DISCARD && cap_guard_step(&g, CAPK_LAYOUT, 10.25) == CV_DISCARD &&
          cap_guard_step(&g, CAPK_LAYOUT, 10.5) == CV_FORCE, "cap guard: 400 ms bound with 250 ms captures -> third capture forced");
    cap_guard_init(&g, 0, 400);
    CHECK(cap_guard_step(&g, CAPK_TORN, 1.0) == CV_FORCE, "cap guard: max_discards 0 -> nothing is ever discarded");
    cap_guard_init(&g, 3, 400); cap_guard_step(&g, CAPK_LAYOUT, 1.0); cap_guard_reset(&g);
    CHECK(cap_guard_step(&g, CAPK_LAYOUT, 9.0) == CV_DISCARD && g.discards == 1, "cap guard: reset (front off) starts a new streak");
}
/* eink-round4: the pre-round4 mirror resample() (doubles), verbatim apart from the buffer arguments: the reference */
static void ref_resample(const uint8_t *gray, int gw, int gh, uint8_t *out, int out_w, int out_h, int ox, int oy, int dw, int dh, const uint8_t *lut) {
    static float *acc; static size_t acc_n; size_t need = (size_t)dw * gh;
    if (need > acc_n) { free(acc); acc = malloc(need * sizeof *acc); acc_n = need; }
    double fx = (double)gw / dw, fy = (double)gh / dh;
    for (int y = 0; y < gh; y++) {
        const uint8_t *r = gray + (size_t)y * gw; float *dst = acc + (size_t)y * dw;
        for (int x = 0; x < dw; x++) {
            double a = x * fx, b = a + fx; int i0 = (int)a, i1 = (int)b; if (i1 >= gw) i1 = gw - 1; double s = 0;
            if (i0 == i1) s = r[i0] * fx;
            else { s += r[i0] * (i0 + 1 - a); for (int i = i0 + 1; i < i1; i++) s += r[i]; if (b > i1) s += r[i1] * (b - i1); }
            dst[x] = (float)(s / fx);
        }
    }
    for (int y = 0; y < dh; y++) {
        int yy = y + oy; if (yy < 0 || yy >= out_h) continue;
        double a = y * fy, b = a + fy; int j0 = (int)a, j1 = (int)b; if (j1 >= gh) j1 = gh - 1;
        for (int x = 0; x < dw; x++) { int xx = x + ox; if (xx < 0 || xx >= out_w) continue; double s;
            if (j0 == j1) s = acc[(size_t)j0 * dw + x] * fy; else { s = acc[(size_t)j0 * dw + x] * (j0 + 1 - a); for (int j = j0 + 1; j < j1; j++) s += acc[(size_t)j * dw + x]; if (b > j1) s += acc[(size_t)j1 * dw + x] * (b - j1); }
            int v = (int)(s / fy + 0.5); out[(size_t)yy * out_w + xx] = lut[v > 255 ? 255 : v < 0 ? 0 : v]; }
    }
}
static double mono_s(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }
static void test_area_resize(void) {
    enum { GW = 1080, GH = 2340, OW = 720, OH = 1440 };
    static uint8_t src[GW * GH], a[OW * OH], b[OW * OH]; uint8_t id[256], tone[256];
    for (int i = 0; i < 256; i++) id[i] = (uint8_t)i;
    tone_lut(tone, 0, TONE_DEFAULT_BLACK, TONE_DEFAULT_WHITE, TONE_DEFAULT_GAMMA);
    unsigned seed = 12345;
    for (int y = 0; y < GH; y++) for (int x = 0; x < GW; x++) {	/* text-like: white page, dark 2-3 px strokes, noise */
        seed = seed * 1103515245u + 12345u; int v = ((x / 3 + y / 5) % 9 == 0) ? 20 : 235; if ((seed >> 16) % 13 == 0) v = (int)((seed >> 8) & 255);
        src[(size_t)y * GW + x] = (uint8_t)v;
    }
    struct area_resizer r = {0};
    struct { int ox, oy, dw, dh; const char *what; } g[] = {{0, 0, OW, OH, "stretch"}, {27, 0, 665, OH, "letterbox"}, {0, -84, OW, 1560, "crop"}};
    for (unsigned k = 0; k < sizeof g / sizeof g[0]; k++) for (int l = 0; l < 2; l++) {
        const uint8_t *lut = l ? tone : id;
        memset(a, 255, sizeof a); memset(b, 255, sizeof b);
        CHECK(area_resize(&r, src, GW, GH, a, OW, OH, g[k].ox, g[k].oy, g[k].dw, g[k].dh, lut) == 0, "area resize %s: ok", g[k].what);
        ref_resample(src, GW, GH, b, OW, OH, g[k].ox, g[k].oy, g[k].dw, g[k].dh, lut);
        int maxd = 0; long sum = 0, n1 = 0;
        for (int i = 0; i < OW * OH; i++) { int d = abs(a[i] - b[i]); if (d > maxd) maxd = d; sum += d; n1 += d > 0; }
        /* identity LUT: within 1 level; tone LUT: a 1-level input difference can become up to ~2 levels at the steepest point */
        CHECK(maxd <= (l ? 3 : 1), "area resize %s%s vs double reference: max diff %d", g[k].what, l ? " (tone LUT)" : "", maxd);
        CHECK(n1 * 100 <= OW * OH * (l ? 3 : 2), "area resize %s%s: %.2f %% of pixels differ", g[k].what, l ? " (tone LUT)" : "", 100.0 * n1 / (OW * OH));
    }
    memset(src, 137, sizeof src);
    memset(a, 0, sizeof a); area_resize(&r, src, GW, GH, a, OW, OH, 0, 0, OW, OH, id);
    int flat = 1; for (int i = 0; i < OW * OH; i++) flat &= a[i] == 137;
    CHECK(flat, "area resize: flat grey stays exact (weights sum to one exactly)");
    memset(src, 255, sizeof src); area_resize(&r, src, GW, GH, a, OW, OH, 0, 0, OW, OH, id);
    flat = 1; for (int i = 0; i < OW * OH; i++) flat &= a[i] == 255;
    CHECK(flat, "area resize: white stays 255 (no 32-bit overflow)");
    double t0 = mono_s(); for (int i = 0; i < 5; i++) area_resize(&r, src, GW, GH, a, OW, OH, 0, 0, OW, OH, id);
    double t1 = mono_s(); for (int i = 0; i < 5; i++) ref_resample(src, GW, GH, b, OW, OH, 0, 0, OW, OH, id);
    double t2 = mono_s();
    printf("area resize timing (host, -O1): fixed point %.1f ms, double reference %.1f ms per frame\n", (t1 - t0) * 200, (t2 - t1) * 200);
    CHECK((t1 - t0) < (t2 - t1), "area resize: faster than the double reference");
    area_resizer_free(&r);
}
/* eink-round4: pol_step() == pol_observe() + pol_decide() (the staged capture splits them) */
static void test_policy_split(void) {
    struct pol_cfg c; pol_default_cfg(&c);
    struct pol_state a, b; pol_init(&a, &c, 0); pol_init(&b, &c, 0);
    double mv[] = {0.2, 0.3, 0, 0, 0, 0.1, 0, 0, 0, 0, 0, 0}, t = 0; int same = 1;
    for (unsigned i = 0; i < sizeof mv / sizeof mv[0]; i++) {
        t += 0.25;
        struct pol_action x = pol_step(&a, t, mv[i], mv[i] > 0 ? mv[i] : 0.05, 0);
        pol_observe(&b, t, mv[i]); struct pol_action y = pol_decide(&b, t, mv[i] > 0 ? mv[i] : 0.05, 0);
        same &= x.kind == y.kind && (x.mode == y.mode || (x.mode && y.mode && !strcmp(x.mode, y.mode)));
        if (x.kind != POL_NONE) { pol_sent(&a, &x, t); pol_sent(&b, &y, t); pol_done(&a, t + 0.1); pol_done(&b, t + 0.1); }
    }
    CHECK(same && a.consec == b.consec && a.last_change == b.last_change, "policy: step == observe + decide");
}
int main(void) {
    test_guarded_copy(); test_drawer_capture(); test_front_follow(); test_cap_guard(); test_area_resize(); test_policy_split();
    test_tone(); test_policy_release_settle(); test_policy_stock(); test_policy_auto(); test_policy_rate(); test_policy_reading(); test_explicit_fast_modes(); test_keys(); test_tmap(); test_plane();
    printf("%s: %d checks, %d failures\n", fails ? "EINK_LOGIC_TESTS_FAIL" : "EINK_LOGIC_TESTS_PASS", checks, fails);
    return fails != 0;
}
