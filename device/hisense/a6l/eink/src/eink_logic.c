// SPDX-License-Identifier: Apache-2.0
/* eink_logic.c — see eink_logic.h. Policy numbers come from the M1 mirror (docs/eink-mirror-milestone-20260923.md)
 * and the stock EpdManagerService/libtcon_eink disassembly (docs/eink-android-integration-20260923.md). */
#include "eink_logic.h"
#include <stdlib.h>
#include <string.h>

void pol_default_cfg(struct pol_cfg *c) {
    c->quiet_ms = 300; c->settle_ms = 1000; c->min_gap_ms = 150; c->clear_every = 10; c->max_per_min = 60;
    c->active_mode = POL_FASTEST; c->reading = 0; c->reading_refresh_every = 8; c->reading_full_frac = 0.6;
}
void pol_init(struct pol_state *s, const struct pol_cfg *c, double now) {
    memset(s, 0, sizeof *s); s->cfg = *c; s->win_t = now; s->done_t = now - 10; s->last_change = now - 10;
}
int pol_in_burst(const struct pol_state *s) { return s->burst; }

struct pol_action pol_step(struct pol_state *s, double now, double moving, double vs_panel, int busy) {
    struct pol_action a = {POL_NONE, NULL};
    const struct pol_cfg *c = &s->cfg;
    if (moving > 0) { s->last_change = now; s->consec++; } else s->consec = 0;
    if (now - s->win_t >= 60) { s->win_t = now; s->win_n = 0; }
    double quiet_ms = (now - s->last_change) * 1000;
    int can = !busy && (now - s->done_t) * 1000 >= c->min_gap_ms && s->win_n < c->max_per_min;
    if (!can) return a;
    if (c->reading) {
        /* reading mode: never chase motion; one update when the page is still, REGAL (partial, no flash) for small
         * changes, GC16 for page turns; every N partial updates a forced GC16 refresh of the same picture */
        if (vs_panel > 0 && quiet_ms >= c->settle_ms) {
            a.kind = POL_SHOW; a.mode = vs_panel > c->reading_full_frac || !s->primed ? POL_QUALITY : POL_READING;
            if (!strcmp(a.mode, POL_QUALITY) && c->clear_every > 0 && (s->clean_n + 1) % (c->clear_every * 2) == 0) a.kind = POL_CLEAR_THEN_SHOW;
        } else if (vs_panel == 0 && s->reading_n >= c->reading_refresh_every && quiet_ms >= c->settle_ms) {
            a.kind = POL_REFRESH; a.mode = POL_QUALITY;
        }
        return a;
    }
    if (vs_panel > 0 && quiet_ms >= (s->burst ? c->settle_ms : c->quiet_ms)) { a.kind = POL_SHOW; a.mode = POL_QUALITY; }
    else if (vs_panel > 0 && (s->burst || s->consec >= 2)) { a.kind = POL_SHOW; a.mode = c->active_mode; }
    else if (vs_panel == 0 && s->fast_on_panel && quiet_ms >= c->settle_ms) { a.kind = POL_REFRESH; a.mode = POL_QUALITY; }
    if (a.kind == POL_SHOW && !strcmp(a.mode, POL_QUALITY) && c->clear_every > 0 && (s->clean_n + 1) % c->clear_every == 0)
        a.kind = POL_CLEAR_THEN_SHOW;
    return a;
}
void pol_sent(struct pol_state *s, const struct pol_action *a, double now) {
    (void)now;
    int clean = a->mode && strcmp(a->mode, s->cfg.active_mode) != 0;
    if (s->cfg.reading) clean = 1;
    s->win_n += a->kind == POL_CLEAR_THEN_SHOW ? 3 : 1;
    s->primed = 1;
    if (a->kind == POL_REFRESH) { s->fast_on_panel = 0; s->reading_n = 0; s->burst = 0; return; }
    if (clean) {
        s->clean_n++; s->fast_on_panel = 0; s->burst = 0;
        if (s->cfg.reading) { if (!strcmp(a->mode, POL_READING)) s->reading_n++; else s->reading_n = 0; }
    } else { s->fast_on_panel = 1; s->burst = 1; }
}
void pol_done(struct pol_state *s, double now) { s->done_t = now; }

