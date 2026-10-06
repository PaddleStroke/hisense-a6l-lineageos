// SPDX-License-Identifier: Apache-2.0
/* eink_logic.c — see eink_logic.h. Policy numbers come from the M1 mirror (docs/eink-mirror-milestone-20260923.md)
 * and the stock EpdManagerService/libtcon_eink disassembly (docs/eink-android-integration-20260923.md). */
#include "eink_logic.h"
#include <stdlib.h>
#include <string.h>

void pol_default_cfg(struct pol_cfg *c) {
    c->quiet_ms = 300; c->settle_ms = 1000; c->min_gap_ms = 150; c->clear_every = 10; c->max_per_min = 120;
    c->active_mode = POL_FASTEST; c->fixed_fast = 0; c->reading = 0; c->reading_refresh_every = 8; c->reading_full_frac = 0.6;
    c->stock = 0; c->release_quiet_ms = 90; c->release_max_ms = 700;
}
void pol_init(struct pol_state *s, const struct pol_cfg *c, double now) {
    memset(s, 0, sizeof *s); s->cfg = *c; s->win_t = now; s->done_t = now - 10; s->last_change = now - 10;
}
int pol_in_burst(const struct pol_state *s) { return s->burst; }
void pol_gesture_released(struct pol_state *s, double now) { s->release_t = now; }

struct pol_action pol_step(struct pol_state *s, double now, double moving, double vs_panel, int busy) {
    struct pol_action a = {POL_NONE, NULL};
    const struct pol_cfg *c = &s->cfg;
    if (moving > 0) { s->last_change = now; s->consec++; } else s->consec = 0;
    if (now - s->win_t >= 60) { s->win_t = now; s->win_n = 0; }
    double quiet_ms = (now - s->last_change) * 1000;
    int can = !busy && (now - s->done_t) * 1000 >= c->min_gap_ms && s->win_n < c->max_per_min;
    if (!can) return a;
    /* eink-round2-20261006 (filmed): right after a drag release the first capture can still show the drag position
     * (the app has not drawn its settled frame yet); one REGAL cycle (~1.5 s) then shows a half-swiped home page or a
     * half-pulled shade. Wait for one unchanged capture pair after the release, bounded by release_max_ms. */
    if (s->release_t > 0 && c->release_quiet_ms > 0) {
        double since_ms = (now - s->release_t) * 1000;
        if (since_ms < c->release_max_ms && (since_ms < c->release_quiet_ms || quiet_ms < c->release_quiet_ms)) return a;
    }
    if (c->stock) {
        /* Stock A6L (filmed 2026-10-06): one REGAL update (39 frames, only changed pixels driven, white never
         * flashes) per change, of the newest capture, as soon as the panel is free. While scrolling this coalesces
         * to ~2 grey updates/s, like stock. Ghost cleanup = forced REGAL (mode kept: no 79-frame transition) of the
         * unchanged page after clear_every updates, deferred until the page has been still for settle_ms. */
        if (vs_panel > 0 && quiet_ms >= c->quiet_ms) { a.kind = POL_SHOW; a.mode = POL_READING; }
        else if (vs_panel == 0 && c->clear_every > 0 && s->reading_n >= c->clear_every && quiet_ms >= c->settle_ms) {
            a.kind = POL_REFRESH; a.mode = POL_READING;
        }
        return a;
    }
    if (c->fixed_fast) {
        if (vs_panel > 0) {
            a.kind = POL_SHOW; a.mode = c->active_mode;
            if (c->clear_every > 0 && (s->fast_n + 1) % c->clear_every == 0) a.kind = POL_CLEAN_SHOW;
        }
        return a;
    }
    if (c->reading) {
        /* reading mode: never chase motion; one update when the page is still, REGAL (partial, no flash) for small
         * changes, GC16 for page turns; every N partial updates a forced GC16 refresh of the same picture */
        if (vs_panel > 0 && quiet_ms >= c->settle_ms) {
            a.kind = POL_SHOW; a.mode = vs_panel > c->reading_full_frac || !s->primed ? POL_QUALITY : POL_READING;
            if (!strcmp(a.mode, POL_QUALITY) && c->clear_every > 0 && (s->clean_n + 1) % (c->clear_every * 2) == 0) a.kind = POL_CLEAN_SHOW;
        } else if (vs_panel == 0 && s->reading_n >= c->reading_refresh_every && quiet_ms >= c->settle_ms) {
            a.kind = POL_REFRESH; a.mode = POL_QUALITY;
        }
        return a;
    }
    if (vs_panel > 0 && quiet_ms >= (s->burst ? c->settle_ms : c->quiet_ms)) { a.kind = POL_SHOW; a.mode = POL_QUALITY; }
    else if (vs_panel > 0 && (s->burst || s->consec >= 2)) { a.kind = POL_SHOW; a.mode = c->active_mode; }
    else if (vs_panel == 0 && s->fast_on_panel && quiet_ms >= c->settle_ms) { a.kind = POL_REFRESH; a.mode = POL_QUALITY; }
    if (a.kind == POL_SHOW && !strcmp(a.mode, POL_QUALITY) && c->clear_every > 0 && (s->clean_n + 1) % c->clear_every == 0)
        a.kind = POL_CLEAN_SHOW;
    return a;
}
void pol_sent(struct pol_state *s, const struct pol_action *a, double now) {
    (void)now;
    int clean = a->kind == POL_CLEAN_SHOW || (a->mode && strcmp(a->mode, s->cfg.active_mode) != 0);
    if (s->cfg.reading) clean = 1;
    s->win_n++;
    s->primed = 1;
    if (s->cfg.stock) {	/* REGAL history: count partial updates; the forced cleanup resets the count */
        if (a->kind == POL_REFRESH || a->kind == POL_CLEAN_SHOW) s->reading_n = 0; else s->reading_n++;
        s->fast_on_panel = 0; s->burst = 0; return;
    }
    if (s->cfg.fixed_fast && a->kind != POL_REFRESH) s->fast_n++;
    if (a->kind == POL_REFRESH) { s->fast_on_panel = 0; s->reading_n = 0; s->burst = 0; return; }
    if (clean) {
        /* Ordinary Auto REGAL is settled gray history, not a full quality
         * cleanup. No partial-count forced refresh is added to Auto. */
        if(!s->cfg.reading&&a->mode&&!strcmp(a->mode,POL_READING)) {
            s->fast_on_panel=0;s->burst=0;return;
        }
        s->clean_n++; s->fast_on_panel = 0; s->burst = 0;
        if (s->cfg.reading) { if (!strcmp(a->mode, POL_READING)) s->reading_n++; else s->reading_n = 0; }
    } else { s->fast_on_panel = 1; s->burst = 1; }
}
void pol_done(struct pol_state *s, double now) { s->done_t = now; }

