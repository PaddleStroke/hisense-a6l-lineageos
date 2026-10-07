// SPDX-License-Identifier: Apache-2.0
/* einklock_logic.c — see einklock_logic.h. Pure logic of the e-ink lock screen: formatting, software renderer, minute
 * schedule, lock state machine. */
#include "einklock_logic.h"
#include "einklock_font.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* ---------------- configuration ---------------- */
void elk_default_cfg(struct elk_cfg *c) {
    memset(c, 0, sizeof *c);
    c->enabled = 1; c->clock = 1; c->battery = 1; c->use24 = -1; c->clean_min = 60; c->bg = ELK_BG_WHITE;
}
int elk_parse_bg(const char *v) {
    if (!v) return ELK_BG_WHITE;
    if (!strcmp(v, "black")) return ELK_BG_BLACK;
    if (!strcmp(v, "image") || !strcmp(v, "lcd")) return ELK_BG_IMAGE;
    return ELK_BG_WHITE;
}
int elk_cfg_equal(const struct elk_cfg *a, const struct elk_cfg *b) {
    return a->enabled == b->enabled && a->clock == b->clock && a->battery == b->battery && a->use24 == b->use24 &&
           a->clean_min == b->clean_min && a->bg == b->bg && a->stale == b->stale && !strcmp(a->locale, b->locale) && !strcmp(a->msg, b->msg);
}
int elk_rtc_learn(int state, int *ontime, double lateness_s, double late_s) {
    if (lateness_s > late_s) { *ontime = 0; return ELK_RTC_LATE; }
    if (++*ontime >= 2) return ELK_RTC_OK;
    return state;
}
int elk_stale_variant(const char *mode, int rtc_state) {
    if (mode && !strcmp(mode, "live")) return 0;
    if (mode && !strcmp(mode, "updated")) return 1;
    return rtc_state == ELK_RTC_LATE;
}

