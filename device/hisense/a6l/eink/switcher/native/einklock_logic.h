// SPDX-License-Identifier: Apache-2.0
/* einklock_logic.h — pure (hardware-free, host unit-tested) logic of a6l_einklock, the e-ink lock screen of the Hisense
 * A6L (eink-lockscreen 6 Oct 2026; firmware/extracted/eink-lockscreen-20261006/README.md).
 *
 * Stock (Vision_SystemUI com.eink.keyguard, decompiled): when both screens are off, EinkKeyguardController renders its
 * keyguard view (clock, date, battery, wallpaper) to a bitmap and posts it to the e-ink in mode 3 (REGAL, "reading");
 * WakeLockEngine arms AlarmManager.setExactAndAllowWhileIdle(ELAPSED_REALTIME_WAKEUP, next minute boundary) and, on the
 * alarm, holds an 800 ms partial wakelock while the clock is redrawn and re-posted, then re-arms. Here the same cadence
 * is done natively: a CLOCK_REALTIME_ALARM timerfd (works in Doze), a software renderer, a6l_epdd "lockframe" (REGAL,
 * only changed pixels are driven), e-ink CRTC switched off right after each frame.
 *
 * Everything here is deterministic: rendering into a caller-owned 720x1440 8-bit grey canvas, time/date formatting,
 * the minute schedule and the lock state machine. No I/O, no clocks, no allocation. */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#define ELK_W 720
#define ELK_H 1440

enum elk_bg { ELK_BG_WHITE = 0, ELK_BG_BLACK = 1, ELK_BG_IMAGE = 2 };

struct elk_cfg {
    int enabled;		/* persist.sys.a6l.eink.lock (default 1) */
    int clock;			/* persist.sys.a6l.eink.lock_clock (default 1): 0 = picture only, no periodic wake-ups */
    int battery;		/* persist.sys.a6l.eink.lock_battery (default 1) */
    int use24;			/* persist.sys.a6l.eink.lock_24h: 1, 0, or -1 = locale default (the app mirrors Android's setting) */
    int clean_min;		/* persist.sys.a6l.eink.lock_clean_min (default 60, 0 = never): forced REGAL ghost cleanup */
    int bg;			/* enum elk_bg (persist.sys.a6l.eink.lock_bg: white | black | image | lcd -> IMAGE) */
    int stale;			/* render the "updated HH:MM" variant instead of the big live clock (see elk_rtc_learn) */
    char locale[24];		/* persist.sys.locale, e.g. "fr-FR" */
    char msg[96];		/* persist.sys.a6l.eink.lock_msg (UTF-8, optional owner line) */
};
void elk_default_cfg(struct elk_cfg *c);
int elk_parse_bg(const char *v);			/* "black" -> BLACK, "image"/"lcd" -> IMAGE, else WHITE */
int elk_cfg_equal(const struct elk_cfg *a, const struct elk_cfg *b);

/* ---------------- formatting ---------------- */
int elk_locale_24h_default(const char *locale);	/* en-US/en-CA/en-AU/es-US... -> 0, everything else -> 1 */
/* "09:41" / "9:41" (+ ampm "AM"/"PM", empty in 24 h) */
void elk_format_time(char *out, size_t n, char *ampm, size_t an, const struct tm *tm, int use24);
/* localized long date, UTF-8 (en, fr, de, es, it, pt; others: ISO 2026-10-06) */
void elk_format_date(char *out, size_t n, const struct tm *tm, const char *locale);
/* "Updated 17:01" / "Mis à jour à 17:01" ... (the RTC-unsafe variant: when minute wake-ups do not happen) */
void elk_format_updated(char *out, size_t n, const char *time, const char *locale);
/* "85%" (en) / "85 %" (fr, de: NBSP), "" when pct < 0 */
void elk_format_battery(char *out, size_t n, int pct, const char *locale);