int pol_apply_refresh_mode(struct pol_cfg *c, const char *m) {
    if (!m || !*m) return -1;
    struct pol_cfg d; pol_default_cfg(&d);
    d.clear_every = c->clear_every; d.max_per_min = c->max_per_min; d.min_gap_ms = c->min_gap_ms;
    d.release_quiet_ms = c->release_quiet_ms; d.release_max_ms = c->release_max_ms;
    if (!strcmp(m, "auto")) { }
    else if (!strcmp(m, "quality")) { d.reading = 1; d.reading_full_frac = 0.0; d.settle_ms = 700; }
    else if (!strcmp(m, "partial") || !strcmp(m, "reading")) { d.reading = 1; }
    else if (!strcmp(m, "fast")) { d.active_mode = POL_FAST; d.fixed_fast = 1; d.quiet_ms = 200; d.settle_ms = 800; }
    else if (!strcmp(m, "stock")) {
        /* min_gap 0 (eink-round2): a6l_epdd ACKs only after its 20 tail idle scans and the rail switch-off, so the
         * panel is already free; the extra 150 ms only lengthened the filmed 1.6 s scroll update cycle (stock ~0.3 s) */
        d.stock = 1; d.active_mode = POL_READING; d.quiet_ms = 0; d.settle_ms = 1500; d.min_gap_ms = 0;
    }
    else if (!strcmp(m, "fastest")) { d.active_mode = POL_FASTEST; d.fixed_fast = 1; d.quiet_ms = 150; d.settle_ms = 600; }
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

void fit_geometry(int gw, int gh, int pw, int ph, int fit, int *ox, int *oy, int *dw, int *dh) {
    if (fit == FIT_STRETCH) { *ox = *oy = 0; *dw = pw; *dh = ph; return; }
    int crop = fit == FIT_CROP;
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

/* ---------------- 4. KMS plane composition (r5 review round4 F41) ---------------- */
int plane_supported(const struct plane_geo *q) {
    uint32_t rot = q->rotation & 0xf;
    if (q->rotation & ~0x3fu) return -1;
    if (rot != PLANE_ROT_0 && rot != PLANE_ROT_90 && rot != PLANE_ROT_180 && rot != PLANE_ROT_270) return -1;
    if (q->blend != PLANE_BLEND_NONE && q->blend != PLANE_BLEND_PREMULTI && q->blend != PLANE_BLEND_COVERAGE) return -1;
    if (q->alpha16 > 0xffff) return -1;
    return 0;
}
static uint8_t luma8(int r, int g, int b) { return (uint8_t)((r * 77 + g * 150 + b * 29) >> 8); }	/* = the mirror's luma() */
/* Conservative fast-path proof: this plane replaces every output pixel.
 * Pixel alpha is ignored by KMS NONE blending. Source bounds must be valid. */
int plane_opaque_fullscreen(const struct plane_geo *q, int fw, int fh, int gw, int gh) {
    return gw > 0 && gh > 0 && fw >= gw && fh >= gh &&
           q->cx == 0 && q->cy == 0 && q->cw == gw && q->ch == gh &&
           q->sx == 0 && q->sy == 0 && q->sw == gw && q->sh == gh &&
           q->rotation == PLANE_ROT_0 && q->alpha16 == 0xffff &&
           q->blend == PLANE_BLEND_NONE;
}

int plane_compose(uint8_t *gray, int gw, int gh, const struct plane_geo *q, const struct plane_fb *fb) {
    if (plane_supported(q) || q->cw <= 0 || q->ch <= 0) return -1;
    /* Common LCD scanout: unrotated, 1:1 pixels with full plane alpha. Avoid
     * floating-point geometry/blending per pixel, including ARGB planes whose
     * format has alpha even when their content is opaque. The integer blend
     * equations below round identically (255 is odd, so no half-way tie).
     * Retain the general path for scaling/rotation/global-alpha planes. */
    if (q->rotation == PLANE_ROT_0 && q->alpha16 == 0xffff &&
        q->sx >= 0 && q->sy >= 0 && q->sx == (int)q->sx && q->sy == (int)q->sy &&
        q->sw == q->cw && q->sh == q->ch && q->sx + q->cw <= fb->w && q->sy + q->ch <= fb->h) {
        int x0 = q->cx < 0 ? 0 : q->cx, y0 = q->cy < 0 ? 0 : q->cy;
        int x1 = q->cx + q->cw < gw ? q->cx + q->cw : gw;
        int y1 = q->cy + q->ch < gh ? q->cy + q->ch : gh;
        if (x0 >= x1 || y0 >= y1) return 0;
        for (int y = y0; y < y1; y++) {
            const uint8_t *s = fb->base + (size_t)(y - q->cy + (int)q->sy) * fb->pitch +
                               (size_t)(x0 - q->cx + (int)q->sx) * fb->bpp;
            uint8_t *d = gray + (size_t)y * gw + x0;
            for (int x = x0; x < x1; x++, s += fb->bpp, d++) {
                uint8_t lv;
                if (fb->bpp == 4) lv = fb->bgr ? luma8(s[2], s[1], s[0]) : luma8(s[0], s[1], s[2]);
                else { uint16_t v = (uint16_t)(s[0] | s[1] << 8); lv = luma8((v >> 11) << 3, ((v >> 5) & 63) << 2, (v & 31) << 3); }
                if (fb->bpp != 4 || !fb->has_alpha || q->blend == PLANE_BLEND_NONE || s[3] == 255) *d = lv;
                else if (q->blend == PLANE_BLEND_COVERAGE) *d = (uint8_t)((s[3] * lv + (255 - s[3]) * *d + 127) / 255);
                else { int v = lv + ((255 - s[3]) * *d + 127) / 255; *d = (uint8_t)(v > 255 ? 255 : v); }
            }
        }
        return 0;
    }
    const uint32_t rot = q->rotation & 0xf; const double pa = q->alpha16 / 65535.0;
    for (int y = q->cy < 0 ? 0 : q->cy; y < q->cy + q->ch && y < gh; y++) {
        for (int x = q->cx < 0 ? 0 : q->cx; x < q->cx + q->cw && x < gw; x++) {
            double u = (x - q->cx + 0.5) / q->cw, v = (y - q->cy + 0.5) / q->ch, xn, yn;	/* normalised destination */
            /* inverse of the counter-clockwise rotation, then of the reflection: normalised source coordinates */
            if (rot == PLANE_ROT_90) { xn = 1 - v; yn = u; }
            else if (rot == PLANE_ROT_180) { xn = 1 - u; yn = 1 - v; }
            else if (rot == PLANE_ROT_270) { xn = v; yn = 1 - u; }
            else { xn = u; yn = v; }
            if (q->rotation & PLANE_REFLECT_X) xn = 1 - xn;
            if (q->rotation & PLANE_REFLECT_Y) yn = 1 - yn;
            int sxx = (int)(q->sx + xn * q->sw), syy = (int)(q->sy + yn * q->sh);
            if (sxx < 0 || sxx >= fb->w || syy < 0 || syy >= fb->h) continue;
            const uint8_t *s = fb->base + (size_t)syy * fb->pitch + (size_t)sxx * fb->bpp; uint8_t lv; int a = 255;
            if (fb->bpp == 4) { lv = fb->bgr ? luma8(s[2], s[1], s[0]) : luma8(s[0], s[1], s[2]); if (fb->has_alpha) a = s[3]; }
            else { uint16_t w16 = (uint16_t)(s[0] | s[1] << 8); lv = luma8((w16 >> 11) << 3, ((w16 >> 5) & 63) << 2, (w16 & 31) << 3); }
            uint8_t *d = &gray[(size_t)y * gw + x];
            double A = q->blend == PLANE_BLEND_NONE ? 1.0 : a / 255.0, o;
            if (q->blend == PLANE_BLEND_COVERAGE) o = pa * A * lv + (1 - pa * A) * *d;
            else o = pa * lv + (1 - pa * A) * *d;	/* None (A = 1) and pre-multiplied */
            int iv = (int)(o + 0.5); *d = (uint8_t)(iv > 255 ? 255 : iv < 0 ? 0 : iv);
        }
    }
    return 0;
}

/* ---------------- 5. tone curve (eink-round2) ---------------- */
/* x^g for x in (0,1], g > 0 without libm (the host tests and the NO_DRM mirror build link no -lm): ln via range reduction
 * to [0.5,1) and the atanh series, exp via range reduction by ln 2 and Taylor. Accurate far below one grey level. */
static double tone_pow(double x, double g) {
    if (x <= 0) return 0;
    if (x >= 1) return 1;
    const double LN2 = 0.69314718055994531;
    int k = 0; while (x < 0.5) { x *= 2; k++; }
    double z = (x - 1) / (x + 1), z2 = z * z, term = z, s = 0;
    for (int n = 1; n < 40; n += 2) { s += term / n; term *= z2; }
    double y = g * (2 * s - k * LN2);	/* <= 0 */
    int m = 0; while (y < -LN2) { y += LN2; m++; }
    double e = 1, t = 1; for (int n = 1; n < 20; n++) { t *= y / n; e += t; }
    while (m-- > 0) e *= 0.5;
    return e;
}
void tone_lut(uint8_t lut[256], int contrast, int black_clip, int white_clip, int gamma_x100) {
    if (contrast < 0) contrast = 0;
    if (contrast > 100) contrast = 100;
    if (gamma_x100 < 50) gamma_x100 = 50;
    if (gamma_x100 > 300) gamma_x100 = 300;
    int bp = contrast * 60 / 100, wp = 255 - contrast * 60 / 100;
    if (black_clip > bp) bp = black_clip;
    if (white_clip < wp) wp = white_clip;
    if (bp < 0) bp = 0;
    if (wp > 255) wp = 255;
    if (wp - bp < 32) { bp = contrast * 60 / 100; wp = 255 - contrast * 60 / 100; }	/* nonsense clips: ignore them */
    for (int i = 0; i < 256; i++) {
        int v;
        if (i <= bp) v = 0;
        else if (i >= wp) v = 255;
        else if (gamma_x100 == 100) v = (i - bp) * 255 / (wp - bp);	/* bit-identical to the pre-round2 linear LUT */
        else { v = (int)(255 * tone_pow((double)(i - bp) / (wp - bp), gamma_x100 / 100.0) + 0.5); if (v > 255) v = 255; }
        lut[i] = (uint8_t)v;
    }
}
