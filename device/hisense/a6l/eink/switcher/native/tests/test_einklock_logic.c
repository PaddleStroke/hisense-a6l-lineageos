// SPDX-License-Identifier: Apache-2.0
/* Host unit tests of einklock_logic.c (e-ink lock screen): formatting, renderer properties (a minute change only touches
 * the time box -> REGAL drives few pixels), background picture parsing, minute schedule and the lock state machine. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../einklock_logic.h"

static int fails, checks;
#define EXPECT(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL "); printf(__VA_ARGS__); printf("\n"); } } while (0)
static struct tm mk(int y, int mo, int d, int wd, int h, int mi) { struct tm t; memset(&t, 0, sizeof t); t.tm_year = y - 1900; t.tm_mon = mo - 1; t.tm_mday = d; t.tm_wday = wd; t.tm_hour = h; t.tm_min = mi; return t; }
static uint8_t A[ELK_W * ELK_H], B[ELK_W * ELK_H], BG[ELK_W * ELK_H];

static void test_format(void) {
    char t[32], ap[8]; struct tm x = mk(2026, 10, 6, 2, 9, 41);
    elk_format_time(t, sizeof t, ap, sizeof ap, &x, 1); EXPECT(!strcmp(t, "09:41") && !ap[0], "24 h 09:41 (%s/%s)", t, ap);
    elk_format_time(t, sizeof t, ap, sizeof ap, &x, 0); EXPECT(!strcmp(t, "9:41") && !strcmp(ap, "AM"), "12 h 9:41 AM (%s %s)", t, ap);
    x.tm_hour = 0; x.tm_min = 5; elk_format_time(t, sizeof t, ap, sizeof ap, &x, 0); EXPECT(!strcmp(t, "12:05") && !strcmp(ap, "AM"), "12 h midnight (%s %s)", t, ap);
    x.tm_hour = 13; x.tm_min = 7; elk_format_time(t, sizeof t, ap, sizeof ap, &x, 0); EXPECT(!strcmp(t, "1:07") && !strcmp(ap, "PM"), "12 h 1:07 PM (%s %s)", t, ap);
    x.tm_hour = 12; elk_format_time(t, sizeof t, ap, sizeof ap, &x, 0); EXPECT(!strcmp(t, "12:07") && !strcmp(ap, "PM"), "12 h noon (%s %s)", t, ap);
    EXPECT(elk_locale_24h_default("fr-FR") == 1 && elk_locale_24h_default("en-US") == 0 && elk_locale_24h_default("en-GB") == 1 &&
           elk_locale_24h_default("es-MX") == 0 && elk_locale_24h_default("es-ES") == 1 && elk_locale_24h_default("") == 1, "locale 12/24 h defaults");
    char d[96]; struct tm o = mk(2026, 10, 6, 2, 17, 1);
    elk_format_date(d, sizeof d, &o, "fr-FR"); EXPECT(!strcmp(d, "mardi 6 octobre"), "fr date (%s)", d);
    elk_format_date(d, sizeof d, &o, "en-US"); EXPECT(!strcmp(d, "Tuesday, October 6"), "en date (%s)", d);
    elk_format_date(d, sizeof d, &o, "de-DE"); EXPECT(!strcmp(d, "Dienstag, 6. Oktober"), "de date (%s)", d);
    elk_format_date(d, sizeof d, &o, "es_ES"); EXPECT(!strcmp(d, "martes, 6 de octubre"), "es date (%s)", d);
    elk_format_date(d, sizeof d, &o, "ja-JP"); EXPECT(!strcmp(d, "2026-10-06"), "unknown language: ISO date (%s)", d);
    elk_format_date(d, sizeof d, &o, ""); EXPECT(!strcmp(d, "Tuesday, October 6"), "no locale: English (%s)", d);
    struct tm f1 = mk(2027, 2, 1, 1, 8, 0); elk_format_date(d, sizeof d, &f1, "fr-CA"); EXPECT(!strcmp(d, "lundi 1er f\xc3\xa9vrier"), "fr 1er + accent (%s)", d);
    elk_format_battery(d, sizeof d, 73, "fr-FR"); EXPECT(!strcmp(d, "73\xc2\xa0%"), "fr battery with NBSP");
    elk_format_battery(d, sizeof d, 73, "en-US"); EXPECT(!strcmp(d, "73%"), "en battery");
    elk_format_battery(d, sizeof d, -1, "en-US"); EXPECT(!d[0], "unknown battery: empty");
    uint32_t cp; EXPECT(elk_utf8_next("\xc3\xa9", &cp) == 2 && cp == 0xe9, "utf8 2-byte");
    EXPECT(elk_utf8_next("\xff", &cp) == 1 && cp == 0xfffd, "utf8 invalid byte");
    EXPECT(elk_utf8_next("", &cp) == 0, "utf8 end");
    EXPECT(elk_text_width(0, "00:00") == elk_text_width(0, "11:11") && elk_text_width(0, "23:59") == elk_text_width(0, "10:01"), "clock digits are tabular (no jitter)");
    EXPECT(elk_text_width(1, "\xe2\x82\xac") == elk_text_width(1, "?"), "code point outside the font renders as '?'");
}

static void bbox(const uint8_t *a, const uint8_t *b, int *x0, int *y0, int *x1, int *y1, long *n) {
    *x0 = ELK_W; *y0 = ELK_H; *x1 = -1; *y1 = -1; *n = 0;
    for (int y = 0; y < ELK_H; y++) for (int x = 0; x < ELK_W; x++) if (a[y * ELK_W + x] != b[y * ELK_W + x]) {
        (*n)++; if (x < *x0) *x0 = x; if (y < *y0) *y0 = y; if (x > *x1) *x1 = x; if (y > *y1) *y1 = y; }
}
static void test_render(void) {
    struct elk_cfg c; elk_default_cfg(&c); snprintf(c.locale, sizeof c.locale, "fr-FR");
    struct elk_info in = {mk(2026, 10, 6, 2, 17, 1), 73, 0}; struct elk_layout L, L2;
    elk_render(A, NULL, &c, &in, &L);
    EXPECT(L.fg == 0, "white background: black text");
    long dark = 0, out = 0;
    for (int y = 0; y < ELK_H; y++) for (int x = 0; x < ELK_W; x++) { int v = A[y * ELK_W + x];
        int inbox = (x >= L.time.x0 && x < L.time.x1 && y >= L.time.y0 && y < L.time.y1) || (x >= L.date.x0 - 2 && x < L.date.x1 + 2 && y >= L.date.y0 - 2 && y < L.date.y1 + 2) ||
                    (x >= L.battery.x0 && x < L.battery.x1 && y >= L.battery.y0 && y < L.battery.y1);
        if (v < 128 && inbox) dark++;
        if (v != 255 && !inbox) out++;
    }
    EXPECT(dark > 20000, "time/date/battery drawn (%ld dark px)", dark);
    EXPECT(out == 0, "nothing drawn outside the time/date/battery boxes (%ld px)", out);
    in.tm.tm_min = 2; elk_render(B, NULL, &c, &in, &L2);
    int x0, y0, x1, y1; long n; bbox(A, B, &x0, &y0, &x1, &y1, &n);
    EXPECT(n > 0 && y0 >= L.time.y0 && y1 < L.time.y1 && x0 >= L.time.x0 && x1 < L.time.x1,
           "17:01 -> 17:02 only changes pixels inside the time box (%ld px in %d,%d-%d,%d)", n, x0, y0, x1, y1);
    EXPECT(n < 8000, "a one-digit minute change touches few pixels (%ld < 8000; REGAL drives only these)", n);
    printf("     minute change 17:01->17:02: %ld changed pixels of %d (%.2f %%)\n", n, ELK_W * ELK_H, 100.0 * n / (ELK_W * ELK_H));
    in.tm.tm_min = 1; in.battery_pct = 72; elk_render(B, NULL, &c, &in, &L2); bbox(A, B, &x0, &y0, &x1, &y1, &n);
    EXPECT(n > 0 && y0 >= L.battery.y0 && y1 < L.battery.y1, "battery 73 -> 72 only changes the battery box");
    in.battery_pct = 73; c.battery = 0; elk_render(B, NULL, &c, &in, &L2); long blank = 0;
    for (int y = 1300; y < ELK_H; y++) for (int x = 0; x < ELK_W; x++) blank += B[y * ELK_W + x] != 255;
    EXPECT(blank == 0, "battery off: nothing at the bottom");
    c.battery = 1; c.bg = ELK_BG_BLACK; elk_render(B, NULL, &c, &in, &L2);
    EXPECT(L2.fg == 255 && B[0] == 0 && B[(ELK_H - 1) * ELK_W + ELK_W - 1] == 0, "black background: white text");
    c.clock = 0; c.bg = ELK_BG_WHITE; elk_render(B, NULL, &c, &in, &L2); long clk = 0;
    for (int y = 150; y < 650; y++) for (int x = 0; x < ELK_W; x++) clk += B[y * ELK_W + x] != 255;
    EXPECT(clk == 0, "clock off: no time/date");
    /* picture background: halo of the opposite colour around the text, contrast colour from the local mean */
    for (int i = 0; i < ELK_W * ELK_H; i++) BG[i] = 200;
    c.clock = 1; c.bg = ELK_BG_IMAGE; elk_render(A, BG, &c, &in, &L);
    long halo = 0, txt = 0;
    for (int y = L.time.y0; y < L.time.y1; y++) for (int x = L.time.x0; x < L.time.x1; x++) { int v = A[y * ELK_W + x]; halo += v == 255; txt += v == 0; }
    EXPECT(L.fg == 0 && halo > 1000 && txt > 5000, "light picture: black text with a white halo (halo %ld, text %ld)", halo, txt);
    for (int i = 0; i < ELK_W * ELK_H; i++) BG[i] = 40;
    elk_render(A, BG, &c, &in, &L); EXPECT(L.fg == 255, "dark picture: white text");
    elk_render(B, NULL, &c, &in, &L2); EXPECT(B[0] == 255, "picture missing: plain white");
    snprintf(c.msg, sizeof c.msg, "Si trouv\xc3\xa9, appelez le 06 00 00 00 00 - merci beaucoup pour votre aide et bonne journ\xc3\xa9" "e");
    c.bg = ELK_BG_WHITE; elk_render(A, NULL, &c, &in, &L);
    EXPECT(L.msg.y1 > L.msg.y0 && L.msg.y1 < L.battery.y0, "owner message box above the battery");
    long m2 = 0; for (int y = L.msg.y0 + 46; y < L.msg.y1; y++) for (int x = 0; x < ELK_W; x++) m2 += A[y * ELK_W + x] < 128;
    EXPECT(m2 > 100, "long owner message wrapped onto a second line");
}