int pol_apply_refresh_mode(struct pol_cfg *c, const char *m) {
    if (!m || !*m) return -1;
    struct pol_cfg d; pol_default_cfg(&d);
    d.clear_every = c->clear_every; d.max_per_min = c->max_per_min; d.min_gap_ms = c->min_gap_ms;
    if (!strcmp(m, "auto")) { }
    else if (!strcmp(m, "quality")) { d.reading = 1; d.reading_full_frac = 0.0; d.settle_ms = 700; }
    else if (!strcmp(m, "partial") || !strcmp(m, "reading")) { d.reading = 1; }
    else if (!strcmp(m, "fast")) { d.active_mode = POL_FAST; d.quiet_ms = 200; d.settle_ms = 800; }
    else if (!strcmp(m, "fastest")) { d.active_mode = POL_FASTEST; d.quiet_ms = 150; d.settle_ms = 600; }
    else return -1;
    *c = d; return 0;
}

void key_init(struct key_state *k, int long_ms) { memset(k, 0, sizeof *k); k->long_ms = long_ms > 0 ? long_ms : 800; }
enum key_action key_event(struct key_state *k, int value, double now) {
    if (value == 2) return key_tick(k, now);
    if (value == 1) { if (!k->down) { k->down = 1; k->t_down = now; k->long_fired = 0; } return KA_NONE; }
    if (!k->down) return KA_NONE;
    k->down = 0;
    if (k->long_fired) return KA_NONE;
    if ((now - k->t_down) * 1000 >= k->long_ms) return KA_CLEAR;	/* released late without a tick in between */
    return KA_TOGGLE;
}
enum key_action key_tick(struct key_state *k, double now) {
    if (k->down && !k->long_fired && (now - k->t_down) * 1000 >= k->long_ms) { k->long_fired = 1; return KA_CLEAR; }
    return KA_NONE;
}
int mode_parse(const char *s, int def) {
    if (!s || !*s) return def;
    if (!strcmp(s, "off") || !strcmp(s, "0") || !strcmp(s, "false")) return EINK_OFF;
    if (!strcmp(s, "mirror") || !strcmp(s, "on") || !strcmp(s, "1") || !strcmp(s, "true")) return EINK_MIRROR;
    return def;
}
const char *mode_name(int m) { return m == EINK_MIRROR ? "mirror" : "off"; }

void fit_geometry(int gw, int gh, int pw, int ph, int crop, int *ox, int *oy, int *dw, int *dh) {
    double sx = (double)pw / gw, sy = (double)ph / gh, sc = crop ? (sx > sy ? sx : sy) : (sx < sy ? sx : sy);
    int w = (int)(gw * sc + 0.5), h = (int)(gh * sc + 0.5);
    if (!crop) { if (w > pw) w = pw; if (h > ph) h = ph; }
    *dw = w; *dh = h; *ox = (pw - w) / 2; *oy = (ph - h) / 2;
}
int tmap_apply(const struct tmap *m, int rx, int ry, int *fx, int *fy) {
    /* raw -> panel portrait */
    double u = m->touch_w > 1 ? (double)rx / (m->touch_w - 1) : 0, v = m->touch_h > 1 ? (double)ry / (m->touch_h - 1) : 0;
    if (m->swap_xy) { double t = u; u = v; v = t; }
    if (m->inv_x) u = 1 - u;
    if (m->inv_y) v = 1 - v;
    double px = u * (m->panel_w - 1), py = v * (m->panel_h - 1);
    /* panel -> front picture (picture pixel centres map onto front pixel centres, corners exactly); the result is
     * always clamped into the picture, the return value says if the touch was inside it */
    if (m->img_w <= 1 || m->img_h <= 1 || m->front_w <= 0 || m->front_h <= 0) { *fx = *fy = 0; return 0; }
    double qx = (px - m->img_x) / (m->img_w - 1), qy = (py - m->img_y) / (m->img_h - 1);
    int inside = qx >= -1e-9 && qy >= -1e-9 && qx <= 1 + 1e-9 && qy <= 1 + 1e-9;
    if (qx < 0) qx = 0;
    if (qx > 1) qx = 1;
    if (qy < 0) qy = 0;
    if (qy > 1) qy = 1;
    int x = (int)(qx * (m->front_w - 1) + 0.5), y = (int)(qy * (m->front_h - 1) + 0.5);
    *fx = x; *fy = y; return inside;
}
int tmap_parse_transform(struct tmap *m, const char *s) {
    m->swap_xy = m->inv_x = m->inv_y = 0;
    if (!s) return 0;
    char buf[64]; strncpy(buf, s, sizeof buf - 1); buf[sizeof buf - 1] = 0;
    for (char *t = strtok(buf, ",+ "); t; t = strtok(NULL, ",+ ")) {
        if (!strcmp(t, "swap")) m->swap_xy = 1; else if (!strcmp(t, "invx")) m->inv_x = 1; else if (!strcmp(t, "invy")) m->inv_y = 1;
        else if (!strcmp(t, "none") || !strcmp(t, "0")) {} else return -1;
    }
    return 0;
}