/* ---------------- rendering ---------------- */
struct elk_info { struct tm tm; int battery_pct; int charging; };
struct elk_box { int x0, y0, x1, y1; };		/* inclusive-exclusive */
struct elk_layout { struct elk_box time, date, battery, msg; int fg; };
/* canvas: ELK_W*ELK_H bytes; bg: same size (used when c->bg == ELK_BG_IMAGE, NULL -> white) */
void elk_render(uint8_t *canvas, const uint8_t *bg, const struct elk_cfg *c, const struct elk_info *in, struct elk_layout *lay);
/* text width in px of UTF-8 s in face 0 = clock, 1 = text, 2 = small (unknown code points render as '?') */
int elk_text_width(int face, const char *s);
size_t elk_utf8_next(const char *s, uint32_t *cp);	/* bytes consumed (0 at end); invalid byte -> U+FFFD, 1 byte */

/* ---------------- background picture (written by the app, copied by init) ---------------- */
/* "P5\n# a6l-lock seq=N\n720 1440\n255\n" + 720*1440 bytes. Returns 0 and fills out (+ *seq, -1 if absent), else -1. */
int elk_parse_pgm(const uint8_t *buf, size_t n, uint8_t *out, long *seq);

/* ---------------- minute schedule ---------------- */
/* The panel shows the new minute ~1.4 s after a cold lock frame starts (bring-up 0.4 + generation 0.35 + rails/lead 0.18 +
 * REGAL 0.46), so the frame for minute M is started lead_s before M. elk_display_time(): the minute to draw when drawing
 * at `now` (realtime seconds); elk_next_target(): the next minute boundary after the displayed one (wake = target - lead). */
long long elk_display_time(double now, int period_s, double lead_s);
long long elk_next_target(double now, int period_s, double lead_s);

/* ---------------- RTC wake check (fallback when the minute alarm cannot wake the suspended system) ----------------
 * The picture stays on the e-paper while the system sleeps, so a big HH:MM that is not updated would silently lie.
 * Every tick taken while locked reports how late it ran (realtime now - planned wake). A tick > late_s late means the
 * alarm did not wake the system (it ran at the next natural wake-up): state ELK_RTC_LATE; 2 consecutive on-time ticks:
 * ELK_RTC_OK. Mode (persist.sys.a6l.eink.lock_clock_mode): "live" = always the big clock, "updated" = always the
 * "Updated HH:MM" variant (time small, refreshed at every natural wake-up), "auto" (default) = live unless LATE.
 * The learned state is kept in persist.vendor.eink.lock_rtc (ok|late) across reboots. */
enum { ELK_RTC_UNKNOWN = -1, ELK_RTC_LATE = 0, ELK_RTC_OK = 1 };
int elk_rtc_learn(int state, int *ontime, double lateness_s, double late_s);
int elk_stale_variant(const char *mode, int rtc_state);	/* 1 = draw the "updated HH:MM" variant */

/* ---------------- lock state machine ---------------- */
struct elk_sm {
    int locked;			/* a lock picture is (being) shown for the current sleep */
    int drew;			/* a lockframe was sent since the lock began -> "lock restore" when it ends */
    double asleep_since;	/* first time seen asleep on the e-ink (entry delay), 0 = not asleep */
    double last_clean;		/* time of the lock entry or of the last forced (ghost-cleaning) frame */
    int frames;			/* lock frames since the lock began */
};
struct elk_in {
    int enabled, clock;
    int asleep_eink;		/* vendor.dualux.state == eink-asleep */
    int on_eink;		/* vendor.dualux.state == eink or eink-asleep */
    int timer_fired;		/* minute tick (or wall clock changed) */
    int changed;		/* settings / background / locale changed */
    int clean_min;
    double now;			/* monotonic seconds */
};
struct elk_act {
    int draw, force;		/* send a lockframe (force = forced REGAL ghost cleanup, whole panel) */
    int restore;		/* send "lock restore" (the lock ended: the mirror's picture comes back if nothing replaced it) */
    int arm;			/* keep the minute timer armed */
};
#define ELK_ENTRY_DELAY_S 0.3	/* let the mirror's in-flight frame / "power off" go first */
void elk_sm_init(struct elk_sm *s);
struct elk_act elk_step(struct elk_sm *s, const struct elk_in *in);
void elk_sm_sent(struct elk_sm *s, const struct elk_act *a, double now);	/* a lockframe was sent (OK or not) */