static void test_pgm(void) {
    static uint8_t buf[ELK_W * ELK_H + 64]; long seq;
    int h = snprintf((char *)buf, 64, "P5\n# a6l-lock seq=42\n720 1440\n255\n"); memset(buf + h, 77, ELK_W * ELK_H);
    EXPECT(elk_parse_pgm(buf, (size_t)h + ELK_W * ELK_H, A, &seq) == 0 && seq == 42 && A[0] == 77 && A[ELK_W * ELK_H - 1] == 77, "PGM with seq parsed");
    EXPECT(elk_parse_pgm(buf, (size_t)h + ELK_W * ELK_H - 1, A, &seq) != 0, "truncated PGM (init copy in progress) rejected");
    h = snprintf((char *)buf, 64, "P5 720 1440 255\n"); memset(buf + h, 1, ELK_W * ELK_H);
    EXPECT(elk_parse_pgm(buf, (size_t)h + ELK_W * ELK_H, A, &seq) == 0 && seq == -1, "PGM without seq parsed (seq -1)");
    h = snprintf((char *)buf, 64, "P5\n1440 720\n255\n");
    EXPECT(elk_parse_pgm(buf, (size_t)h + ELK_W * ELK_H, A, &seq) != 0, "landscape PGM rejected");
    EXPECT(elk_parse_pgm((const uint8_t *)"P6\n", 3, A, &seq) != 0, "not P5 rejected");
}

