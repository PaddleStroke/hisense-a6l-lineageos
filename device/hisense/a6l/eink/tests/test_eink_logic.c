// SPDX-License-Identifier: Apache-2.0
/* Host unit tests for eink_logic.c (mode policy, key state machine, touch mapping). cc -o t test_eink_logic.c ../src/eink_logic.c */
#include <stdio.h>
#include <string.h>
#include "../src/eink_logic.h"

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
static const char *kn(enum pol_kind k) { return k == POL_NONE ? "none" : k == POL_SHOW ? "show" : k == POL_REFRESH ? "refresh" : "clear+show"; }

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
        pol_sent(&m->s, &a, m->t); m->busy = 1; m->busy_until = m->t + busy_s * (a.kind == POL_CLEAR_THEN_SHOW ? 3 : 1); m->panel = m->content;
        if (a.kind == POL_REFRESH) m->n_refresh++; else if (!strcmp(a.mode, POL_FASTEST)) m->n_fast++; else m->n_quality++;
        if (a.kind == POL_CLEAR_THEN_SHOW) m->n_clear++;
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
    /* clear_every = 3: the 3rd clean (quality) update is preceded by a clear */
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
}
int main(void) {
    test_policy_auto(); test_policy_rate(); test_policy_reading(); test_keys(); test_tmap();
    printf("%s: %d checks, %d failures\n", fails ? "EINK_LOGIC_TESTS_FAIL" : "EINK_LOGIC_TESTS_PASS", checks, fails);
    return fails != 0;
}
