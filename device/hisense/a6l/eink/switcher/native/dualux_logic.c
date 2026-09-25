// SPDX-License-Identifier: Apache-2.0
/* dualux_logic.c — see dualux_logic.h (agent dualux, 25 Sep 2026; docs/dualux-20260925.md). */
#include "dualux_logic.h"
#include <math.h>
#include <string.h>

void dx_default_cfg(struct dx_cfg *c) { c->long_ms = 800; c->power_long_ms = 450; c->ekey_in_eink = DX_EK_SLEEP; c->mirror_in_lcd = 0; c->double_ms = 300; }
void dx_init(struct dx_state *s, const struct dx_cfg *c, int screen, int awake) {
    memset(s, 0, sizeof *s); s->cfg = *c; s->screen = screen == DX_EINK ? DX_EINK : DX_LCD; s->awake = !!awake;
}
void dx_out_init(struct dx_out *o) { memset(o, 0, sizeof *o); o->set_screen = -1; }
void dx_apply(struct dx_state *s, const struct dx_out *o) { if (o->set_screen == DX_LCD || o->set_screen == DX_EINK) s->screen = o->set_screen; }
void dx_set_awake(struct dx_state *s, int awake) { s->awake = !!awake; }

int dx_power_grabbed(const struct dx_state *s) { return s->screen == DX_EINK && s->awake; }
int dx_front_touch_grabbed(const struct dx_state *s) { return s->screen == DX_EINK; }
int dx_lcd_blank(const struct dx_state *s) { return s->screen == DX_EINK; }
int dx_mirror_on(const struct dx_state *s) { return s->screen == DX_EINK || s->cfg.mirror_in_lcd; }
const char *dx_state_name(const struct dx_state *s) { return s->screen == DX_LCD ? "lcd" : s->awake ? "eink" : "eink-asleep"; }
int dx_screen_parse(const char *v, int def) {
    if (!v || !*v) return def;
    if (!strcmp(v, "eink") || !strcmp(v, "1") || !strcmp(v, "epd")) return DX_EINK;
    if (!strcmp(v, "lcd") || !strcmp(v, "0")) return DX_LCD;
    return def;
}

struct dx_out dx_eink_key(struct dx_state *s, int value, double now) {
    struct dx_out o; dx_out_init(&o);
    if (value == 2) return dx_tick(s, now);
    if (value == 1) {
        if (s->ek_down) return o;
        s->ek_down = 1; s->ek_t = now; s->ek_long = 0; s->ek_double = 0;
        if (s->ek_pending) { s->ek_pending = 0; s->ek_double = 1; o.clear = 1; }	/* double press on the e-ink: clear */
        return o;
    }
    if (!s->ek_down) return o;
    s->ek_down = 0;
    if (s->ek_long || s->ek_double) return o;		/* long press / double press already acted */
    if ((now - s->ek_t) * 1000 >= s->cfg.long_ms) { o.clear = 1; return o; }	/* released late, no tick in between */
    if (s->screen == DX_LCD) { o.set_screen = DX_EINK; o.wake = 1; return o; }	/* KEY_WAKEUP is a no-op when awake */
    if (!s->awake) { o.wake = 1; return o; }		/* e-ink asleep: wake on the e-ink */
    if (s->cfg.double_ms <= 0) { if (s->cfg.ekey_in_eink == DX_EK_CLEAR) o.clear = 1; else o.sleep = 1; return o; }
    s->ek_pending = 1; s->ek_pending_t = now;		/* single press: act once no second press came (dx_tick) */
    return o;
}

struct dx_out dx_power_key(struct dx_state *s, int value, double now) {
    struct dx_out o; dx_out_init(&o);
    if (value == 2) return dx_tick(s, now);
    if (value == 1) {
        if (s->pw_down) return o;
        s->pw_down = 1; s->pw_t = now; s->pw_injected = 0; s->pw_grabbed_at_down = dx_power_grabbed(s);
        /* not grabbed: Android gets this press itself. If the e-ink is the screen but Android sleeps, the press wakes
         * Android: switch to the LCD at once (stock: the power key wakes the primary screen) */
        if (!s->pw_grabbed_at_down && s->screen == DX_EINK && !s->awake) o.set_screen = DX_LCD;
        return o;
    }
    if (!s->pw_down) return o;
    s->pw_down = 0;
    if (!s->pw_grabbed_at_down) return o;
    if (s->pw_injected) { o.power_up = 1; s->pw_injected = 0; return o; }	/* long press was handed to Android */
    o.set_screen = DX_LCD;				/* short press on the e-ink: back to the LCD, no sleep */
    return o;
}