static void test_schedule(void) {
    EXPECT(elk_display_time(30, 60, 1.0) == 0 && elk_next_target(30, 60, 1.0) == 60, "mid-minute: draw this minute, next target the boundary");
    EXPECT(elk_display_time(59.0, 60, 1.0) == 60, "woken lead s before the boundary: draw the coming minute");
    EXPECT(elk_display_time(58.99, 60, 1.0) == 0, "before the lead window: still this minute");
    EXPECT(elk_display_time(59.6, 60, 1.0) == 60 && elk_next_target(59.6, 60, 1.0) == 120, "entry inside the lead window: coming minute, next is the one after");
    EXPECT(elk_next_target(60.5, 60, 1.0) == 120, "after a frame finished past the boundary: next boundary");
    EXPECT(elk_display_time(1791298859.0, 60, 1.0) == 1791298860LL, "epoch-sized times");
    EXPECT(elk_next_target(10, 0, 0) == 60, "period 0 -> 60");
}

static struct elk_act step(struct elk_sm *s, int en, int asleep, int on, int fired, int changed, double t) {
    struct elk_in in = {en, 1, asleep, on, fired, changed, 60, t}; struct elk_act a = elk_step(s, &in); elk_sm_sent(s, &a, t); return a;
}
static void test_sm(void) {
    struct elk_sm s; elk_sm_init(&s); struct elk_act a;
    a = step(&s, 1, 0, 1, 0, 0, 0); EXPECT(!a.draw && !a.restore && a.arm, "awake on the e-ink: timer armed, nothing drawn");
    a = step(&s, 1, 0, 0, 0, 0, 1); EXPECT(!a.arm && !a.draw, "LCD: nothing, timer off");
    a = step(&s, 1, 1, 1, 0, 0, 10); EXPECT(!a.draw && a.hold, "asleep: entry delay, wakelock held");
    a = step(&s, 1, 1, 1, 0, 0, 10.5); EXPECT(!a.draw && a.hold, "asleep 0.5 s: still waiting (round 5: 0.8 s)");
    a = step(&s, 1, 1, 1, 1, 0, 10.6); EXPECT(!a.draw, "round 5: a minute tick does not shortcut the entry delay");
    a = step(&s, 1, 1, 1, 0, 0, 10.35 + 0.5); EXPECT(a.draw && !a.force && s.locked, "asleep 0.85 s: lock picture");
    a = step(&s, 1, 1, 1, 0, 0, 11); EXPECT(!a.draw, "no tick: nothing");
    a = step(&s, 1, 1, 1, 1, 0, 60); EXPECT(a.draw && !a.force, "minute tick: redraw, not forced");
    a = step(&s, 1, 1, 1, 0, 1, 70); EXPECT(a.draw && !a.force, "settings changed: redraw");
    a = step(&s, 1, 1, 1, 1, 0, 10.35 + 3600); EXPECT(a.draw && a.force, "60 min after the lock began: forced ghost cleanup");
    a = step(&s, 1, 1, 1, 1, 0, 10.35 + 3660); EXPECT(a.draw && !a.force, "next tick: normal again");
    a = step(&s, 1, 0, 1, 0, 0, 4000); EXPECT(!a.restore && !s.locked && s.restore_pending, "wake-up on the e-ink: no restore during the wake-up (LCD CRTC comes up too)");
    a = step(&s, 1, 0, 1, 0, 0, 4000.5); EXPECT(!a.restore, "wake-up on the e-ink + 0.5 s: still waiting");
    a = step(&s, 1, 0, 1, 0, 0, 4001.05); EXPECT(a.restore && !s.restore_pending, "wake-up on the e-ink + 1 s: restore");
    a = step(&s, 1, 0, 1, 0, 0, 4002); EXPECT(!a.restore && !a.draw, "restore only once");
    a = step(&s, 1, 1, 1, 1, 0, 5000); EXPECT(!a.draw && a.hold, "a tick while asleep but not yet locked (missed entry): wakelock, entry delay");
    a = step(&s, 1, 1, 1, 0, 0, 5000.9); EXPECT(a.draw && s.locked, "missed entry: drawn 0.8 s later");
    a = step(&s, 0, 1, 1, 0, 0, 5001); EXPECT(a.restore && !s.locked && !a.arm, "disabled while locked (still asleep): restore at once, timer off");
    a = step(&s, 0, 1, 1, 0, 0, 5002); EXPECT(!a.draw && !a.restore, "disabled: nothing");
    struct elk_in in = {1, 0, 1, 1, 0, 0, 60, 6000}; elk_step(&s, &in); in.now = 6000.9; a = elk_step(&s, &in); elk_sm_sent(&s, &a, 6000.9);
    EXPECT(a.draw && !a.arm, "clock off: one picture at entry, no timer");
    in.timer_fired = 1; in.now = 6060; a = elk_step(&s, &in); EXPECT(!a.draw, "clock off: ticks ignored");
    in.timer_fired = 0; in.asleep_eink = 0; in.on_eink = 0; a = elk_step(&s, &in); EXPECT(!a.restore && s.restore_pending, "round 5: woken on the LCD: nothing sent (no e-ink modeset during the LCD wake-up)");
    in.now = 6100; a = elk_step(&s, &in); EXPECT(!a.restore, "LCD in use: the lock picture stays (stock kept its poster)");
    in.on_eink = 1; in.now = 6200; a = elk_step(&s, &in); in.now = 6201.1; a = elk_step(&s, &in); EXPECT(a.restore, "back on the e-ink (awake) 1 s: restore");
    in.asleep_eink = 1; in.now = 6300; elk_step(&s, &in); in.now = 6300.5; in.resumed = 1; a = elk_step(&s, &in);
    EXPECT(!a.draw && a.hold, "round 5: suspended inside the entry window: entry delay restarted");
    in.resumed = 0; in.now = 6301.0; a = elk_step(&s, &in); EXPECT(!a.draw, "restarted window: 0.5 s after the resume: waiting");
    in.now = 6301.35; a = elk_step(&s, &in); EXPECT(a.draw, "restarted window: drawn 0.85 s after the resume");
    elk_sm_abort(&s); EXPECT(!s.locked, "state changed just before the entry frame: aborted, not locked");
    elk_restore_failed(&s, 6400); EXPECT(s.restore_pending, "failed restore: pending again");
    in.asleep_eink = 0; in.on_eink = 1; in.now = 6401; a = elk_step(&s, &in); EXPECT(!a.restore, "failed restore: retried only after 5 s");
    in.now = 6405.1; a = elk_step(&s, &in); EXPECT(a.restore, "failed restore: retried after 5 s");
    struct elk_in c0 = {1, 1, 1, 1, 0, 0, 0, 7000}; elk_step(&s, &c0); c0.now = 7000.9; a = elk_step(&s, &c0); elk_sm_sent(&s, &a, 7000.9);
    c0.timer_fired = 1; c0.now = 7000.9 + 7200; a = elk_step(&s, &c0); EXPECT(a.draw && !a.force, "clean_min 0: never forced");
}
/* eink-round6: lock picture in LCD mode (e-ink not mirroring), settle after display transitions */
static struct elk_act lstep(struct elk_sm *s, int en, int lcd_idle, int settling, int fired, int changed, double t) {
    struct elk_in in = {en, 1, 0, 0, fired, changed, 60, t, 0, lcd_idle, settling}; struct elk_act a = elk_step(s, &in); elk_sm_sent(s, &a, t); return a;
}
static void test_lcd_mode(void) {
    struct elk_sm s; elk_sm_init(&s); struct elk_act a;
    s.restore_pending = 1;	/* as at daemon start */
    a = lstep(&s, 1, 0, 0, 0, 0, 0); EXPECT(!a.draw && !a.arm && !a.hold, "LCD mode, e-ink mirroring the LCD: nothing (timer off)");
    a = lstep(&s, 1, 1, 1, 0, 0, 10); EXPECT(!a.draw && a.arm && a.hold, "boot in LCD mode, e-ink not in use: timer armed, entry pending");
    a = lstep(&s, 1, 1, 0, 0, 0, 11.5); EXPECT(!a.draw, "LCD mode: 1.5 s: still waiting (2 s entry delay)");
    a = lstep(&s, 1, 1, 0, 0, 0, 12.05); EXPECT(a.draw && s.locked && !s.restore_pending, "LCD mode: lock picture 2 s after LCD mode was seen (after boot: no more white e-ink)");
    a = lstep(&s, 1, 1, 0, 1, 0, 60); EXPECT(a.draw, "LCD mode: minute tick redraws the clock");
    a = lstep(&s, 1, 1, 1, 1, 0, 120); EXPECT(!a.draw && s.deferred, "LCD power change < 1.5 s ago: tick deferred");
    a = lstep(&s, 1, 1, 1, 0, 0, 120.5); EXPECT(!a.draw, "  ... still settling: nothing");
    a = lstep(&s, 1, 1, 0, 0, 0, 121.6); EXPECT(a.draw && !s.deferred, "  ... settled: the deferred tick is drawn");
    a = lstep(&s, 1, 1, 0, 0, 0, 122); EXPECT(!a.draw, "  ... once");
    struct elk_in sw = {1, 1, 0, 1, 0, 0, 60, 200, 0, 0, 1}; a = elk_step(&s, &sw); elk_sm_sent(&s, &a, 200);
    EXPECT(!a.draw && !a.restore && !s.locked && s.restore_pending && s.restore_by_mirror, "switched to the e-ink: lock over, no restore during the switch");
    /* eink-round10: LCD-mode lock -> e-ink: the mirror (off during the lock) redraws; no restore of the stale cached page */
    sw.settling = 0; sw.now = 201.05; a = elk_step(&s, &sw); EXPECT(!a.restore && !a.mirror_redraw && s.restore_pending, "round 10: awake on the e-ink 1 s, mirror not on yet: no restore (waits for the mirror)");
    sw.mirror_on = 1; sw.now = 201.2; a = elk_step(&s, &sw); EXPECT(!a.restore && a.mirror_redraw && !s.restore_pending, "round 10: mirror on: restore dropped, its first frame replaces the lock picture");
    sw.now = 210; a = elk_step(&s, &sw); EXPECT(!a.restore && !a.mirror_redraw && !a.draw, "round 10: nothing more");
    elk_sm_init(&s);
    a = lstep(&s, 1, 1, 0, 0, 0, 220); a = lstep(&s, 1, 1, 0, 0, 0, 222.1); EXPECT(a.draw, "LCD mode lock (round 10 fallback)");
    sw.mirror_on = 0; sw.now = 230; a = elk_step(&s, &sw); sw.now = 233.9; a = elk_step(&s, &sw); EXPECT(!a.restore, "round 10: e-ink without the mirror: no restore before 4 s");
    sw.now = 234.05; a = elk_step(&s, &sw); EXPECT(a.restore && !s.restore_pending, "round 10: e-ink without the mirror 4 s: restore (fallback)");
    elk_sm_init(&s);
    a = lstep(&s, 1, 1, 0, 0, 0, 240); a = lstep(&s, 1, 1, 0, 0, 0, 242.1); EXPECT(a.draw, "LCD mode lock (round 10 race)");
    sw.mirror_on = 1; sw.now = 243; a = elk_step(&s, &sw); EXPECT(!a.restore && a.mirror_redraw && !s.restore_pending, "round 10: mirror already on at the lock end: no restore at all");
    /* round 5 lock (asleep on the e-ink, mirror on) woken on the e-ink: restore kept (the paused mirror resends nothing) */
    struct elk_in as = {1, 1, 1, 1, 0, 0, 60, 250, 0, 0, 0, 1}; elk_sm_init(&s); elk_step(&s, &as); as.now = 250.85; a = elk_step(&s, &as); elk_sm_sent(&s, &a, 250.85);
    EXPECT(a.draw && !s.lcd_lock, "asleep on the e-ink, mirror on: lock picture");
    as.asleep_eink = 0; as.now = 260; a = elk_step(&s, &as); as.now = 261.05; a = elk_step(&s, &as);
    EXPECT(a.restore && !a.mirror_redraw, "round 10: lock begun asleep on the e-ink with the mirror on: restore after 1 s as before");
    /* asleep on the e-ink -> woken on the LCD (mirror off: LCD-mode lock) -> back on the e-ink with the mirror: no restore */
    as.asleep_eink = 1; as.now = 270; elk_step(&s, &as); as.now = 270.85; a = elk_step(&s, &as); elk_sm_sent(&s, &a, 270.85);
    struct elk_in lw = {1, 1, 0, 0, 0, 0, 60, 280, 0, 1, 1, 0}; a = elk_step(&s, &lw); EXPECT(s.locked && s.lcd_lock, "round 10: woken on the LCD (mirror off): the lock goes on as an LCD-mode lock");
    as.asleep_eink = 0; as.now = 300; a = elk_step(&s, &as); EXPECT(!a.restore && a.mirror_redraw, "round 10: then the e-ink with the mirror: no restore");
    elk_sm_init(&s);
    a = lstep(&s, 1, 1, 0, 0, 0, 300); a = lstep(&s, 1, 1, 0, 0, 0, 302.1); EXPECT(a.draw, "LCD mode lock again");
    a = lstep(&s, 1, 0, 0, 0, 0, 303); EXPECT(!a.draw && !a.restore && !s.locked && s.restore_pending, "mirror turned on in LCD mode: lock over, the mirror redraws (no restore)");
    { struct elk_in m = {1, 1, 0, 0, 0, 0, 60, 303.3, 0, 0, 0, 1}; a = elk_step(&s, &m); EXPECT(!a.restore && a.mirror_redraw && !s.restore_pending, "round 10: ... and the pending restore is dropped once mirror mode is seen"); }
    elk_sm_init(&s);
    a = lstep(&s, 1, 1, 0, 0, 0, 400); a = lstep(&s, 1, 1, 0, 0, 0, 402.1); EXPECT(a.draw, "LCD mode lock");
    a = lstep(&s, 0, 1, 0, 0, 0, 403); EXPECT(a.restore && !s.locked && !a.arm, "lock disabled in LCD mode: 'lock restore off' at once, timer off");
    elk_sm_init(&s);
    a = lstep(&s, 1, 1, 1, 0, 0, 500); a = lstep(&s, 1, 1, 1, 0, 0, 502.5); EXPECT(!a.draw && a.hold, "LCD mode entry during an LCD power change: waits");
    a = lstep(&s, 1, 1, 0, 0, 0, 502.6); EXPECT(a.draw, "  ... drawn once settled");
    struct elk_in e = {1, 1, 1, 1, 0, 0, 60, 600, 0, 0, 1}; elk_sm_init(&s); elk_step(&s, &e); e.now = 600.85; a = elk_step(&s, &e);
    EXPECT(a.draw, "asleep on the e-ink: entry 0.8 s after, not delayed by the settle (round-5 timing kept)");
}