/* ---------------- formatting ---------------- */
static void lang_of(const char *locale, char lang[3], char region[3]) {
    lang[0] = lang[1] = lang[2] = 0; region[0] = region[1] = region[2] = 0;
    if (!locale) return;
    for (int i = 0; i < 2 && locale[i]; i++) lang[i] = (char)(locale[i] | 0x20);
    const char *r = strpbrk(locale, "-_");
    if (r) for (int i = 0; i < 2 && r[1 + i]; i++) region[i] = (char)(r[1 + i] & ~0x20);
}
int elk_locale_24h_default(const char *locale) {
    char l[3], r[3]; lang_of(locale, l, r);
    if (!strcmp(l, "en")) return !strcmp(r, "GB") || !strcmp(r, "IE");	/* en-US, en-CA, en-AU, en-IN ... use 12 h */
    if (!strcmp(l, "es") && (!strcmp(r, "US") || !strcmp(r, "MX"))) return 0;
    return 1;
}
void elk_format_time(char *out, size_t n, char *ampm, size_t an, const struct tm *tm, int use24) {
    if (an) ampm[0] = 0;
    if (use24) { snprintf(out, n, "%02d:%02d", tm->tm_hour, tm->tm_min); return; }
    int h = tm->tm_hour % 12; if (!h) h = 12;
    snprintf(out, n, "%d:%02d", h, tm->tm_min);
    if (an) snprintf(ampm, an, "%s", tm->tm_hour < 12 ? "AM" : "PM");
}
struct lang_names { const char *lang; const char *day[7]; const char *mon[12]; };
static const struct lang_names NAMES[] = {
    {"en", {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"},
           {"January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December"}},
    {"fr", {"dimanche", "lundi", "mardi", "mercredi", "jeudi", "vendredi", "samedi"},
           {"janvier", "f\xc3\xa9vrier", "mars", "avril", "mai", "juin", "juillet", "ao\xc3\xbbt", "septembre", "octobre", "novembre", "d\xc3\xa9" "cembre"}},
    {"de", {"Sonntag", "Montag", "Dienstag", "Mittwoch", "Donnerstag", "Freitag", "Samstag"},
           {"Januar", "Februar", "M\xc3\xa4rz", "April", "Mai", "Juni", "Juli", "August", "September", "Oktober", "November", "Dezember"}},
    {"es", {"domingo", "lunes", "martes", "mi\xc3\xa9rcoles", "jueves", "viernes", "s\xc3\xa1" "bado"},
           {"enero", "febrero", "marzo", "abril", "mayo", "junio", "julio", "agosto", "septiembre", "octubre", "noviembre", "diciembre"}},
    {"it", {"domenica", "luned\xc3\xac", "marted\xc3\xac", "mercoled\xc3\xac", "gioved\xc3\xac", "venerd\xc3\xac", "sabato"},
           {"gennaio", "febbraio", "marzo", "aprile", "maggio", "giugno", "luglio", "agosto", "settembre", "ottobre", "novembre", "dicembre"}},
    {"pt", {"domingo", "segunda-feira", "ter\xc3\xa7" "a-feira", "quarta-feira", "quinta-feira", "sexta-feira", "s\xc3\xa1" "bado"},
           {"janeiro", "fevereiro", "mar\xc3\xa7o", "abril", "maio", "junho", "julho", "agosto", "setembro", "outubro", "novembro", "dezembro"}},
};
void elk_format_date(char *out, size_t n, const struct tm *tm, const char *locale) {
    char l[3], r[3]; lang_of(locale, l, r);
    int wd = tm->tm_wday % 7, mo = tm->tm_mon % 12, d = tm->tm_mday;
    if (wd < 0 || mo < 0) { snprintf(out, n, "?"); return; }
    for (size_t i = 0; i < sizeof NAMES / sizeof NAMES[0]; i++) {
        const struct lang_names *L = &NAMES[i];
        if (strcmp(l, L->lang) && !(i == 0 && !l[0])) continue;
        if (!strcmp(L->lang, "en")) snprintf(out, n, "%s, %s %d", L->day[wd], L->mon[mo], d);
        else if (!strcmp(L->lang, "fr")) snprintf(out, n, d == 1 ? "%s %der %s" : "%s %d %s", L->day[wd], d, L->mon[mo]);
        else if (!strcmp(L->lang, "de")) snprintf(out, n, "%s, %d. %s", L->day[wd], d, L->mon[mo]);
        else if (!strcmp(L->lang, "it")) snprintf(out, n, "%s %d %s", L->day[wd], d, L->mon[mo]);
        else snprintf(out, n, "%s, %d de %s", L->day[wd], d, L->mon[mo]);	/* es, pt */
        return;
    }
    snprintf(out, n, "%04d-%02d-%02d", tm->tm_year + 1900, mo + 1, d);
}
void elk_format_updated(char *out, size_t n, const char *time, const char *locale) {
    char l[3], r[3]; lang_of(locale, l, r);
    const char *p = !strcmp(l, "fr") ? "Mis \xc3\xa0 jour \xc3\xa0 " : !strcmp(l, "de") ? "Aktualisiert um " : !strcmp(l, "es") ? "Actualizado a las "
                  : !strcmp(l, "it") ? "Aggiornato alle " : !strcmp(l, "pt") ? "Atualizado \xc3\xa0s " : "Updated ";
    snprintf(out, n, "%s%s", p, time);
}
void elk_format_battery(char *out, size_t n, int pct, const char *locale) {
    char l[3], r[3]; lang_of(locale, l, r);
    if (pct < 0) { if (n) out[0] = 0; return; }
    if (pct > 100) pct = 100;
    if (!strcmp(l, "fr") || !strcmp(l, "de")) snprintf(out, n, "%d\xc2\xa0%%", pct);	/* NBSP before % */
    else snprintf(out, n, "%d%%", pct);
}

/* ---------------- UTF-8 + glyphs ---------------- */
size_t elk_utf8_next(const char *s, uint32_t *cp) {
    const unsigned char *u = (const unsigned char *)s;
    if (!u[0]) { *cp = 0; return 0; }
    if (u[0] < 0x80) { *cp = u[0]; return 1; }
    int len = (u[0] & 0xe0) == 0xc0 ? 2 : (u[0] & 0xf0) == 0xe0 ? 3 : (u[0] & 0xf8) == 0xf0 ? 4 : 0;
    if (!len) { *cp = 0xfffd; return 1; }
    uint32_t v = u[0] & (0x7f >> len);
    for (int i = 1; i < len; i++) { if ((u[i] & 0xc0) != 0x80) { *cp = 0xfffd; return 1; } v = (v << 6) | (u[i] & 0x3f); }
    *cp = v; return (size_t)len;
}
static const struct elk_font *face(int f) { return f == 0 ? &elk_font_clock : f == 1 ? &elk_font_text : &elk_font_small; }
static const struct elk_glyph *glyph(const struct elk_font *F, uint32_t cp) {
    int lo = 0, hi = F->n - 1;
    while (lo <= hi) { int m = (lo + hi) / 2; if (F->g[m].cp == cp) return &F->g[m]; if (F->g[m].cp < cp) lo = m + 1; else hi = m - 1; }
    return cp != '?' ? glyph(F, '?') : NULL;
}
int elk_text_width(int f, const char *s) {
    const struct elk_font *F = face(f); int w = 0; uint32_t cp; size_t k;
    while ((k = elk_utf8_next(s, &cp))) { s += k; const struct elk_glyph *g = glyph(F, cp); if (g) w += g->adv; }
    return w;
}

/* ---------------- canvas operations ---------------- */
/* Text is drawn into an alpha mask first (one band of the canvas), then optionally dilated into a halo of the opposite
 * colour (readability over a picture), then both are blended into the canvas. Static scratch: single-threaded use. */
#define BAND_H 320
static uint8_t s_mask[ELK_W * BAND_H], s_halo[ELK_W * BAND_H];
static void mask_glyph(int band_y, const struct elk_glyph *g, int x, int base) {
    int x0 = x + g->xoff, y0 = base + g->yoff - band_y;
    const uint8_t *p = elk_font_data + g->off, *e = p + g->len; int i = 0, n = g->w * g->h;
    while (p < e && i < n) {
        int run = (*p >> 4) + 1, a = (*p & 15) * 17; p++;
        for (; run > 0 && i < n; run--, i++) {
            if (!a) continue;
            int xx = x0 + i % g->w, yy = y0 + i / g->w;
            if (xx < 0 || xx >= ELK_W || yy < 0 || yy >= BAND_H) continue;
            uint8_t *m = &s_mask[yy * ELK_W + xx]; if (*m < a) *m = (uint8_t)a;
        }
    }
}
static void mask_text(int band_y, int f, const char *s, int x, int base) {
    const struct elk_font *F = face(f); uint32_t cp; size_t k;
    while ((k = elk_utf8_next(s, &cp))) { s += k; const struct elk_glyph *g = glyph(F, cp); if (!g) continue; if (g->w) mask_glyph(band_y, g, x, base); x += g->adv; }
}
static void blend_band(uint8_t *canvas, int band_y, const uint8_t *m, int colour) {
    for (int y = 0; y < BAND_H; y++) {
        int cy = band_y + y; if (cy < 0 || cy >= ELK_H) continue;
        for (int x = 0; x < ELK_W; x++) { int a = m[y * ELK_W + x]; if (!a) continue;
            uint8_t *p = &canvas[cy * ELK_W + x]; *p = (uint8_t)((*p * (255 - a) + colour * a + 127) / 255); }
    }
}
static void dilate(int r) {	/* s_halo = max over a disc of radius r of s_mask */
    memset(s_halo, 0, sizeof s_halo);
    for (int y = 0; y < BAND_H; y++) for (int x = 0; x < ELK_W; x++) { int a = s_mask[y * ELK_W + x]; if (!a) continue;
        for (int dy = -r; dy <= r; dy++) { int yy = y + dy; if (yy < 0 || yy >= BAND_H) continue;
            for (int dx = -r; dx <= r; dx++) { int xx = x + dx; if (xx < 0 || xx >= ELK_W || dx * dx + dy * dy > r * r) continue;
                uint8_t *h = &s_halo[yy * ELK_W + xx]; if (*h < a) *h = (uint8_t)a; } } }
}
static void band_flush(uint8_t *canvas, int band_y, int fg, int halo) {
    if (halo) { dilate(3); blend_band(canvas, band_y, s_halo, 255 - fg); }
    blend_band(canvas, band_y, s_mask, fg);
    memset(s_mask, 0, sizeof s_mask);
}
static void fill_rect(uint8_t *c, int x0, int y0, int x1, int y1, int v) {
    for (int y = y0 < 0 ? 0 : y0; y < y1 && y < ELK_H; y++) for (int x = x0 < 0 ? 0 : x0; x < x1 && x < ELK_W; x++) c[y * ELK_W + x] = (uint8_t)v;
}
static int mean_rect(const uint8_t *c, int x0, int y0, int x1, int y1) {
    unsigned long long s = 0, n = 0;
    for (int y = y0; y < y1; y++) for (int x = x0; x < x1; x++) { s += c[y * ELK_W + x]; n++; }
    return n ? (int)(s / n) : 255;
}
/* battery outline 46x24 + nub, fill proportional; charging = bolt cut out of the fill (opposite colour) */
static void battery_icon(uint8_t *c, int x, int ybase, int pct, int charging, int fg) {
    int w = 46, h = 24, t = 3, y = ybase - h;
    fill_rect(c, x, y, x + w, y + t, fg); fill_rect(c, x, y + h - t, x + w, y + h, fg);
    fill_rect(c, x, y, x + t, y + h, fg); fill_rect(c, x + w - t, y, x + w, y + h, fg);
    fill_rect(c, x + w, y + 7, x + w + 4, y + h - 7, fg);
    int inner = w - 2 * t - 4, f = pct < 0 ? 0 : (inner * (pct > 100 ? 100 : pct) + 50) / 100;
    fill_rect(c, x + t + 2, y + t + 2, x + t + 2 + f, y + h - t - 2, fg);
    if (charging) {	/* bolt: two triangles, drawn in the opposite colour over the fill, outlined in fg where empty */
        static const int bx[2][3] = {{28, 13, 24}, {21, 33, 17}}, by[2][3] = {{2, 14, 14}, {10, 10, 22}};
        for (int k = 0; k < 2; k++)
            for (int yy = 0; yy < h; yy++) for (int xx = 0; xx < w; xx++) {
                int s1 = (bx[k][1] - bx[k][0]) * (yy - by[k][0]) - (by[k][1] - by[k][0]) * (xx - bx[k][0]);
                int s2 = (bx[k][2] - bx[k][1]) * (yy - by[k][1]) - (by[k][2] - by[k][1]) * (xx - bx[k][1]);
                int s3 = (bx[k][0] - bx[k][2]) * (yy - by[k][2]) - (by[k][0] - by[k][2]) * (xx - bx[k][2]);
                if ((s1 >= 0 && s2 >= 0 && s3 >= 0) || (s1 <= 0 && s2 <= 0 && s3 <= 0)) {
                    uint8_t *p = &c[(y + yy) * ELK_W + x + xx]; *p = (uint8_t)(*p == fg ? 255 - fg : fg); }
            }
    }
}

/* ---------------- lock screen layout (portrait 720x1440, e-ink orientation of the mirror) ---------------- */
#define TIME_BASE 470
#define DATE_BASE 560
#define MSG_BASE 1250
#define BATT_BASE 1392
#define BATT_X 36
void elk_render(uint8_t *canvas, const uint8_t *bg, const struct elk_cfg *c, const struct elk_info *in, struct elk_layout *lay) {
    struct elk_layout L; memset(&L, 0, sizeof L);
    if (c->bg == ELK_BG_IMAGE && bg) memcpy(canvas, bg, (size_t)ELK_W * ELK_H);
    else memset(canvas, c->bg == ELK_BG_BLACK ? 0 : 255, (size_t)ELK_W * ELK_H);
    int halo = c->bg == ELK_BG_IMAGE && bg;
    int fg = mean_rect(canvas, 40, 240, ELK_W - 40, 600) >= 128 ? 0 : 255;	/* stock: isWhiteAreaOfView -> black/white text */
    L.fg = fg;
    if (c->clock && c->stale) {	/* RTC wake not working: the date big, the time small as "updated HH:MM" (never a stale big clock) */
        char t[16], ap[8], t2[24], u[96], d[96];
        elk_format_time(t, sizeof t, ap, sizeof ap, &in->tm, c->use24 >= 0 ? c->use24 : elk_locale_24h_default(c->locale));
        snprintf(t2, sizeof t2, "%s%s%s", t, ap[0] ? " " : "", ap);
        elk_format_updated(u, sizeof u, t2, c->locale); elk_format_date(d, sizeof d, &in->tm, c->locale);
        int by = TIME_BASE - (BAND_H - 40), dw = elk_text_width(1, d), dx = (ELK_W - dw) / 2;
        mask_text(by, 1, d, dx, TIME_BASE); band_flush(canvas, by, fg, halo);
        L.date = (struct elk_box){dx, TIME_BASE - elk_font_text.ascent, dx + dw, TIME_BASE + elk_font_text.descent};
        int uw = elk_text_width(2, u), ux = (ELK_W - uw) / 2; by = DATE_BASE - (BAND_H - 40);
        mask_text(by, 2, u, ux, DATE_BASE); band_flush(canvas, by, fg, halo);
        L.time = (struct elk_box){ux, DATE_BASE - elk_font_small.ascent, ux + uw, DATE_BASE + elk_font_small.descent};
    } else if (c->clock) {
        char t[16], ap[8]; elk_format_time(t, sizeof t, ap, sizeof ap, &in->tm, c->use24 >= 0 ? c->use24 : elk_locale_24h_default(c->locale));
        int tw = elk_text_width(0, t), aw = ap[0] ? 14 + elk_text_width(2, ap) : 0, x = (ELK_W - tw - aw) / 2;
        int by = TIME_BASE - (BAND_H - 40);
        mask_text(by, 0, t, x, TIME_BASE);
        if (ap[0]) mask_text(by, 2, ap, x + tw + 14, TIME_BASE);
        band_flush(canvas, by, fg, halo);
        L.time = (struct elk_box){x, TIME_BASE - elk_font_clock.ascent, x + tw + aw, TIME_BASE + 8};
        char d[96]; elk_format_date(d, sizeof d, &in->tm, c->locale);
        int dw = elk_text_width(1, d), dx = (ELK_W - dw) / 2; by = DATE_BASE - (BAND_H - 40);
        mask_text(by, 1, d, dx, DATE_BASE); band_flush(canvas, by, fg, halo);
        L.date = (struct elk_box){dx, DATE_BASE - elk_font_text.ascent, dx + dw, DATE_BASE + elk_font_text.descent};
    }
    int fgb = halo ? (mean_rect(canvas, 20, 1180, ELK_W - 20, 1420) >= 128 ? 0 : 255) : fg;
    if (c->msg[0]) {	/* owner line: up to two lines, wrapped at spaces, 640 px */
        char line[2][96] = {"", ""}; const char *s = c->msg; int li = 0;
        while (*s && li < 2) {
            char w[96]; int wl = 0; while (*s == ' ') s++;
            while (s[wl] && s[wl] != ' ' && wl < 95) wl++;
            memcpy(w, s, (size_t)wl); w[wl] = 0; if (!wl) break;
            char cand[192]; snprintf(cand, sizeof cand, "%s%s%s", line[li], line[li][0] ? " " : "", w);
            if (elk_text_width(2, cand) <= 640 || !line[li][0]) { snprintf(line[li], sizeof line[li], "%.95s", cand); s += wl; }
            else li++;
        }
        int by = MSG_BASE - (BAND_H - 100);
        for (int i = 0; i < 2; i++) if (line[i][0]) mask_text(by, 2, line[i], (ELK_W - elk_text_width(2, line[i])) / 2, MSG_BASE + i * 46);
        band_flush(canvas, by, fgb, halo);
        L.msg = (struct elk_box){40, MSG_BASE - elk_font_small.ascent, ELK_W - 40, MSG_BASE + 46 + elk_font_small.descent};
    }
    if (c->battery && in->battery_pct >= 0) {	/* bottom-left corner (user requirement; time/date stay the primary content) */
        char b[16]; elk_format_battery(b, sizeof b, in->battery_pct, c->locale);
        int bw = elk_text_width(2, b), total = 50 + 14 + bw, x = BATT_X;
        if (halo) fill_rect(canvas, x - 10, BATT_BASE - 40, x + total + 10, BATT_BASE + 14, 255 - fgb);	/* plain chip behind the battery */
        battery_icon(canvas, x, BATT_BASE - 2, in->battery_pct, in->charging, fgb);
        int by = BATT_BASE - (BAND_H - 40);
        mask_text(by, 2, b, x + 64, BATT_BASE); band_flush(canvas, by, fgb, 0);
        L.battery = (struct elk_box){x, BATT_BASE - 40, x + total, BATT_BASE + 14};
    }
    if (lay) *lay = L;
}

/* ---------------- background picture ---------------- */
static int pgm_tok(const uint8_t *b, size_t n, size_t *i, long *seq) {
    for (;;) {
        while (*i < n && (b[*i] == ' ' || b[*i] == '\t' || b[*i] == '\n' || b[*i] == '\r')) (*i)++;
        if (*i < n && b[*i] == '#') {
            size_t s = *i; while (*i < n && b[*i] != '\n') (*i)++;
            char cm[64]; size_t l = *i - s < sizeof cm - 1 ? *i - s : sizeof cm - 1; memcpy(cm, b + s, l); cm[l] = 0;
            long v; if (seq && sscanf(cm, "# a6l-lock seq=%ld", &v) == 1) *seq = v;
            continue;
        }
        break;
    }
    if (*i >= n || b[*i] < '0' || b[*i] > '9') return -1;
    long v = 0; while (*i < n && b[*i] >= '0' && b[*i] <= '9') { v = v * 10 + (b[*i] - '0'); (*i)++; if (v > 100000) return -1; }
    return (int)v;
}
int elk_parse_pgm(const uint8_t *buf, size_t n, uint8_t *out, long *seq) {
    if (seq) *seq = -1;
    if (n < 2 || buf[0] != 'P' || buf[1] != '5') return -1;
    size_t i = 2; int w = pgm_tok(buf, n, &i, seq), h = pgm_tok(buf, n, &i, seq), mx = pgm_tok(buf, n, &i, seq);
    if (w != ELK_W || h != ELK_H || mx != 255 || i >= n) return -1;
    i++;	/* exactly one whitespace byte after maxval */
    if (n - i != (size_t)ELK_W * ELK_H) return -1;
    memcpy(out, buf + i, (size_t)ELK_W * ELK_H); return 0;
}

/* ---------------- minute schedule ---------------- */
long long elk_display_time(double now, int period_s, double lead_s) {
    if (period_s <= 0) period_s = 60;
    return (long long)floor((now + lead_s + 0.002) / period_s) * period_s;
}
long long elk_next_target(double now, int period_s, double lead_s) {
    if (period_s <= 0) period_s = 60;
    return elk_display_time(now, period_s, lead_s) + period_s;
}

/* ---------------- lock state machine ---------------- */
void elk_sm_init(struct elk_sm *s) { memset(s, 0, sizeof *s); }
struct elk_act elk_step(struct elk_sm *s, const struct elk_in *in) {
    struct elk_act a; memset(&a, 0, sizeof a);
    int want = in->enabled && (in->asleep_eink || in->lcd_idle), awake_eink = in->on_eink && !in->asleep_eink;
    a.arm = in->enabled && in->clock && (in->on_eink || in->lcd_idle);	/* armed on the e-ink even awake: a sleep missed by the poll is caught */
    if (!s->locked) {
        if (s->restore_pending) {	/* eink-round5: the covered page returns only on the e-ink, 1 s after the wake-up */
            if ((in->asleep_eink || in->lcd_idle) && !in->enabled) { a.restore = 1; s->restore_pending = 0; }	/* eink-round6: also LCD mode */
            else if (awake_eink) {
                if (s->eink_since <= 0) s->eink_since = in->now;
                if (in->now - s->eink_since >= ELK_RESTORE_DELAY_S) { a.restore = 1; s->restore_pending = 0; s->eink_since = 0; }
            } else if (s->eink_since > 0 && s->eink_since <= in->now) s->eink_since = 0;	/* (a retry backoff stays) */
        }
        if (!want) { s->asleep_since = 0; return a; }
        if (s->asleep_since <= 0 || in->resumed) s->asleep_since = in->now;	/* a suspend inside the entry window restarts it */
        a.hold = 1;
        if (in->now - s->asleep_since < (in->asleep_eink ? ELK_ENTRY_DELAY_S : ELK_LCD_ENTRY_DELAY_S)) return a;
        if (!in->asleep_eink && in->settling) return a;	/* eink-round6: LCD mode: not during an LCD power change */
        s->locked = 1; s->drew = 0; s->frames = 0; s->last_clean = in->now; s->restore_pending = 0; s->eink_since = 0; s->deferred = 0;
        a.restore = 0; a.draw = 1; return a;
    }
    if (!want) {	/* woke up, left the e-ink, or the lock screen was disabled */
        if (s->drew) { s->restore_pending = 1; s->eink_since = awake_eink ? in->now : 0; }
        s->locked = 0; s->drew = 0; s->asleep_since = 0; s->frames = 0; s->deferred = 0;
        if (s->restore_pending && (in->asleep_eink || in->lcd_idle) && !in->enabled) { a.restore = 1; s->restore_pending = 0; }	/* disabled while not in use */
        return a;
    }
    int tick = (in->timer_fired && in->clock) || s->deferred == 2;	/* eink-round6: a deferred tick stays a tick */
    if (tick || in->changed || s->deferred) {
        if (in->settling) { s->deferred = tick ? 2 : s->deferred ? s->deferred : 1; return a; }	/* drawn once the display transition settled */
        a.draw = 1; s->deferred = 0;
        a.force = tick && in->clean_min > 0 && in->now - s->last_clean >= in->clean_min * 60.0 - 1.0;
    }
    return a;
}
void elk_sm_abort(struct elk_sm *s) { if (!s->frames) { s->locked = 0; s->drew = 0; s->asleep_since = 0; } }
void elk_restore_failed(struct elk_sm *s, double now) { s->restore_pending = 1; s->eink_since = now + 4.0; }
void elk_sm_sent(struct elk_sm *s, const struct elk_act *a, double now) {
    if (!a->draw) return;
    s->drew = 1; s->frames++;
    if (a->force) s->last_clean = now;
}