struct dx_out dx_tick(struct dx_state *s, double now) {
    struct dx_out o; dx_out_init(&o);
    if (s->ek_down && !s->ek_long && !s->ek_double && (now - s->ek_t) * 1000 >= s->cfg.long_ms) { s->ek_long = 1; o.clear = 1; }
    if (s->ek_pending && !s->ek_down && (now - s->ek_pending_t) * 1000 >= s->cfg.double_ms) {
        s->ek_pending = 0;
        if (s->screen == DX_EINK && s->awake) { if (s->cfg.ekey_in_eink == DX_EK_CLEAR) o.clear = 1; else o.sleep = 1; }
    }
    if (s->pw_down && s->pw_grabbed_at_down && !s->pw_injected && (now - s->pw_t) * 1000 >= s->cfg.power_long_ms) {
        s->pw_injected = 1; o.power_down = 1; }
    return o;
}

struct dx_out dx_request(struct dx_state *s, const char *req) {
    struct dx_out o; dx_out_init(&o);
    if (!req) return o;
    if (!strcmp(req, "eink")) { if (s->screen != DX_EINK) { o.set_screen = DX_EINK; o.wake = !s->awake; } }
    else if (!strcmp(req, "lcd")) { if (s->screen != DX_LCD) { o.set_screen = DX_LCD; o.wake = !s->awake; } }
    else if (!strcmp(req, "toggle")) { o.set_screen = s->screen == DX_EINK ? DX_LCD : DX_EINK; o.wake = !s->awake; }
    else if (!strcmp(req, "clear")) o.clear = 1;
    return o;
}

/* ---------------- brightness ---------------- */
#define HLG_A 0.17883277
#define HLG_B 0.28466892
#define HLG_C 0.55991073
double dx_hlg_oetf(double e) {		/* identical to drm_hwcomposer BacklightController::HlgOetf */
    if (e <= 0) return 0;
    if (e >= 1) return 1;
    double h = e * 12.0; return h <= 1.0 ? sqrt(h) * 0.5 : HLG_A * log(h - HLG_B) + HLG_C;
}
double dx_hlg_inv(double v) {
    if (v <= 0) return 0;
    if (v >= 1) return 1;
    double h = v <= 0.5 ? (2 * v) * (2 * v) : exp((v - HLG_C) / HLG_A) + HLG_B;
    double e = h / 12.0; return e < 0 ? 0 : e > 1 ? 1 : e;
}
double dx_lcd_to_linear(int v, int max, int hw_linear) {
    if (max <= 0 || v <= 0) return 0;
    double s = (double)v / max; if (s > 1) s = 1;
    return hw_linear ? s : dx_hlg_inv(s);
}
void dx_fl_default(struct dx_fl_cfg *c) { c->enable = 1; c->max_pct = 100; c->min_level = 1; c->gamma = 1.0; }
int dx_frontlight_level(const struct dx_state *s, const struct dx_fl_cfg *c, int v, int lcd_max, int hw_linear, int fl_max) {
    if (s->screen != DX_EINK || !s->awake || !c->enable || fl_max <= 0 || v <= 0) return 0;
    double b = dx_lcd_to_linear(v, lcd_max, hw_linear);
    if (c->gamma > 0 && c->gamma != 1.0) b = pow(b, c->gamma);
    int pct = c->max_pct < 0 ? 0 : c->max_pct > 100 ? 100 : c->max_pct;
    int lvl = (int)lround(b * fl_max * pct / 100.0);
    if (lvl < c->min_level) lvl = c->min_level;
    if (lvl > fl_max) lvl = fl_max;
    return pct == 0 ? 0 : lvl;
}