static void test_wallclock(void) {	/* eink-round10 */
    double now = 1791393900.0 + 20.5;	/* hh:mm:20.5 */
    long long shown = elk_display_time(now, 60, 1.0);
    EXPECT(!elk_wallclock_redraw(shown, now + 6, 60, 1.0), "round 10: cancel at a resume in the same minute: no redraw");
    EXPECT(!elk_wallclock_redraw(shown, now + 38.4, 60, 1.0), "round 10: cancel 0.1 s before the lead: no redraw (the tick comes)");
    EXPECT(elk_wallclock_redraw(shown, now + 38.6, 60, 1.0), "round 10: cancel at the tick (alarm expired during the resume): redraw");
    EXPECT(elk_wallclock_redraw(shown, now - 3600, 60, 1.0), "round 10: time set back an hour: redraw");
    EXPECT(elk_wallclock_redraw(shown, now + 120, 60, 1.0), "round 10: time set 2 min ahead: redraw");
    EXPECT(elk_wallclock_redraw(-1, now, 60, 1.0), "round 10: nothing drawn yet: redraw");
}

static void test_rtc_fallback(void) {
    int on = 0, s = ELK_RTC_UNKNOWN;
    s = elk_rtc_learn(s, &on, 0.4, 20); EXPECT(s == ELK_RTC_UNKNOWN, "one on-time tick: still unknown");
    s = elk_rtc_learn(s, &on, 0.3, 20); EXPECT(s == ELK_RTC_OK, "two on-time ticks: RTC wake OK");
    s = elk_rtc_learn(s, &on, 312.0, 20); EXPECT(s == ELK_RTC_LATE && on == 0, "tick 5 min late (ran at a natural wake): LATE");
    s = elk_rtc_learn(s, &on, 0.5, 20); EXPECT(s == ELK_RTC_LATE, "one on-time tick does not clear LATE");
    s = elk_rtc_learn(s, &on, 0.5, 20); EXPECT(s == ELK_RTC_OK, "two on-time ticks clear LATE");
    EXPECT(!elk_stale_variant("auto", ELK_RTC_UNKNOWN) && !elk_stale_variant("", ELK_RTC_OK) && elk_stale_variant("auto", ELK_RTC_LATE), "auto: updated variant only when LATE");
    EXPECT(!elk_stale_variant("live", ELK_RTC_LATE) && elk_stale_variant("updated", ELK_RTC_OK), "live / updated force the variant");
    char u[64]; elk_format_updated(u, sizeof u, "17:01", "fr-FR"); EXPECT(!strcmp(u, "Mis \xc3\xa0 jour \xc3\xa0 17:01"), "fr updated (%s)", u);
    elk_format_updated(u, sizeof u, "5:01 PM", "en-US"); EXPECT(!strcmp(u, "Updated 5:01 PM"), "en updated (%s)", u);
    struct elk_cfg c; elk_default_cfg(&c); snprintf(c.locale, sizeof c.locale, "fr-FR"); c.stale = 1;
    struct elk_info in = {mk(2026, 10, 6, 2, 17, 1), 73, 0}; struct elk_layout L;
    elk_render(A, NULL, &c, &in, &L);
    long big = 0; for (int y = 300; y < 410; y++) for (int x = 60; x < 660; x++) big += A[y * ELK_W + x] < 128;
    EXPECT(big == 0, "updated variant: no big digits (%ld dark px in the clock area)", big);
    long small = 0; for (int y = L.time.y0; y < L.time.y1; y++) for (int x = L.time.x0; x < L.time.x1; x++) small += A[y * ELK_W + x] < 128;
    EXPECT(small > 300 && L.date.y1 < L.time.y0, "updated variant: date, then the small 'updated' line (%ld px)", small);
}

int main(void) {
    test_format(); test_render(); test_pgm(); test_schedule(); test_sm(); test_lcd_mode(); test_wallclock(); test_rtc_fallback();
    printf("%d checks, %d failures\n", checks, fails);
    printf("%s\n", fails ? "EINKLOCK_TESTS_FAIL" : "EINKLOCK_TESTS_PASS");
    return fails ? 1 : 0;
}
